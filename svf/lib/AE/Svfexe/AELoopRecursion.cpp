//===- AELoopRecursion.cpp -- Loop / recursion handling for AE ---------//
//
//                     SVF: Static Value-Flow Analysis
//
// Copyright (C) <2013->  <Yulei Sui>
//

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Affero General Public License for more details.

// You should have received a copy of the GNU Affero General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
//===----------------------------------------------------------------------===//
//
// Loop and recursion handling factored out of AbstractInterpretation.cpp.
// Contains:
//   * The widen/narrow fixpoint driver (handleLoopOrRecursion)
//   * Recursion-specific helpers (isRecursiveFun, isRecursiveCallSite,
//     skipRecursiveCall, skipRecursionWithTop, shouldApplyNarrowing)
//

#include "AE/Svfexe/AEWTO.h"
#include "AE/Svfexe/AbstractInterpretation.h"
#include "SVFIR/SVFIR.h"
#include "Util/Options.h"
#include "WPA/Andersen.h"
#include <cstdlib>
#include <iostream>

using namespace SVF;
using namespace SVFUtil;

// =====================================================================
//  Recursion helpers
// =====================================================================

/// Check if a function is recursive (part of a call graph SCC)
bool AbstractInterpretation::isRecursiveFun(const FunObjVar* fun)
{
    return preAnalysis->getPointerAnalysis()->isInRecursion(fun);
}

bool AbstractInterpretation::recursiveTopSummary(const CallICFGNode* call) const
{
    if (!call || Options::HandleRecur() != TOP)
        return false;
    auto* pointerAnalysis = preAnalysis->getPointerAnalysis();
    const auto summarized = [&](const FunObjVar* function) {
        return function && pointerAnalysis->isInRecursion(function) &&
               (!call->getCaller() ||
                !pointerAnalysis->inSameCallGraphSCC(call->getCaller(), function));
    };
    if (const FunObjVar* direct = call->getCalledFunction())
        return summarized(direct);
    if (callGraph->hasIndCSCallees(call))
        for (const FunObjVar* function : callGraph->getIndCSCallees(call))
            if (summarized(function))
                return true;
    return false;
}

AbstractInterpretation::RecursiveMod AbstractInterpretation::recursiveTopMod(
    const CallICFGNode* callNode) const
{
    RecursiveMod effect;
    std::vector<const FunObjVar*> pending;
    std::set<const FunObjVar*> visited;
    const auto appendCallees = [&](const CallICFGNode* call) {
        if (const FunObjVar* direct = call->getCalledFunction())
            pending.push_back(direct);
        else
        {
            const SVFVar* pointer = call->getIndFunPtr();
            if (!pointer || !callGraph->hasIndCSCallees(call))
            {
                effect.allObjects = true;
                return;
            }
            const auto& targets = preAnalysis->getPointerAnalysis()->getPts(pointer->getId());
            if (targets.empty())
                effect.allObjects = true;
            for (NodeID id : targets)
                if (!SVFUtil::isa<FunObjVar>(svfir->getGNode(id)))
                    effect.allObjects = true;
            const auto& resolved = callGraph->getIndCSCallees(call);
            if (resolved.empty())
                effect.allObjects = true;
            for (const FunObjVar* function : resolved)
                pending.push_back(function);
        }
    };
    if (!callNode)
    {
        effect.allObjects = true;
        return effect;
    }
    appendCallees(callNode);
    while (!pending.empty())
    {
        const FunObjVar* function = pending.back();
        pending.pop_back();
        if (!visited.insert(function).second)
            continue;
        if (SVFUtil::isExtCall(function))
        {
            // No external effect/callback contract: globals, reachable
            // objects and lifetimes must all be treated as unconstrained.
            effect.allObjects = true;
            continue;
        }
        for (const SVFBasicBlock* bb : function->getReachableBBs())
            for (const ICFGNode* node : bb->getICFGNodeList())
            {
                if (const auto* nested = SVFUtil::dyn_cast<CallICFGNode>(node))
                    appendCallees(nested);
                for (const SVFStmt* statement : node->getSVFStmts())
                    if (const auto* store = SVFUtil::dyn_cast<StoreStmt>(statement))
                    {
                        // Unlike call-site state, Andersen has targets for
                        // local pointers whose definitions are never run.
                        const auto& targets = preAnalysis->getPointerAnalysis()->getPts(
                                                  store->getLHSVarID());
                        if (targets.empty())
                            effect.allObjects = true;
                        for (NodeID id : targets)
                        {
                            const auto* object = SVFUtil::dyn_cast<ObjVar>(svfir->getGNode(id));
                            const BaseObjVar* base = object ? svfir->getBaseObject(id) : nullptr;
                            if (!base || base->isBlackHoleObj())
                                effect.allObjects = true;
                            else
                                effect.modifiedBases.set(base->getId());
                        }
                    }
            }
    }
    return effect;
}

