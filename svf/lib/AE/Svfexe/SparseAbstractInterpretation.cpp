//===- SparseAbstractInterpretation.cpp -- Sparse box/address AE --------===//
//
//                     SVF: Static Value-Flow Analysis
//
// Copyright (C) <2013->  <Yulei Sui>
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU Affero General Public License for more details.
//
// You should have received a copy of the GNU Affero General Public License
// along with this program. If not, see <http://www.gnu.org/licenses/>.
//
// Contributors: Xiao Cheng, Jiawei Wang
//
//===----------------------------------------------------------------------===//

#include "AE/Svfexe/SparseAbstractInterpretation.h"

#include "Graphs/SVFG.h"
#include "MSSA/SVFGBuilder.h"
#include "SVFIR/SVFIR.h"
#include "Util/Options.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <iterator>
#include <optional>
#include <set>

namespace SVF
{

namespace
{

std::vector<SVF::Variable> nonDefaultVariables(
    const SemiSparseAbstractInterpretation::State& state)
{
    // All domain support queries are already sorted and unique. Keep that
    // order without allocating a tree node for every initialized coordinate.
    const auto numbers = state.numerical().supportVariables();
    const auto pointers = state.addresses().nonDefaultVariables();
    std::vector<SVF::Variable> payloads;
    payloads.reserve(numbers.size() + pointers.size());
    std::set_union(numbers.begin(), numbers.end(), pointers.begin(), pointers.end(),
                   std::back_inserter(payloads));
    const auto initialized = state.initializedVariables();
    std::vector<SVF::Variable> variables;
    variables.reserve(payloads.size() + initialized.size());
    std::set_union(payloads.begin(), payloads.end(), initialized.begin(), initialized.end(),
                   std::back_inserter(variables));
    return variables;
}

std::set<SVF::Variable> definedScalarVariables(const ICFGNode* node,
        const SVFIRAdapter& adapter)
{
    std::set<SVF::Variable> result;
    const auto add = [&](const SVFVar* variable)
    {
        if (!variable)
            return;
        const auto* value = SVFUtil::dyn_cast<ValVar>(variable);
        if (value && adapter.contains(*value))
            result.insert(adapter.variable(*value));
    };
    for (const SVFStmt* statement : node->getSVFStmts())
    {
        if (const auto* address = SVFUtil::dyn_cast<AddrStmt>(statement))
            add(address->getLHSVar());
        else if (const auto* binary =
                     SVFUtil::dyn_cast<BinaryOPStmt>(statement))
            add(binary->getRes());
        else if (const auto* compare = SVFUtil::dyn_cast<CmpStmt>(statement))
            add(compare->getRes());
        else if (const auto* load = SVFUtil::dyn_cast<LoadStmt>(statement))
            add(load->getLHSVar());
        else if (const auto* copy = SVFUtil::dyn_cast<CopyStmt>(statement))
            add(copy->getLHSVar());
        else if (const auto* gep = SVFUtil::dyn_cast<GepStmt>(statement))
            add(gep->getLHSVar());
        else if (const auto* select = SVFUtil::dyn_cast<SelectStmt>(statement))
            add(select->getRes());
        else if (const auto* phi = SVFUtil::dyn_cast<PhiStmt>(statement))
            add(phi->getRes());
        else if (const auto* call = SVFUtil::dyn_cast<CallPE>(statement))
            add(call->getRes());
        else if (const auto* ret = SVFUtil::dyn_cast<RetPE>(statement))
            add(ret->getLHSVar());
    }
    // External models bind the actual return directly in the call node's
    // post-state.  Internal calls expose their result at the RetICFGNode via
    // RetPE; importing that future definition at the call boundary would make
    // reconstruction circular.
    if (const auto* call = SVFUtil::dyn_cast<CallICFGNode>(node))
        if (call->getCalledFunction() &&
                SVFUtil::isExtCall(call->getCalledFunction()))
            add(call->getRetICFGNode()->getActualRet());
    if (const auto* returnSite = SVFUtil::dyn_cast<RetICFGNode>(node))
        add(returnSite->getActualRet());
    return result;
}

} // namespace

SemiSparseAbstractInterpretation::SemiSparseAbstractInterpretation()
{
    this->preAnalysis->initCycleValVars();
    initializeScalarAvailability();
}

void SemiSparseAbstractInterpretation::initializeScalarAvailability()
{
    scalarSymbols_.clear();
    scalarVariables_.clear();
    scalarAvailability_.clear();
    auto& symbols = scalarSymbols_;
    auto& variables = scalarVariables_;
    const auto symbol = [&](SVF::Variable variable)
    {
        const auto entry = symbols.emplace(variable, variables.size());
        if (entry.second)
            variables.push_back(variable);
        return entry.first->second;
    };
    const auto encode = [&](const std::set<SVF::Variable>& values)
    {
        NodeBS result;
        for (SVF::Variable variable : values)
            result.set(symbol(variable));
        return result;
    };
    Map<const ICFGNode*, NodeBS> available, definitions;
    std::vector<const ICFGNode*> nodes;
    for (auto iterator = this->icfg->begin(); iterator != this->icfg->end();
            ++iterator)
    {
        nodes.push_back(iterator->second);
        available.try_emplace(iterator->second);
        definitions.emplace(iterator->second, encode(
                                definedScalarVariables(iterator->second, this->adapter_)));
    }
    std::sort(nodes.begin(), nodes.end(),
              [](const ICFGNode* left, const ICFGNode* right)
    {
        return left->getId() < right->getId();
    });

    const ICFGNode* global = this->icfg->getGlobalICFGNode();
    NodeBS globalOut = definitions.at(global);
    // Use the same entry policy as execution, not all syntactic callers.
    // This is static reachability: no guard feasibility is assumed here.
    std::set<const ICFGNode*> reachable{global};
    std::set<const ICFGNode*> entryNodes;
    FIFOWorkList<const FunObjVar*> roots = this->collectProgEntryFuns();
    while (!roots.empty())
    {
        const FunEntryICFGNode* entry =
            this->icfg->getFunEntryICFGNode(roots.pop());
        reachable.insert(entry);
        entryNodes.insert(entry);
        for (const SVFVar* argument : entry->getFormalParms())
        {
            const auto* value = SVFUtil::dyn_cast<ValVar>(argument);
            if (value && this->adapter_.contains(*value))
                globalOut.set(symbol(this->adapter_.variable(*value)));
        }
    }
    available[global] = std::move(globalOut);

    bool reachChanged=true;
    while(reachChanged)
    {
        reachChanged=false;
        for(const ICFGNode* node:nodes)
        {
            if(!reachable.count(node)) continue;
            const auto* summaryCall = SVFUtil::dyn_cast<CallICFGNode>(node);
            if (summaryCall && summaryCall->getCalledFunction() &&
                    this->recursiveTopSummary(summaryCall))
                reachChanged |= reachable.insert(
                                    summaryCall->getRetICFGNode()).second;
            for(const ICFGEdge* edge:node->getOutEdges())
            {
                if(!SVFUtil::isa<IntraCFGEdge>(edge) &&
                        !SVFUtil::isa<CallCFGEdge>(edge) &&
                        !SVFUtil::isa<RetCFGEdge>(edge)) continue;
                // Reaching a shared callee through main does not execute an
                // unvisited caller's return site. Revisit on later discovery
                // of that call site; this fixed point is independent of IDs.
                if(const auto* ret=SVFUtil::dyn_cast<RetCFGEdge>(edge))
                    if(!reachable.count(ret->getCallSite())) continue;
                reachChanged|=reachable.insert(edge->getDstNode()).second;
            }
        }
    }
    for(const ICFGNode* node:nodes)
        if(!reachable.count(node)) available[node].clear();

    // Unvisited equations start at the universe, the identity for intersection.
    // Keep it implicit until a finite boundary reaches the node.
    std::set<const ICFGNode*> pending = reachable;
    pending.erase(global);
    Map<const ICFGNode*, std::vector<const ICFGNode*>> dependents;
    FIFOWorkList<const ICFGNode*> worklist;
    for (const ICFGNode* node : nodes)
    {
        if (node == global || !reachable.count(node))
            continue;
        worklist.push(node);
        for (const ICFGEdge* edge : node->getInEdges())
        {
            const auto* ret = SVFUtil::dyn_cast<RetCFGEdge>(edge);
            const ICFGNode* source = ret
                                     ? static_cast<const ICFGNode*>(ret->getCallSite())
                                     : edge->getSrcNode();
            dependents[source].push_back(node);
        }
        if (const auto* ret = SVFUtil::dyn_cast<RetICFGNode>(node))
            dependents[ret->getCallICFGNode()].push_back(node);
    }
    while (!worklist.empty())
    {
        const ICFGNode* node = worklist.pop();
        bool first = true;
        bool hasPendingInput = false;
        NodeBS incoming;
        const auto* returnSite = SVFUtil::dyn_cast<RetICFGNode>(node);
        const CallICFGNode* summaryCall = returnSite
                                          ? returnSite->getCallICFGNode() : nullptr;
        const bool summaryReturn = summaryCall &&
                                   summaryCall->getCalledFunction() &&
                                   this->recursiveTopSummary(summaryCall);
        // Execution also starts selected roots directly from the global
        // state (including no-main entry SCCs without an explicit edge).
        if (entryNodes.count(node))
        {
            incoming=available.at(global);
            first=false;
        }
        if (summaryReturn)
        {
            if (pending.count(summaryCall))
                hasPendingInput = true;
            else
            {
                incoming = available.at(summaryCall);
                first = false;
            }
        }
        for (const ICFGEdge* edge : node->getInEdges())
        {
            // TOP binds its own result; the unexecuted callee's formal
            // return ghost is not an input to the synthetic summary edge.
            if (summaryReturn)
                break;
            if (!SVFUtil::isa<IntraCFGEdge>(edge) &&
                    !SVFUtil::isa<CallCFGEdge>(edge) &&
                    !SVFUtil::isa<RetCFGEdge>(edge))
                continue;
            if (!reachable.count(edge->getSrcNode())) continue;
            if (const auto* ret=SVFUtil::dyn_cast<RetCFGEdge>(edge))
                if (!reachable.count(ret->getCallSite())) continue;
            const auto predecessor =
                available.find(edge->getSrcNode());
            if (predecessor == available.end())
                continue;
            const auto* returnEdge = SVFUtil::dyn_cast<RetCFGEdge>(edge);
            const ICFGNode* source = returnEdge
                                     ? static_cast<const ICFGNode*>(returnEdge->getCallSite())
                                     : edge->getSrcNode();
            if (pending.count(source))
            {
                hasPendingInput = true;
                continue;
            }
            NodeBS edgeAvailable =
                available.at(source);
            // Return transfer retains the callee's formal-return ghosts
            // needed to bind RetPE relations and restores only the caller
            // SSA frame. Other callee locals are out of scope here and
            // must not leak into the caller's global scalar carrier.
            if (returnEdge)
            {
                for (const SVFStmt* statement : node->getSVFStmts())
                {
                    const auto* binding =
                        SVFUtil::dyn_cast<RetPE>(statement);
                    const auto* formal = binding
                                         ? SVFUtil::dyn_cast<ValVar>(binding->getRHSVar())
                                         : nullptr;
                    if (formal && this->adapter_.contains(*formal))
                        edgeAvailable.set(
                            symbol(this->adapter_.variable(*formal)));
                }
            }
            if (first)
            {
                incoming = std::move(edgeAvailable);
                first = false;
            }
            else
            {
                incoming &= edgeAvailable;
            }
        }
        if (first && hasPendingInput)
            continue;
        incoming |= definitions.at(node);
        if (pending.erase(node) || incoming != available.at(node))
        {
            available[node] = std::move(incoming);
            for (const ICFGNode* dependent : dependents[node])
                worklist.push(dependent);
        }
    }
    assert(pending.empty() && "Reachable scalar availability has no finite boundary");
    scalarAvailability_ = std::move(available);
}

bool SemiSparseAbstractInterpretation::isScalarAvailable(
    const ICFGNode* node, SVF::Variable variable) const
{
    const auto state = scalarAvailability_.find(node);
    const auto symbol = scalarSymbols_.find(variable);
    return state != scalarAvailability_.end() &&
           symbol != scalarSymbols_.end() && state->second.test(symbol->second);
}

std::vector<SVF::Variable>
SemiSparseAbstractInterpretation::availableScalarVariables(
    const ICFGNode* node) const
{
    std::vector<SVF::Variable> variables;
    const auto state = scalarAvailability_.find(node);
    if (state != scalarAvailability_.end())
        for (NodeID symbol : state->second)
            variables.push_back(scalarVariables_.at(symbol));
    std::sort(variables.begin(), variables.end());
    return variables;
}

SemiSparseAbstractInterpretation::State
SemiSparseAbstractInterpretation::flowState(bool bottom) const
{
    return State(this->makeNumericalDomain(bottom),
                 this->adapter_.memoryLayout(), true);
}

SemiSparseAbstractInterpretation::State&
SemiSparseAbstractInterpretation::scalarState()
{
    if (!scalarState_)
        scalarState_.emplace(flowState());
    return *scalarState_;
}

const SemiSparseAbstractInterpretation::State*
SemiSparseAbstractInterpretation::findScalarState() const
{
    return scalarState_ ? &*scalarState_ : nullptr;
}

SemiSparseAbstractInterpretation::State&
SemiSparseAbstractInterpretation::scalarTransferState(const ICFGNode*)
{
    return scalarState();
}

SemiSparseAbstractInterpretation::State
SemiSparseAbstractInterpretation::phiAlternativeState(
    const ICFGNode* predecessor)
{
    State alternative = this->topState();
    if (predecessor)
    {
        std::vector<SVF::Variable> seeds;
        const auto available = scalarAvailability_.find(predecessor);
        if (available != scalarAvailability_.end())
            seeds = availableScalarVariables(predecessor);
        {
            for (SVF::Variable variable : seeds)
                alternative.assignValueFrom(variable, scalarState(), variable);
        }
        if (this->hasAbsState(predecessor))
            alternative.numerical().meetWith(
                           this->state(predecessor).numerical());
        // Semi-sparse branch predicates live in refinementTrace_ because
        // scalar SSA coordinates are removed from persistent ICFG states.
        // A phi alternative must include the predicate that selected its
        // predecessor; otherwise joining `x` from x<=y with `y` from x>y
        // loses the derived min/max relation and retains only unary hulls.
        const auto refinement = refinementTrace_.find(predecessor);
        // Replaying branch relations into every loop phi makes the same
        // octagonal closure participate in each widening iteration.  Cyclic
        // functions already reconstruct loop values through WTO
        // widen/scatter; keep this supplementary path-sensitive phi recovery
        // for acyclic SSA joins, where it is stable and directly useful.
        if (refinement != refinementTrace_.end() &&
                !this->preAnalysis->functionHasCycle(
                    predecessor->getFun()))
            applyScalarRefinement(alternative, refinement->second);
    }
    return alternative;
}

void SemiSparseAbstractInterpretation::recordMergedReturnSummary(
    const RetICFGNode* returnSite)
{
    if (!returnSite)
        return;
    const auto* target =
        SVFUtil::dyn_cast<ValVar>(returnSite->getActualRet());
    if (!target || !this->adapter_.contains(*target))
        return;
    const SVF::Variable targetVariable = this->adapter_.variable(*target);
    {
        // The edge merge computes the return value in the transient flow
        // state.  Publish its scalar facet before finalizeAbstractState drops
        // active ValVars from that state.
        scalarState().assignValueFrom(
            targetVariable, this->state(returnSite), targetVariable);
        return;
    }
}

const SVF::AbstractDomain* SemiSparseAbstractInterpretation::
getScalarAbstractState() const
{
    return findScalarState();
}

void SemiSparseAbstractInterpretation::handleGlobalNode()
{
    Base::handleGlobalNode();
    finalizeAbstractState(this->icfg->getGlobalICFGNode());
}

SVF::Interval SemiSparseAbstractInterpretation::getInterval(
    const ValVar* value, const ICFGNode* node)
{

    const SVF::Interval result = getDefinedInterval(value, node);
    return result;
}

SVF::Interval SemiSparseAbstractInterpretation::getDefinedInterval(
    const ValVar* value, const ICFGNode* node)
{

    if (const auto* integer = SVFUtil::dyn_cast<ConstIntValVar>(value))
        return SVF::Interval::singleton(SVF::Rational(integer->getSExtValue()));
    if (const auto* floating = SVFUtil::dyn_cast<ConstFPValVar>(value))
        return SVFIRAdapter::floatingConstant(floating->getFPValue());
    if (!value)
        return SVF::Interval::top();
    if (value->getId() == this->svfir->getBlkPtr() ||
            SVFUtil::isa<BlackHoleValVar>(value))
        return Base::getDefinedInterval(value, node);
    if (!this->adapter_.contains(*value))
        return SVF::Interval::top();

    // Synthetic copy helpers have no def-site. Original's sparse reader
    // resolves them at the global node, where their numerical value is Top,
    // rather than reading the value written at the generated load's node.
    // Keep this policy for subsequent generated stores as well as queries.
    if (SVFUtil::isa<DummyValVar>(value) && !value->getICFGNode())
        return SVF::Interval::top();

    const SVF::Variable variable = this->adapter_.variable(*value);
    const auto available = scalarAvailability_.find(node);
    if (node && (available == scalarAvailability_.end() ||
                 !isScalarAvailable(node, variable)))
        return value->isPointer() ? SVF::Interval::bottom()
               : SVF::Interval::top();
    // A forward reference in a global initializer has not executed its
    // AddrStmt yet. Match the unresolved-symbol policy of the dense reader.
    if ((SVFUtil::isa<FunValVar>(value) || SVFUtil::isa<GlobalValVar>(value)) &&
            !scalarState().hasValue(variable))
        return SVF::Interval::top();
    SVF::Interval result;
    const auto definition = scalarDefinitions_.find(variable);
    if (definition != scalarDefinitions_.end())
        result = definition->second.interval(variable);
    else
        result = scalarState().interval(variable);
    // Conditional-edge refinement is intentionally local to the ICFG state.
    // Read it in addition to the module-wide scalar carrier so transfer
    // functions observe path constraints without copying all SSA values into
    // every program point.
    if (node && this->hasAbsState(node))
    {
        const State& local = this->state(node);
        const SVF::Interval refined = local.numerical().bound(variable);
        if (!refined.isTop())
        {
            if (result.isBottom())
                result = refined;
            else
                result.meetWith(refined);
        }
    }

    return result;
}

SVF::AddressSet SemiSparseAbstractInterpretation::getAddressSet(
    const ValVar* value, const ICFGNode* node)
{

    (void)node;
    if (!value)
        return SVF::AddressSet::top();
    if (value->getId() == IRGraph::NullPtr ||
            SVFUtil::isa<ConstNullPtrValVar>(value))
        return SVF::AddressSet::singleton(SVF::Location::null());
    if (value->getId() == this->svfir->getBlkPtr() ||
            SVFUtil::isa<BlackHoleValVar>(value))
        return this->blackHoleAddressSet();
    if (SVFUtil::isa<DummyValVar>(value) && !value->getICFGNode())
        return SVF::AddressSet::bottom();
    if (!this->adapter_.contains(*value))
        return SVF::AddressSet::bottom();
    const SVF::Variable variable = this->adapter_.variable(*value);
    const auto available = scalarAvailability_.find(node);
    if (node && (available == scalarAvailability_.end() ||
                 !isScalarAvailable(node, variable)))
        return value->isPointer() ? SVF::AddressSet::top()
               : SVF::AddressSet::bottom();
    const auto definition = scalarDefinitions_.find(variable);
    return definition != scalarDefinitions_.end()
           ? definition->second.addressSet(variable)
           : scalarState().addressSet(variable);
}

bool SemiSparseAbstractInterpretation::hasAbsValue(
    const ValVar* value, const ICFGNode* node) const
{

    (void)node;
    if (SVFUtil::isa<ConstIntValVar>(value) ||
            SVFUtil::isa<ConstFPValVar>(value))
        return true;
    return value && this->adapter_.contains(*value);
}

bool SemiSparseAbstractInterpretation::numericalValueMayBeUninitialized(
    const ValVar* value, const ICFGNode* node) const
{

    if (!value || !this->adapter_.contains(*value))
        return true;
    const SVF::Variable variable = this->adapter_.variable(*value);
    const auto definition = scalarDefinitions_.find(variable);
    if (definition != scalarDefinitions_.end())
        return definition->second.numericalMayBeUninitialized(variable);
    return !scalarState_ ||
           scalarState_->numericalMayBeUninitialized(variable);
}

void SemiSparseAbstractInterpretation::updateValue(
    const ValVar* value, const SVF::Interval& interval,
    const SVF::AddressSet& addresses, const ICFGNode* node)
{
    if (value && this->adapter_.contains(*value))
    {
        const SVF::Variable variable = this->adapter_.variable(*value);
        this->assignValue(scalarState(), variable, interval, addresses);
        State summary = this->topState();
        this->assignValue(summary, variable, interval, addresses);
        {
            const auto previous = scalarDefinitions_.find(variable);
            if (previous != scalarDefinitions_.end())
                summary.joinWith(previous->second);
        }
        scalarDefinitions_.insert_or_assign(variable, std::move(summary));
    }
}

void SemiSparseAbstractInterpretation::addUninitializedNumericalAlternative(
    const ValVar* value, const ICFGNode* node)
{
    (void)node;
    if (value && this->adapter_.contains(*value))
    {
        const SVF::Variable variable = this->adapter_.variable(*value);
        scalarState().addUninitializedNumericalAlternative(variable);
        const auto summary = scalarDefinitions_.find(variable);
        if (summary != scalarDefinitions_.end())
            summary->second.addUninitializedNumericalAlternative(variable);
    }
}

void SemiSparseAbstractInterpretation::copyAbstractState(
    const ICFGNode* source, const ICFGNode* destination)
{
    this->stateTrace_.insert_or_assign(destination, this->state(source));
}

void SemiSparseAbstractInterpretation::resetAbstractState(
    const ICFGNode* node)
{
    this->stateTrace_.insert_or_assign(node, flowState());
}

void SemiSparseAbstractInterpretation::finalizeAbstractState(
    const ICFGNode* node)
{
    State& denseState = this->ensureState(node);
    forgetActiveScalarValues(denseState);
}

void SemiSparseAbstractInterpretation::restoreCallerFrame(
    State& denseState, const RetICFGNode* returnSite,
    const State& callerState,
    const std::set<SVF::Variable>& callerScalars) const
{
    if (isSharedCalleeReturn(returnSite))
    {
        const bool nonReentrant = preservesCallerFrame(returnSite);
        const FunObjVar* caller = returnSite->getCallICFGNode()->getCaller();
        for (SVF::Variable variable : callerScalars)
        {
            const ValVar* value = this->adapter_.value(variable);
            if (nonReentrant && value && value->getFunction() == caller)
            {
                // Only this activation's available SSA coordinates are
                // immutable along every admitted callee path. Preserve
                // their exit relations with *current* memory, and intersect
                // with the call-site facts. Never restore call-site memory.
                this->constrainInterval(
                    denseState, variable, callerState.interval(variable));
                denseState.restoreMissingNumericalInitializationFrom(
                    callerState, variable);
                denseState.restoreMissingAddressFrom(callerState, variable);
            }
            else
            {
                // Recursion, unknown effects, or a coordinate owned by
                // another function retain the old frame replacement path.
                this->forgetValue(denseState, variable);
                denseState.assignValueFrom(variable, callerState, variable);
            }
        }
        if ((!callerScalars.empty()) || (callerState.isBottom()))
        {
            State projectedCaller = callerState;
            const std::vector<SVF::Variable> callerVariables(
                callerScalars.begin(), callerScalars.end());
            projectedCaller.numerical().project(callerVariables);
            denseState.numerical().meetWith(projectedCaller.numerical());
        }
    }
    else
    {
        for (SVF::Variable variable : callerScalars)
        {
            // The relational flow state can contain a sound relation for a
            // caller SSA coordinate while deliberately omitting that
            // coordinate's initialization carrier.  A strong coordinate
            // assignment would then erase all relations through it.  Caller
            // SSA values are unchanged by a single-call-site callee, so meet
            // their unary bounds and restore only missing product facets.
            this->constrainInterval(
                denseState, variable, callerState.interval(variable));
            denseState.restoreMissingNumericalInitializationFrom(
                callerState, variable);
            denseState.restoreMissingAddressFrom(callerState, variable);
        }
    }
    restoreCallerFrameAfterSharedCallee(denseState, returnSite, &callerState);
}

void SemiSparseAbstractInterpretation::forgetActiveScalarValues(
    State& denseState) const
{
    const SVF::Variable contentBegin =
        this->adapter_.firstObjectContentVariable();
    std::set<SVF::Variable> retainedScalars;

    for (SVF::Variable variable :
            denseState.numerical().supportVariablesBefore(contentBegin))
        if (retainedScalars.count(variable) == 0)
            denseState.numerical().forget(variable);
    for (SVF::Variable variable :
            denseState.addresses().nonDefaultVariablesBefore(contentBegin))
        denseState.addresses().forget(variable);
    for (SVF::Variable variable : denseState.initializedVariablesBefore(contentBegin))
        if (retainedScalars.count(variable) == 0)
            denseState.resetValue(variable);
}

void SemiSparseAbstractInterpretation::forgetMemoryValues(
    State& denseState) const
{
    for (SVF::Variable variable : nonDefaultVariables(denseState))
    {
        if (this->adapter_.contentObject(variable))
            this->forgetValue(denseState, variable);
    }
}

void SemiSparseAbstractInterpretation::restoreCallerFrameAfterSharedCallee(
    State& denseState, const RetICFGNode* returnSite,
    const State* callerOverride) const
{
    if (!returnSite)
        return;
    const CallICFGNode* call = returnSite->getCallICFGNode();
    if (!call || (!callerOverride && !this->hasAbsState(call)))
        return;

    if (!isSharedCalleeReturn(returnSite))
        return;

    const State& caller = callerOverride ? *callerOverride : this->state(call);
    NodeBS exposedBases;
    bool exposesAllObjects = false;
    auto expose = [&](const ObjVar* object)
    {
        if (object)
            exposedBases.set(
                this->svfir->getBaseObject(object->getId())->getId());
    };
    for (const ValVar* actual : call->getActualParms())
    {
        if (!actual || !actual->isPointer())
            continue;
        for (NodeID objectId :
                this->preAnalysis->getPointerAnalysis()->getPts(actual->getId()))
        {
            expose(SVFUtil::dyn_cast<ObjVar>(
                       this->svfir->getGNode(objectId)));
        }
        if (!this->adapter_.contains(*actual))
            continue;
        const SVF::AddressSet addresses = caller.addressSet(
                                              this->adapter_.variable(*actual));
        if (addresses.hasUnknownObject())
        {
            exposesAllObjects = true;
            continue;
        }
        for (SVF::Location location : addresses)
            expose(this->objectAt(location));
    }

    auto restore = [&](SVF::Variable content)
    {
        const ObjVar* object = this->adapter_.contentObject(content);
        const BaseObjVar* base = object
                                 ? this->svfir->getBaseObject(object->getId()) : nullptr;
        if (!base)
            return;
        if (base->isStack() && !exposesAllObjects &&
                !exposedBases.test(base->getId()))
            denseState.restoreMissingMemoryFrom(caller, content);
        else
            denseState.restoreMissingAddressFrom(caller, content);
    };
    for (SVF::Variable content : nonDefaultVariables(caller))
        restore(content);
}

bool SemiSparseAbstractInterpretation::isSharedFunction(
    const FunObjVar* function) const
{
    if (!function)
        return false;
    const ICFGNode* entry = this->icfg->getFunEntryICFGNode(function);
    const std::size_t callers = std::count_if(
                                    entry->getInEdges().begin(), entry->getInEdges().end(),
                                    [](const ICFGEdge* incoming)
    {
        return SVFUtil::isa<CallCFGEdge>(incoming);
    });
    return callers > 1;
}

bool SemiSparseAbstractInterpretation::isSharedCalleeReturn(
    const RetICFGNode* returnSite) const
{
    if (!returnSite)
        return false;
    for (const ICFGEdge* edge : returnSite->getInEdges())
    {
        if (!SVFUtil::isa<RetCFGEdge>(edge) || !edge->getSrcNode()->getFun())
            continue;
        if (isSharedFunction(edge->getSrcNode()->getFun()))
            return true;
    }
    return false;
}

bool SemiSparseAbstractInterpretation::preservesCallerFrame(
    const RetICFGNode* returnSite) const
{
    return returnSite && (!isSharedCalleeReturn(returnSite) ||
                          callerFrameCannotBeReentered(returnSite));
}

bool SemiSparseAbstractInterpretation::callerFrameCannotBeReentered(
    const RetICFGNode* returnSite) const
{
    const auto cached = nonReentrantReturnCache_.find(returnSite);
    if (cached != nonReentrantReturnCache_.end())
        return cached->second;
    const CallICFGNode* call = returnSite ? returnSite->getCallICFGNode() : nullptr;
    const FunObjVar* caller = call ? call->getCaller() : nullptr;
    std::vector<const FunObjVar*> pending;
    std::set<const FunObjVar*> visited;
    // An indirect target set must consist entirely of resolved function
    // objects. An empty/black-hole/non-function target is not a proof.
    const auto appendTargets = [&](const CallICFGNode* site)
    {
        if (const FunObjVar* direct = site->getCalledFunction())
        {
            pending.push_back(direct);
            return true;
        }
        const SVFVar* pointer = site->getIndFunPtr();
        if (!pointer || !this->callGraph->hasIndCSCallees(site))
            return false;
        const auto& targets = this->preAnalysis->getPointerAnalysis()->getPts(
                                  pointer->getId());
        if (targets.empty())
            return false;
        const auto& resolved = this->callGraph->getIndCSCallees(site);
        for (NodeID target : targets)
        {
            const auto* function = SVFUtil::dyn_cast<FunObjVar>(
                                       this->svfir->getGNode(target));
            if (!function || resolved.count(function) == 0)
                return false;
        }
        // Also traverse any extra conservative call-graph target rather
        // than assuming the two representations have identical contents.
        for (const FunObjVar* function : resolved)
            pending.push_back(function);
        return !resolved.empty();
    };
    bool safe = caller && appendTargets(call);
    while (safe && !pending.empty())
    {
        const FunObjVar* function = pending.back();
        pending.pop_back();
        if (function == caller)
        {
            safe = false;
            break;
        }
        if (!visited.insert(function).second)
            continue;
        if (SVFUtil::isExtCall(function))
        {
            // The frozen regression harness defines this zero-argument
            // primitive as fresh nondeterministic input with no callbacks.
            // No libc/unknown external function is inferred callback-free
            // merely because the call graph has no outgoing edge for it.
            if (!Options::AEHarnessNondetNoCallback() ||
                    function->getName() != "nondet_i32")
                safe = false;
            continue;
        }
        for (const CallICFGNode* nested : this->svfir->getCallSiteSet())
            if (nested->getCaller() == function && !appendTargets(nested))
            {
                safe = false;
                break;
            }
    }
    nonReentrantReturnCache_.emplace(returnSite, safe);

    return safe;
}

void SemiSparseAbstractInterpretation::applyScalarRefinement(
    State& denseState, const State& checkpoint)
{

    for (SVF::Variable variable : checkpoint.numerical().supportVariables())
    {
        const ValVar* value = this->adapter_.value(variable);
        if (!value || value->isPointer())
            continue;
        this->constrainInterval(denseState, variable,
                                checkpoint.numerical().bound(variable));
        denseState.setAddressSet(variable, SVF::AddressSet::bottom());
    }
}

void SemiSparseAbstractInterpretation::materializeValue(
    State& denseState, const ValVar* value, const ICFGNode* node)
{
    if (!value || !this->adapter_.contains(*value))
        return;
    const SVF::Variable variable = this->adapter_.variable(*value);
    if (value->isPointer())
    {
        denseState.setAddressSet(variable, getAddressSet(value, node));
        denseState.setInterval(variable, SVF::Interval::bottom());
    }
    else
    {
        this->constrainInterval(denseState, variable, getInterval(value, node));
        denseState.setAddressSet(variable, SVF::AddressSet::bottom());
    }
}

void SemiSparseAbstractInterpretation::materializeRelations(
    State& denseState, const std::vector<SVF::Variable>& variables,
    const ICFGNode* node)
{
    return;
}

std::vector<SVF::Variable> SemiSparseAbstractInterpretation::
carrierDependencyClosure(const std::vector<SVF::Variable>& seeds) const
{
    std::set<SVF::Variable> closure(seeds.begin(), seeds.end());
    std::vector<SVF::Variable> worklist(seeds.begin(), seeds.end());
    while (!worklist.empty())
    {
        const SVF::Variable variable = worklist.back();
        worklist.pop_back();
        const auto dependencies = scalarDependencies_.find(variable);
        if (dependencies == scalarDependencies_.end())
            continue;
        for (SVF::Variable dependency : dependencies->second)
        {
            if (closure.insert(dependency).second)
                worklist.push_back(dependency);
        }
    }
    return std::vector<SVF::Variable>(closure.begin(), closure.end());
}

void SemiSparseAbstractInterpretation::materializeScalarDefinitions(
    State& destination, const std::vector<SVF::Variable>& seeds,
    const ICFGNode* node,
    const std::set<SVF::Variable>* availableOverride) const
{
    if (!node || seeds.empty())
        return;
    const auto available = scalarAvailability_.find(node);
    if (!availableOverride && available == scalarAvailability_.end())
        return;

    const auto isAvailable = [&](SVF::Variable variable)
    {
        return (availableOverride ? availableOverride->count(variable) != 0
                : isScalarAvailable(node, variable)) ||
               extraAvailable(node, variable);
    };
    std::set<SVF::Variable> retained;
    for (SVF::Variable seed : seeds)
    {
        if (!isAvailable(seed))
            continue;
        // Keep an available definition even when part of its transitive
        // carrier is out of scope at this point.  Projecting the saved
        // summary below existentially forgets those unavailable coordinates;
        // dropping the whole closure would also drop the seed's value and
        // initialization facet.
        retained.insert(seed);
        const std::vector<SVF::Variable> closure =
            carrierDependencyClosure({seed});
        for (SVF::Variable dependency : closure)
        {
            if (isAvailable(dependency))
                retained.insert(dependency);
        }
    }
    if (retained.empty())
        return;

    const std::vector<SVF::Variable> variables(retained.begin(), retained.end());
    for (SVF::Variable variable : variables)
    {
        const auto definition = scalarDefinitions_.find(variable);
        if (definition != scalarDefinitions_.end())
            destination.assignValueFrom(variable, definition->second, variable);
        else if (scalarState_)
            destination.assignValueFrom(variable, *scalarState_, variable);
    }
    // Coordinate copies intentionally discard relations. Reapply every
    // available definition summary only after initialization/address facets
    // are in place, and never meet whole products with incompatible sparse
    // defaults.
    for (SVF::Variable variable : variables)
    {
        const auto definition = scalarDefinitions_.find(variable);
        if (definition == scalarDefinitions_.end())
            continue;
        State projected = definition->second;
        projected.numerical().project(variables);
        destination.numerical().meetWith(projected.numerical());
    }

}

void SemiSparseAbstractInterpretation::loadValue(
    const ValVar* pointer, SVF::Interval& interval, SVF::AddressSet& addresses,
    bool& numericalMayBeUninitialized,
    const ICFGNode* node)
{
    Base::loadValue(pointer, interval, addresses,
                    numericalMayBeUninitialized, node);
    if (pointer && this->adapter_.contains(*pointer))
        this->forgetValue(this->ensureState(node),
                          this->adapter_.variable(*pointer));
}

void SemiSparseAbstractInterpretation::storeValue(
    const ValVar* pointer, const SVF::Interval& interval,
    const SVF::AddressSet& addresses, const ICFGNode* node)
{
    Base::storeValue(pointer, interval, addresses, node);
    if (pointer && this->adapter_.contains(*pointer))
        this->forgetValue(this->ensureState(node),
                          this->adapter_.variable(*pointer));
}

SemiSparseAbstractInterpretation::State
SemiSparseAbstractInterpretation::propagatedState(const State& state) const
{
    return state;
}

void SemiSparseAbstractInterpretation::filterPropagatedState(State& state) const
{
    (void)state;
}

bool SemiSparseAbstractInterpretation::mergeStatesFromPredecessors(
    const ICFGNode* node)
{

    State merged = flowState(true);
    std::optional<State> mergedRefinement;
    bool refinementIsTop = false;
    bool hasFeasiblePredecessor = false;

    for (const ICFGEdge* edge : node->getInEdges())
    {
        const ICFGNode* predecessor = edge->getSrcNode();
        if (!this->hasAbsState(predecessor))
            continue;

        bool shouldMerge = false;
        const auto* conditional = SVFUtil::dyn_cast<IntraCFGEdge>(edge);
        if (conditional || SVFUtil::isa<CallCFGEdge>(edge))
        {
            shouldMerge = true;
        }
        else if (SVFUtil::isa<RetCFGEdge>(edge))
        {
            const auto* returnSite = SVFUtil::dyn_cast<RetICFGNode>(node);
            shouldMerge = returnSite &&
                          this->hasAbsState(returnSite->getCallICFGNode());
        }
        if (!shouldMerge)
            continue;

        const auto refinementIterator = refinementTrace_.find(predecessor);
        const bool hasConditional = conditional && conditional->getCondition();
        const bool needsRefinement =
            hasConditional || refinementIterator != refinementTrace_.end();
        std::optional<State> refinement;
        if (needsRefinement)
        {
            refinement = refinementIterator != refinementTrace_.end()
                         ? refinementIterator->second
                         : State(this->makeNumericalDomain(false),
                                 this->adapter_.memoryLayout());
            if (hasConditional)
                this->assumeBranch(conditional, *refinement);
            if (refinement->isBottom())
            {

                continue;
            }
        }

        State source = propagatedState(this->state(predecessor));
        if (const auto* ret = SVFUtil::dyn_cast<RetCFGEdge>(edge))
        {
            // Scalar SSA definitions live in the semi-sparse carrier rather
            // than in persistent ICFG states.  A return binding is owned by
            // one callee edge, so reconstruct that edge's formal return in
            // its source state before applying RetPE.  Doing this per edge
            // preserves join(Post_edge) for multi-target calls; materializing
            // after the join would conflate alternative formal returns.
            const RetPE* binding = ret->getRetPE();
            const auto* returned = binding
                                   ? SVFUtil::dyn_cast<ValVar>(
                                       binding->getRHSVar())
                                   : nullptr;
            if (returned && this->adapter_.contains(*returned))
            {
                materializeScalarDefinitions(
                    source, {this->adapter_.variable(*returned)},
                    predecessor);
            }
            this->applyReturnEdgeTransfer(source, ret);
        }
        filterPropagatedState(source);
        if (hasConditional)
            this->collectBranchRefinement(conditional, source);

        merged.joinWith(source);
        if (!refinement || refinement->isTop())
        {
            refinementIsTop = true;
            mergedRefinement.reset();
        }
        else if (!refinementIsTop)
        {
            forgetMemoryValues(*refinement);
            if (!mergedRefinement)
                mergedRefinement = std::move(*refinement);
            else
                mergedRefinement->joinWith(*refinement);
        }
        hasFeasiblePredecessor = true;
    }

    if (!hasFeasiblePredecessor)
    {

        return false;
    }
    if (const auto* returnSite = SVFUtil::dyn_cast<RetICFGNode>(node))
    {
        const CallICFGNode* call = returnSite->getCallICFGNode();
        const auto callerAvailability = scalarAvailability_.find(call);
        if (call && this->hasAbsState(call) &&
                callerAvailability != scalarAvailability_.end())
        {
            {
                const SVF::Variable end =
                    this->adapter_.firstObjectContentVariable();
                std::set<SVF::Variable> active;
                for (const State* state :
                        {
                            &this->state(call),
                            static_cast<const State*>(&merged)
                        })
                {
                    const auto add = [&](const std::vector<SVF::Variable>& variables)
                    {
                        for (SVF::Variable variable : variables)
                            if (isScalarAvailable(call, variable))
                                active.insert(variable);
                    };
                    add(state->numerical().supportVariablesBefore(end));
                    add(state->addresses().nonDefaultVariablesBefore(end));
                    add(state->initializedVariablesBefore(end));
                }
                // Absent Box coordinates are independent uninitialized facets.
                restoreCallerFrame(
                    merged, returnSite, this->state(call), active);
            }
        }
    }
    if (mergedRefinement && !refinementIsTop && !mergedRefinement->isTop())
    {
        refinementTrace_.insert_or_assign(node, *mergedRefinement);
        applyScalarRefinement(merged, *mergedRefinement);
    }
    else
    {
        refinementTrace_.erase(node);
    }
    this->stateTrace_.insert_or_assign(node, std::move(merged));

    return true;
}

std::unique_ptr<SVF::AbstractDomain> SemiSparseAbstractInterpretation::
cloneCycleHeadState(const ICFGCycleWTO* cycle)
{
    const ICFGNode* head = cycle->head()->getICFGNode();
    State snapshot = this->state(head);
    const auto available = scalarAvailability_.find(head);
    std::vector<SVF::Variable> cycleVariables;
    for (const ValVar* value : this->preAnalysis->getCycleValVars(cycle))
    {
        if (!value || !this->adapter_.contains(*value))
            continue;
        const SVF::Variable variable = this->adapter_.variable(*value);
        if (available != scalarAvailability_.end() &&
                !isScalarAvailable(head, variable))
            continue;
        cycleVariables.push_back(variable);
    }
    {
        for (SVF::Variable variable : cycleVariables)
            snapshot.assignValueFrom(variable, scalarState(), variable);
    }
    const auto refinement = refinementTrace_.find(head);
    if (refinement != refinementTrace_.end())
    {
        State projected = refinement->second;
        projected.numerical().project(cycleVariables);
        applyScalarRefinement(snapshot, projected);
    }

    return std::make_unique<State>(std::move(snapshot));
}

void SemiSparseAbstractInterpretation::scatterCycleValues(
    const ICFGCycleWTO* cycle, const State& cycleState)
{
    const ICFGNode* head = cycle->head()->getICFGNode();
    const auto available = scalarAvailability_.find(head);
    for (const ValVar* value : this->preAnalysis->getCycleValVars(cycle))
    {
        if (!value || !this->adapter_.contains(*value))
            continue;
        const SVF::Variable variable = this->adapter_.variable(*value);
        if (available != scalarAvailability_.end() &&
                !isScalarAvailable(head, variable))
            continue;
        // A widened cycle value is a complete product coordinate.  Copy its
        // initialization guards as well as its numerical/address payloads;
        // rebuilding it from the payload alone turns Top into Initialized
        // and loses the distinction between an unknown value and an
        // uninitialized facet.
        scalarState().assignValueFrom(variable, cycleState, variable);
        {
            State summary = this->topState();
            summary.assignValueFrom(variable, cycleState, variable);
            scalarDefinitions_.insert_or_assign(variable,
                                                std::move(summary));
        }
    }
}

bool SemiSparseAbstractInterpretation::widenCycleState(
    const SVF::AbstractDomain& previous, const SVF::AbstractDomain& current,
    const ICFGCycleWTO* cycle)
{
    const bool fixpoint = Base::widenCycleState(previous, current, cycle);

    scatterCycleValues(cycle, this->state(cycle->head()->getICFGNode()));
    finalizeAbstractState(cycle->head()->getICFGNode());
    return fixpoint;
}

bool SemiSparseAbstractInterpretation::narrowCycleState(
    const SVF::AbstractDomain& previous, const SVF::AbstractDomain& current,
    const ICFGCycleWTO* cycle)
{
    const bool fixpoint = Base::narrowCycleState(previous, current, cycle);

    if (!fixpoint)
    {
        scatterCycleValues(cycle, this->state(cycle->head()->getICFGNode()));
    }
    finalizeAbstractState(cycle->head()->getICFGNode());
    return fixpoint;
}

namespace
{

bool hasRedefinitionOf(const ICFGNode* node, const IndirectSVFGEdge* edge)
{
    for (const VFGNode* valueFlowNode : node->getVFGNodes())
    {
        if (SVFUtil::isa<StoreVFGNode>(valueFlowNode) &&
                valueFlowNode->getDefSVFVars().intersects(edge->getPointsTo()))
            return true;
    }
    return false;
}

} // namespace

FullSparseAbstractInterpretation::FullSparseAbstractInterpretation()
{
    svfgBuilder_ = std::make_unique<SVFGBuilder>(true);
    svfgBuilder_->buildFullSVFG(this->preAnalysis->getPointerAnalysis());
    // MemorySSA cannot encode runtime object-top aliases introduced by
    // integer-to-pointer conversions. Preserve the prior contents from the
    // start, not only after the first unknown write, so its unchanged weak
    // alternative remains available as well as the newly written value.
    bool unknownMemory = false;
    for (auto it = this->icfg->begin(); it != this->icfg->end(); ++it)
        for (const SVFStmt* statement : it->second->getSVFStmts())
        {
            if (const auto* copy = SVFUtil::dyn_cast<CopyStmt>(statement))
                unknownMemory |= copy->getCopyKind() == CopyStmt::INTTOPTR;
            if (const auto* store = SVFUtil::dyn_cast<StoreStmt>(statement))
            {
                const auto& targets = this->preAnalysis->getPointerAnalysis()->getPts(store->getLHSVarID());
                unknownMemory |= targets.empty() || targets.test(this->svfir->getBlackHoleNode());
            }
        }
    if (unknownMemory)
        for (const auto& [location, content] : this->adapter_.memoryLayout().cells())
        {
            (void)location;
            denseMemoryVariables_.insert(content);
        }
    // Dedicated memcpy models read memory directly, while the synthetic
    // SVFG edges may name field zero instead of the source scalar base.
    // Carry those inputs through the ICFG from their original writes, just
    // as modeled output cells already use denseMemoryVariables_.
    for (auto it = this->icfg->begin(); it != this->icfg->end(); ++it)
    {
        const auto* call = SVFUtil::dyn_cast<CallICFGNode>(it->second);
        const FunObjVar* callee = call ? call->getCalledFunction() : nullptr;
        if (!callee || !ExtAPI::getExtAPI()->hasExtFuncAnnotation(callee, "MEMCPY"))
            continue;
        for (const ValVar* argument : call->getActualParms())
            if (argument->isPointer())
                for (NodeID id : this->preAnalysis->getPointerAnalysis()->getPts(argument->getId()))
                {
                    const BaseObjVar* base = this->svfir->getBaseObject(id);
                    if (base)
                        denseMemoryVariables_.insert(this->adapter_.contentVariable(*base));
                }
    }
}

FullSparseAbstractInterpretation::
~FullSparseAbstractInterpretation() = default;

void FullSparseAbstractInterpretation::classifyMemoryVariables() const
{
    const auto& cells = this->adapter_.memoryLayout().cells();
    for (auto cell = cells.upper_bound(classifiedMemoryLocation_);
            cell != cells.end(); ++cell)
    {
        const ObjVar* object = this->adapter_.contentObject(cell->second);
        if (object && SVFUtil::isa<GepObjVar>(object))
            fieldVariables_.emplace(cell->second, object->getId());
        if (object && !SVFUtil::isa<GepObjVar>(object) &&
                !denseMemoryVariables_.count(cell->second))
            svfgMemoryVariables_.insert(cell->second);
        classifiedMemoryLocation_ = cell->first;
    }
}

FullSparseAbstractInterpretation::State
FullSparseAbstractInterpretation::propagatedState(const State& state) const
{
    if (state.isBottom())
        return state;
    classifyMemoryVariables();
    for (auto variable = svfgMemoryVariables_.begin();
            variable != svfgMemoryVariables_.end();)
    {
        if (denseMemoryVariables_.count(*variable))
            variable = svfgMemoryVariables_.erase(variable);
        else
            ++variable;
    }
    return state.withoutValues(svfgMemoryVariables_);
}

void FullSparseAbstractInterpretation::recordBranchRefinement(
    NodeID objectId, const SVF::Interval& narrowed, SVF::AbstractDomain&,
    const ICFGNode*, const ICFGNode* successor)
{
    if (narrowed.isBottom())
        return;
    auto& refinements = memoryRefinementTrace_[successor];
    const auto iterator = refinements.find(objectId);
    if (iterator == refinements.end())
        refinements.emplace(objectId, narrowed);
    else
        iterator->second.joinWith(narrowed);
}

void FullSparseAbstractInterpretation::storeValue(
    const ValVar* pointer, const SVF::Interval& interval,
    const SVF::AddressSet& valueAddresses, const ICFGNode* node)
{
    const SVF::AddressSet addresses = Base::getAddressSet(pointer, node);
    // A runtime unknown target can alias objects absent from Andersen's
    // finite MemorySSA edge labels. Its weak writes must survive ICFG joins;
    // otherwise a later sparse pull resurrects the pre-write definition.
    if (addresses.hasUnknownObject())
    {
        for (const auto& [location, content] : this->adapter_.memoryLayout().cells())
        {
            (void)location;
            denseMemoryVariables_.insert(content);
        }
        memoryRefinementTrace_.erase(node);
    }
    auto refinement = memoryRefinementTrace_.find(node);
    if (refinement != memoryRefinementTrace_.end() &&
            !addresses.hasUnknownObject())
    {
        for (SVF::Location location : addresses)
            if (const ObjVar* object = this->objectAt(location))
                refinement->second.erase(object->getId());
    }
    Base::storeValue(pointer, interval, valueAddresses, node);
    recordMemoryDefinition(node, addresses);
}

void FullSparseAbstractInterpretation::recordMemoryDefinition(
    const ICFGNode* node, const SVF::AddressSet& targets)
{
    auto record = [&](SVF::Location location)
    {
        if (location.isNull() ||
                !this->adapter_.memoryLayout().contains(location))
            return;
        const ObjVar* object = this->objectAt(location);
        if (!object)
            return;
        // A freed-target store updated BlackHole's cell, not the original
        // object named by this MemorySSA edge. Do not invent a definition for
        // that original object merely because its payload slot is absent.
        if (this->memoryVariable(*object, this->state(node)) !=
                this->adapter_.contentVariable(*object))
        {
            const auto definitions = memoryDefinitionSupport_.find(node);
            if (definitions != memoryDefinitionSupport_.end())
            {
                definitions->second.erase(this->adapter_.contentVariable(*object));
                if (definitions->second.empty())
                    memoryDefinitionSupport_.erase(definitions);
            }
            return;
        }
        if (Base::hasAbsValue(object, node))
            return;
        memoryDefinitionSupport_[node].insert(
            this->adapter_.memoryLayout().contentOf(location));
    };

    if (targets.hasUnknownObject())
    {
        for (const auto& [location, content] :
                this->adapter_.memoryLayout().cells())
        {

            (void)content;
            record(location);
        }
    }
    else
    {
        for (SVF::Location location : targets.locations())
            record(location);
    }
}

bool FullSparseAbstractInterpretation::hasMemoryDefinition(
    const ICFGNode* node, SVF::Variable content) const
{
    const auto support = memoryDefinitionSupport_.find(node);
    return support != memoryDefinitionSupport_.end() &&
           support->second.count(content) != 0;
}

void FullSparseAbstractInterpretation::updateMemoryValue(
    SVF::Location location, const SVF::Interval& interval,
    const SVF::AddressSet& addresses, const ICFGNode* node)
{
    if (!location.isNull())
    {
        const ObjVar& object = this->adapter_.object(location);
        denseMemoryVariables_.insert(
            this->adapter_.contentVariable(object));
    }
    Base::updateMemoryValue(location, interval, addresses, node);
}

bool FullSparseAbstractInterpretation::mergeStatesFromPredecessors(
    const ICFGNode* node)
{
    memoryRefinementTrace_.erase(node);
    previousMemoryDefinitionSupport_.erase(node);
    const auto oldSupport = memoryDefinitionSupport_.find(node);
    if (oldSupport != memoryDefinitionSupport_.end() &&
            !oldSupport->second.empty())
    {
        previousMemoryDefinitionSupport_.emplace(node, oldSupport->second);
    }
    memoryDefinitionSupport_.erase(node);
    if (!Base::mergeStatesFromPredecessors(node))
        return false;
    // Caller-frame restoration must not create an extra MemorySSA definition
    // from only one restored facet. Pull these objects from their defining
    // edges; keep the ordinary ICFG fallback for fields and modeled writes.
    if (SVFUtil::isa<RetICFGNode>(node))
    {
        classifyMemoryVariables();
        if (!svfgMemoryVariables_.empty())
            this->ensureState(node) = propagatedState(this->state(node));
    }
    // A direct object constraint collected from one incoming branch cannot be
    // applied after another incoming path has joined without that constraint.
    // Inherited constraints below already implement the precise all-preds
    // intersection rule; discard edge-local constraints at explicit merges.
    if (node->getInEdges().size() > 1)
        memoryRefinementTrace_.erase(node);
    pullObjectValueFlows(node);
    propagateAndApplyMemoryRefinement(node);
    return true;
}

bool FullSparseAbstractInterpretation::widenCycleState(
    const SVF::AbstractDomain& previous, const SVF::AbstractDomain& current,
    const ICFGCycleWTO* cycle)
{
    const ICFGNode* head = cycle->head()->getICFGNode();
    const auto old = previousMemoryDefinitionSupport_.find(head);
    const std::set<SVF::Variable> empty;
    const std::set<SVF::Variable>& oldSupport =
        old == previousMemoryDefinitionSupport_.end() ? empty : old->second;
    auto& support = memoryDefinitionSupport_[head];
    support.insert(oldSupport.begin(), oldSupport.end());
    const bool supportFixpoint = support == oldSupport;
    if (support.empty())
        memoryDefinitionSupport_.erase(head);
    return Base::widenCycleState(previous, current, cycle) &&
           supportFixpoint;
}

bool FullSparseAbstractInterpretation::narrowCycleState(
    const SVF::AbstractDomain& previous, const SVF::AbstractDomain& current,
    const ICFGCycleWTO* cycle)
{
    const ICFGNode* head = cycle->head()->getICFGNode();
    const auto old = previousMemoryDefinitionSupport_.find(head);
    const auto next = memoryDefinitionSupport_.find(head);
    const bool oldEmpty = old == previousMemoryDefinitionSupport_.end();
    const bool nextEmpty = next == memoryDefinitionSupport_.end();
    const bool supportFixpoint =
        (oldEmpty && nextEmpty) ||
        (!oldEmpty && !nextEmpty && old->second == next->second);
    return Base::narrowCycleState(previous, current, cycle) &&
           supportFixpoint;
}

void FullSparseAbstractInterpretation::pullObjectValueFlows(
    const ICFGNode* node)
{
    NodeBS denseLocalObjects;
    State& destination = this->ensureState(node);
    classifyMemoryVariables();
    for (const auto& [variable, object] : fieldVariables_)
    {
        const bool stored =
            destination.numericalInitialization().value(variable) !=
            destination.numericalInitialization().defaultState() ||
            destination.addressInitialization().value(variable) !=
            destination.addressInitialization().defaultState() ||
            (!destination.numerical().isBottom() &&
             !destination.numerical().bound(variable).isTop()) ||
            (!destination.addresses().isBottom() &&
             !destination.addresses().addressSet(variable).isTop());
        if (stored)
            denseLocalObjects.set(object);
    }
    NodeBS pulledObjects;

    for (const VFGNode* valueFlowNode : node->getVFGNodes())
    {
        for (auto edgeIterator = valueFlowNode->InEdgeBegin();
                edgeIterator != valueFlowNode->InEdgeEnd(); ++edgeIterator)
        {
            const auto* indirect =
                SVFUtil::dyn_cast<IndirectSVFGEdge>(*edgeIterator);
            if (!indirect ||
                    !isIndirectSVFGEdgeFeasible(indirect, valueFlowNode))
                continue;

            const auto* sourceNode =
                SVFUtil::dyn_cast<SVFGNode>(indirect->getSrcNode());
            assert(sourceNode && sourceNode->getICFGNode() &&
                   "SVFG source must have an ICFG node");
            const ICFGNode* source = sourceNode->getICFGNode();
            if (!this->hasAbsState(source))
                continue;

            for (NodeID objectId : indirect->getPointsTo())
            {
                SVFVar* graphNode = this->svfir->getGNode(objectId);
                NodeBS objectsToPull;
                if (SVFUtil::isa<GepObjVar>(graphNode))
                    objectsToPull.set(objectId);
                else if (auto* base = SVFUtil::dyn_cast<BaseObjVar>(graphNode))
                    objectsToPull = this->svfir->getAllFieldsObjVars(base);
                else
                    objectsToPull.set(objectId);

                for (NodeID fieldId : objectsToPull)
                {
                    if (denseLocalObjects.test(fieldId))
                        continue;
                    const auto* object = SVFUtil::dyn_cast<ObjVar>(
                                             this->svfir->getGNode(fieldId));
                    if (!object)
                        continue;
                    const SVF::Variable content =
                        this->adapter_.contentVariable(*object);
                    // Model-side writes have no StoreStmt/SVFG definition.
                    // Keep their ICFG value, even when both facets are Top.
                    if (denseMemoryVariables_.count(content))
                        continue;
                    const bool hasDefinition = Base::hasAbsValue(object, source) ||
                                               hasMemoryDefinition(source, content);
                    // MemorySSA roots a local stack object's initial contents
                    // at FormalIN, not at its AddrStmt. Decode that initial
                    // definition using AE's uninitialized-memory policy.
                    bool initialStackDefinition = false;
                    if (!hasDefinition && SVFUtil::isa<FormalINSVFGNode>(sourceNode))
                    {
                        const BaseObjVar* base = this->svfir->getBaseObject(fieldId);
                        initialStackDefinition = base->isStack() &&
                                                 base->getFunction() == source->getFun();
                    }
                    if (!hasDefinition && !initialStackDefinition)
                        continue;

                    const State& sourceState = this->state(source);
                    const SVF::Variable destinationContent =
                        this->memoryVariable(*object, destination);
                    const SVF::Variable sourceContent =
                        this->memoryVariable(*object, sourceState);
                    if (pulledObjects.test(fieldId))
                        destination.joinValueFrom(destinationContent,
                                                  sourceState, sourceContent);
                    else
                        destination.assignValueFrom(destinationContent,
                                                    sourceState, sourceContent);
                    if (!Base::hasAbsValue(object, node) &&
                            this->memoryVariable(*object, this->state(node)) == content)
                        memoryDefinitionSupport_[node].insert(content);
                    pulledObjects.set(fieldId);
                }
            }
        }
    }
}

bool FullSparseAbstractInterpretation::isIntraEdgeBranchFeasible(
    const IntraCFGEdge* edge, const ICFGNode* source)
{
    return !edge->getCondition() || !this->hasAbsState(source) ||
           this->isBranchEdgeFeasibleAt(edge, source);
}

bool FullSparseAbstractInterpretation::isIndirectSVFGEdgeFeasible(
    const IndirectSVFGEdge* edge, const VFGNode* destination)
{
    assert(edge && destination && "SVFG edge and destination must exist");
    const auto* sourceNode = SVFUtil::dyn_cast<SVFGNode>(edge->getSrcNode());
    assert(sourceNode && "indirect SVFG edge must have an SVFG source");
    const ICFGNode* source = sourceNode->getICFGNode();
    const ICFGNode* target = destination->getICFGNode();
    assert(source && target && "SVFG endpoints must have ICFG nodes");

    const FunObjVar* function = source->getFun();
    if (source == target || !function || function != target->getFun())
        return true;

    std::deque<const ICFGNode*> worklist;
    Set<const ICFGNode*> visited;
    worklist.push_back(source);
    visited.insert(source);
    while (!worklist.empty())
    {
        const ICFGNode* current = worklist.front();
        worklist.pop_front();
        if (current != source && hasRedefinitionOf(current, edge))
            continue;

        if (const auto* call = SVFUtil::dyn_cast<CallICFGNode>(current))
        {
            const ICFGNode* successor = call->getRetICFGNode();
            if (successor && successor->getFun() == function)
            {
                if (successor == target)
                    return true;
                if (visited.insert(successor).second)
                    worklist.push_back(successor);
            }
        }

        for (const ICFGEdge* cfgEdge : current->getOutEdges())
        {
            const auto* intra = SVFUtil::dyn_cast<IntraCFGEdge>(cfgEdge);
            const ICFGNode* successor = intra ? intra->getDstNode() : nullptr;
            if (!successor || successor->getFun() != function ||
                    !isIntraEdgeBranchFeasible(intra, current))
                continue;
            if (successor == target)
                return true;
            if (visited.insert(successor).second)
                worklist.push_back(successor);
        }
    }
    return false;
}

void FullSparseAbstractInterpretation::propagateAndApplyMemoryRefinement(
    const ICFGNode* node)
{
    Map<NodeID, SVF::Interval> inherited;
    bool canInherit = true;
    bool first = true;
    for (const ICFGEdge* edge : node->getInEdges())
    {
        const ICFGNode* predecessor = edge->getSrcNode();
        if (!this->hasAbsState(predecessor))
            continue;
        const auto predecessorRefinement =
            memoryRefinementTrace_.find(predecessor);
        if (predecessorRefinement == memoryRefinementTrace_.end())
        {
            canInherit = false;
            break;
        }
        if (first)
        {
            inherited = predecessorRefinement->second;
            first = false;
            continue;
        }
        for (auto iterator = inherited.begin(); iterator != inherited.end();)
        {
            const auto incoming =
                predecessorRefinement->second.find(iterator->first);
            if (incoming == predecessorRefinement->second.end())
                iterator = inherited.erase(iterator);
            else
            {
                iterator->second.joinWith(incoming->second);
                ++iterator;
            }
        }
    }

    if (canInherit && !first)
    {
        auto& refinements = memoryRefinementTrace_[node];
        for (const auto& [objectId, constraint] : inherited)
        {
            const auto current = refinements.find(objectId);
            if (current == refinements.end())
                refinements.emplace(objectId, constraint);
            else
                current->second.meetWith(constraint);
        }
    }

    const auto refinements = memoryRefinementTrace_.find(node);
    if (refinements == memoryRefinementTrace_.end())
        return;
    State& denseState = this->ensureState(node);
    for (const auto& [objectId, constraint] : refinements->second)
    {
        const auto* object =
            SVFUtil::dyn_cast<ObjVar>(this->svfir->getGNode(objectId));
        if (!object)
            continue;
        const SVF::Variable content = this->adapter_.contentVariable(*object);
        if (!object->isPointer())
            this->constrainInterval(denseState, content, constraint);
    }
}

} // namespace SVF