/// TOP summarizes effects of the whole callee plus transitive calls, even
/// when the recursive WTO cycle has an already-executed acyclic prefix.
void AbstractInterpretation::skipRecursionWithTop(const CallICFGNode* callNode)
{
    namespace AD = AbstractDomain;
    const RetICFGNode* retNode = callNode->getRetICFGNode();
    const RecursiveMod effect = recursiveTopMod(callNode);
    if (std::getenv("SVF_AE_TRACE_RECURSIVE_TOP"))
        std::cerr << "AE_RECURSIVE_TOP call=" << callNode->getId()
                  << " bases=" << effect.modifiedBases.count()
                  << " all=" << effect.allObjects
                  << " return_successors=" << retNode->getOutEdges().size()
                  << '\n';
    prepareRecursiveHavoc(callNode, effect.modifiedBases, effect.allObjects);
    State before = ensureState(callNode);
    std::vector<AD::Location> locations;
    for (const auto& [location, content] : before.memoryLayout().cells())
    {
        (void)content;
        const ObjVar* object = objectAt(location);
        const BaseObjVar* base = object ? svfir->getBaseObject(object->getId()) : nullptr;
        if (effect.allObjects || (base && effect.modifiedBases.test(base->getId())))
            locations.push_back(location);
    }
    for (AD::Location location : locations)
        updateMemoryValue(location, AD::Interval::top(), AD::AddressSet::top(), callNode);
    if (effect.allObjects)
        ensureState(callNode).lifetimes() = AD::LifetimeDomain::top();
    // MOD is may-write, so retain any previous uninitialized alternative.
    ensureState(callNode).joinWith(before);
    for (const SVFStmt* statement : retNode->getSVFStmts())
        if (const auto* retPE = SVFUtil::dyn_cast<RetPE>(statement))
            updateValue(retPE->getLHSVar(), AD::Interval::top(),
                        retPE->getLHSVar()->isPointer() ? AD::AddressSet::top()
                        : AD::AddressSet::bottom(), callNode);
    // Return out-degree never bypasses the effect.
    copyAbstractState(callNode, retNode);
}

/// Check if caller and callee are in the same CallGraph SCC (i.e. a recursive
/// callsite)
bool AbstractInterpretation::isRecursiveCallSite(const CallICFGNode* callNode,
        const FunObjVar* callee)
{
    const FunObjVar* caller = callNode->getCaller();
    return preAnalysis->getPointerAnalysis()->inSameCallGraphSCC(caller,
            callee);
}

/// Skip recursive callsites (within SCC); entry calls from outside SCC are not
/// skipped
bool AbstractInterpretation::skipRecursiveCall(const CallICFGNode* callNode)
{
    const FunObjVar* callee = getCallee(callNode);
    if (!callee)
        return false;

    // Non-recursive function: never skip, always inline
    if (!isRecursiveFun(callee))
        return false;

    // For recursive functions, skip only recursive callsites (within same SCC).
    // Entry calls (from outside SCC) are not skipped - they are inlined so that
    // handleLoopOrRecursion() can analyze the function body.
    // This applies uniformly to all modes (TOP/WIDEN_ONLY/WIDEN_NARROW).
    return isRecursiveCallSite(callNode, callee);
}

/// Check if narrowing should be applied: always for regular loops,
/// mode-dependent for recursion
bool AbstractInterpretation::shouldApplyNarrowing(const FunObjVar* fun)
{
    // Non-recursive functions (regular loops): always apply narrowing
    if (!isRecursiveFun(fun))
        return true;

    // Recursive functions: WIDEN_NARROW applies narrowing, WIDEN_ONLY does not
    // TOP mode exits early in handleLoopOrRecursion, so should not reach here
    switch (Options::HandleRecur())
    {
    case TOP:
        assert(false && "TOP mode should not reach narrowing phase for "
                        "recursive functions");
        return false;
    case WIDEN_ONLY:
        return false; // Skip narrowing for recursive functions
    case WIDEN_NARROW:
        return true; // Apply narrowing for recursive functions
    default:
        assert(false && "Unknown recursion handling mode");
        return false;
    }
}

std::unique_ptr<AbstractDomain::AbstractDomain> AbstractInterpretation::
cloneCycleHeadState(const ICFGCycleWTO* cycle)
{
    return cloneAbstractState(cycle->head()->getICFGNode());
}

bool AbstractInterpretation::widenCycleState(
    const AbstractDomain::AbstractDomain& previous,
    const AbstractDomain::AbstractDomain& current, const ICFGCycleWTO* cycle)
{
    const State& previousDense = static_cast<const State&>(previous);
    const State& currentDense = static_cast<const State&>(current);
    State next = previousDense;
    next.widenWith(currentDense);
    traceIndexState("widen-previous", cycle->head()->getICFGNode(), previousDense);
    traceIndexState("widen-incoming", cycle->head()->getICFGNode(), currentDense);
    traceIndexState("widen-next", cycle->head()->getICFGNode(), next);
    if (std::getenv("SVF_AE_TRACE_CYCLE_STATE"))
        std::cerr << "AE_WIDEN head=" << cycle->head()->getICFGNode()->getId()
                  << " previous=" << previousDense.numerical().toString()
                  << " current=" << currentDense.numerical().toString()
                  << " next=" << next.numerical().toString() << '\n';
    const bool fixpoint =
        next.isEquivalentTo(previousDense) == AbstractDomain::CheckResult::True;
    const ICFGNode* head = cycle->head()->getICFGNode();
    stateTrace_.insert_or_assign(head, std::move(next));
    return fixpoint;
}

bool AbstractInterpretation::narrowCycleState(
    const AbstractDomain::AbstractDomain& previous,
    const AbstractDomain::AbstractDomain& current, const ICFGCycleWTO* cycle)
{
    const ICFGNode* head = cycle->head()->getICFGNode();
    if (!shouldApplyNarrowing(head->getFun()))
        return true;
    const State& previousDense = static_cast<const State&>(previous);
    State currentDense = static_cast<const State&>(current);
    // Sparse transfers may materialize a new MemorySSA/cycle facet during the
    // descending phase. Enforce narrowing's generic next <= current contract.
    // The normal descending path already satisfies that contract. Avoid
    // rebuilding and closing a relational meet when the lattice check proves
    // that the meet would be exactly currentDense. False and Unknown retain
    // the original conservative meet.
    if (currentDense.isSubsetOf(previousDense) !=
            AbstractDomain::CheckResult::True)
        currentDense.meetWith(previousDense);
    State next = previousDense;
    next.narrowWith(currentDense);
    traceIndexState("narrow-previous", head, previousDense);
    traceIndexState("narrow-incoming", head, currentDense);
    traceIndexState("narrow-next", head, next);
    const bool fixpoint =
        next.isEquivalentTo(previousDense) == AbstractDomain::CheckResult::True;
    if (!fixpoint)
        stateTrace_.insert_or_assign(head, std::move(next));
    return fixpoint;
}

// =====================================================================
//  Cycle / recursion driver
//
//  Handle a WTO cycle (loop or recursive function) using widening /
//  narrowing iteration.  Widening at cycle head ensures termination.
//
//  == What is being widened ==
//  The abstract state at the cycle head node, which includes:
//  - Variable values (intervals) that may change across loop iterations
//  - For example, a loop counter `i` starting at 0 and incrementing
//    each iteration
//
//  == Regular loops (non-recursive functions) ==
//  All modes (TOP/WIDEN_ONLY/WIDEN_NARROW) behave the same for regular
//  loops:
//   1. Widening phase: iterate until the cycle head state stabilizes
//      Example: for(i=0; i<100; i++)  ->  i widens to [0, +inf]
//   2. Narrowing phase: refine the over-approximation from widening
//      Example: [0, +inf] narrows to [0, 100] using loop condition
//
//  == Recursive function cycles ==
//  Behavior depends on Options::HandleRecur():
//
//  - TOP:           skip body entirely, set return + reachable stores
//                   to TOP (most conservative, fastest)
//  - WIDEN_ONLY:    widening only, no narrowing
//                     factorial(5) -> [10000, +inf]
//  - WIDEN_NARROW:  widening + narrowing
//                     factorial(5) -> [10000, 10000]
//
//  == Semi-sparse note ==
//  In semi-sparse mode ValVars live at their def-sites and do not flow
//  through cycle_head's merge.  The cycle helpers in
//  Native sparse implementations gather them into the cycle head
//  snapshot and scatter them back after each widen/narrow step so the
//  fixpoint can observe ValVar growth across iterations.
// =====================================================================

void AbstractInterpretation::handleLoopOrRecursion(const ICFGCycleWTO* cycle,
        const CallICFGNode* caller)
{
    const ICFGNode* cycle_head = cycle->head()->getICFGNode();

    // TOP mode for recursive function cycles: set all stores and return value
    // to TOP
    if (Options::HandleRecur() == TOP && isRecursiveFun(cycle_head->getFun()))
    {
        if (caller)
            skipRecursionWithTop(caller);
        return;
    }

    // Iterate until fixpoint with widening/narrowing on the cycle head.
    bool increasing = true;
    // A stable head is not enough until the body has been replayed from that
    // head. Otherwise the final successor states may still correspond to the
    // previous narrowing iterate and fail the original transfer equations.
    bool narrowingHeadStable = false;
    u32_t widen_delay = Options::WidenDelay();
    for (u32_t cur_iter = 0;; cur_iter++)
    {
        if (cur_iter >= widen_delay)
        {
            // cloneCycleHeadState handles dense (returns trace[cycle_head])
            // and semi-sparse (collects ValVars from def-sites) uniformly.
            std::unique_ptr<AbstractDomain::AbstractDomain> previous =
                cloneCycleHeadState(cycle);

            if (mergeStatesFromPredecessors(cycle_head))
                handleICFGNode(cycle_head);
            std::unique_ptr<AbstractDomain::AbstractDomain> current =
                cloneCycleHeadState(cycle);

            if (increasing)
            {
                const bool stateFixpoint =
                    widenCycleState(*previous, *current, cycle);
                if (stateFixpoint)
                {
                    increasing = false;
                    continue;
                }
            }
            else
            {
                const bool stateFixpoint =
                    narrowCycleState(*previous, *current, cycle);
                if (stateFixpoint)
                {
                    if (narrowingHeadStable)
                        break;
                    narrowingHeadStable = true;
                }
                else
                    narrowingHeadStable = false;
            }
        }
        else
        {
            // Before widen_delay: process cycle head with gated pattern
            if (mergeStatesFromPredecessors(cycle_head))
                handleICFGNode(cycle_head);
        }

        // Process cycle body components (each with gated merge+handle)
        for (const ICFGWTOComp* comp : cycle->getWTOComponents())
        {
            if (const ICFGSingletonWTO* singleton =
                        SVFUtil::dyn_cast<ICFGSingletonWTO>(comp))
            {
                const ICFGNode* node = singleton->getICFGNode();
                if (mergeStatesFromPredecessors(node))
                    handleICFGNode(node);
            }
            else if (const ICFGCycleWTO* subCycle =
                         SVFUtil::dyn_cast<ICFGCycleWTO>(comp))
            {
                if (mergeStatesFromPredecessors(
                            subCycle->head()->getICFGNode()))
                    handleLoopOrRecursion(subCycle, caller);
            }
        }
    }
}
