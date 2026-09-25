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

namespace AD = AbstractDomain;

namespace
{

std::vector<AD::Variable> nonDefaultVariables(
    const SemiSparseAbstractInterpretation::State& state)
{
    // All domain support queries are already sorted and unique. Keep that
    // order without allocating a tree node for every initialized coordinate.
    const auto numbers = state.numerical().supportVariables();
    const auto pointers = state.addresses().nonDefaultVariables();
    std::vector<AD::Variable> payloads;
    payloads.reserve(numbers.size() + pointers.size());
    std::set_union(numbers.begin(), numbers.end(), pointers.begin(), pointers.end(),
                   std::back_inserter(payloads));
    const auto initialized = state.initializedVariables();
    std::vector<AD::Variable> variables;
    variables.reserve(payloads.size() + initialized.size());
    std::set_union(payloads.begin(), payloads.end(), initialized.begin(), initialized.end(),
                   std::back_inserter(variables));
    return variables;
}

std::set<AD::Variable> definedScalarVariables(const ICFGNode* node,
                                              const SVFIRAdapter& adapter)
{
    std::set<AD::Variable> result;
    const auto add = [&](const SVFVar* variable) {
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
    std::set<AD::Variable> universe;
    for (auto iterator = this->svfir->begin(); iterator != this->svfir->end();
         ++iterator)
    {
        const auto* value = SVFUtil::dyn_cast<ValVar>(iterator->second);
        if (value && this->adapter_.contains(*value))
            universe.insert(this->adapter_.variable(*value));
    }

    std::vector<const ICFGNode*> nodes;
    for (auto iterator = this->icfg->begin(); iterator != this->icfg->end();
         ++iterator)
    {
        nodes.push_back(iterator->second);
        scalarAvailability_.emplace(iterator->second, universe);
    }
    std::sort(nodes.begin(), nodes.end(),
              [](const ICFGNode* left, const ICFGNode* right) {
                  return left->getId() < right->getId();
              });

    const ICFGNode* global = this->icfg->getGlobalICFGNode();
    std::set<AD::Variable> globalOut =
        definedScalarVariables(global, this->adapter_);
    FIFOWorkList<const FunObjVar*> roots = this->collectProgEntryFuns();
    while (!roots.empty())
    {
        const FunEntryICFGNode* entry =
            this->icfg->getFunEntryICFGNode(roots.pop());
        for (const SVFVar* argument : entry->getFormalParms())
        {
            const auto* value = SVFUtil::dyn_cast<ValVar>(argument);
            if (value && this->adapter_.contains(*value))
                globalOut.insert(this->adapter_.variable(*value));
        }
    }
    scalarAvailability_[global] = std::move(globalOut);

    bool changed = true;
    while (changed)
    {
        changed = false;
        for (const ICFGNode* node : nodes)
        {
            if (node == global)
                continue;
            bool first = true;
            std::set<AD::Variable> incoming;
            for (const ICFGEdge* edge : node->getInEdges())
            {
                if (!SVFUtil::isa<IntraCFGEdge>(edge) &&
                    !SVFUtil::isa<CallCFGEdge>(edge) &&
                    !SVFUtil::isa<RetCFGEdge>(edge))
                    continue;
                const auto predecessor =
                    scalarAvailability_.find(edge->getSrcNode());
                if (predecessor == scalarAvailability_.end())
                    continue;
                std::set<AD::Variable> edgeAvailable = predecessor->second;
                // Return transfer retains the callee's formal-return ghosts
                // needed to bind RetPE relations and restores only the caller
                // SSA frame. Other callee locals are out of scope here and
                // must not leak into the caller's global scalar carrier.
                if (const auto* ret = SVFUtil::dyn_cast<RetCFGEdge>(edge))
                {
                    edgeAvailable.clear();
                    const auto caller =
                        scalarAvailability_.find(ret->getCallSite());
                    if (caller != scalarAvailability_.end())
                        edgeAvailable = caller->second;
                    for (const SVFStmt* statement : node->getSVFStmts())
                    {
                        const auto* binding =
                            SVFUtil::dyn_cast<RetPE>(statement);
                        const auto* formal = binding
                            ? SVFUtil::dyn_cast<ValVar>(binding->getRHSVar())
                            : nullptr;
                        if (formal && this->adapter_.contains(*formal))
                            edgeAvailable.insert(
                                this->adapter_.variable(*formal));
                    }
                }
                if (first)
                {
                    incoming = std::move(edgeAvailable);
                    first = false;
                }
                else
                {
                    std::set<AD::Variable> intersection;
                    std::set_intersection(
                        incoming.begin(), incoming.end(),
                        edgeAvailable.begin(), edgeAvailable.end(),
                        std::inserter(intersection, intersection.end()));
                    incoming = std::move(intersection);
                }
            }
            if (first)
                incoming.clear();
            const std::set<AD::Variable> definitions =
                definedScalarVariables(node, this->adapter_);
            incoming.insert(definitions.begin(), definitions.end());
            if (incoming != scalarAvailability_.at(node))
            {
                scalarAvailability_[node] = std::move(incoming);
                changed = true;
            }
        }
    }
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
        std::vector<AD::Variable> seeds;
        const auto available = scalarAvailability_.find(predecessor);
        if (available != scalarAvailability_.end())
            seeds.assign(available->second.begin(), available->second.end());
        if (Options::AEDomain() == AENumericalDomain::Box)
        {
            for (AD::Variable variable : seeds)
                alternative.assignValueFrom(variable, scalarState(), variable);
        }
        else
            materializeScalarDefinitions(alternative, seeds, predecessor);
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

void SemiSparseAbstractInterpretation::assignRelationalValue(
    const ValVar* target, const AD::LinearExpression& expression,
    const AD::Interval& interval, const AD::AddressSet& addresses,
    const ICFGNode* node)
{
    if (!target || !this->adapter_.contains(*target))
        return;
    const AD::Variable variable = this->adapter_.variable(*target);
    State& scalars = scalarState();
    scalars.assignNumeric(variable, expression);
    scalars.setAddressSet(variable, addresses);
    if (Options::AEDomain() != AENumericalDomain::Box)
    {
        State summary = this->topState();
        std::vector<AD::Variable> sources;
        for (const auto& [source, coefficient] : expression.terms())
        {
            (void)coefficient;
            sources.push_back(source);
        }
        materializeScalarDefinitions(summary, sources, node);
        summary.assignNumeric(variable, expression);
        // The interval was evaluated in the current invocation.  It is stable
        // for a root or single-call-site function, but not for a
        // context-insensitive callee shared by multiple call sites: a later
        // caller can enlarge the formal inputs after an earlier return edge
        // has already been processed.  Keep only the invocation-independent
        // affine equality in that case.  Reusing call-local unary bounds would
        // make the earlier return state fail the interprocedural Post equation.
        const bool sharedFunction =
            isSharedFunction(node ? node->getFun() : nullptr);
        const bool cyclicFunction =
            this->preAnalysis->functionHasCycle(node ? node->getFun()
                                               : nullptr);
        const AD::Interval impliedInterval =
            summary.numerical().bound(variable);
        if (!sharedFunction && !cyclicFunction &&
                !impliedInterval.isSubsetOf(interval))
            this->constrainInterval(summary, variable, interval);
        summary.setAddressSet(variable, addresses);
        // Keep the independently proved unary result together with the affine
        // equality. Only join a saved value from an older visit to this
        // definition (for example, a loop iteration).
        const auto pending = pendingDefinitionHistory_.find(variable);
        if (pending != pendingDefinitionHistory_.end() &&
            pending->second.first == node)
        {
            summary.joinWith(pending->second.second);
            pendingDefinitionHistory_.erase(pending);
        }
        scalarDefinitions_.insert_or_assign(variable, std::move(summary));
        if (std::getenv("SVF_AE_TRACE_SCALAR_SUMMARY"))
            std::cerr << "AE scalar summary assign node=" << node->getId()
                      << " target=" << variable.id()
                      << " shared_function=" << sharedFunction
                      << " cyclic_function=" << cyclicFunction << " state="
                      << scalarDefinitions_.at(variable).numerical().toString()
                      << '\n';
    }
    for (const auto& [source, coefficient] : expression.terms())
    {
        (void)coefficient;
        recordRelationalDependency(variable, source);
    }
}

void SemiSparseAbstractInterpretation::recordRelationalDependency(
    AD::Variable target, AD::Variable source)
{
    if (target == source)
        return;
    const auto dependencies = scalarDependencies_.find(source);
    if (dependencies == scalarDependencies_.end() ||
        dependencies->second.empty())
        scalarDependencies_[target].insert(source);
    else
        scalarDependencies_[target].insert(dependencies->second.begin(),
                                           dependencies->second.end());
}

void SemiSparseAbstractInterpretation::recordRelationalTupleDependency(
    AD::Variable target, AD::Variable peer)
{
    if (target != peer)
        scalarDependencies_[target].insert(peer);
}

void SemiSparseAbstractInterpretation::recordRelationalSummary(
    AD::Variable target, const State& sourceSummary, const ICFGNode* node)
{
    if (Options::AEDomain() == AENumericalDomain::Box)
        return;
    // Phi/select alternatives record syntactic dependencies before their
    // relational states are joined.  A join can eliminate some or all of
    // those relations, so stale branch-local sources must not remain an
    // availability prerequisite for the target's (possibly Top) definition.
    const std::vector<AD::Variable> support =
        sourceSummary.numerical().supportVariables();
    if (std::find(support.begin(), support.end(), target) == support.end())
    {
        scalarDependencies_.erase(target);
    }
    else
    {
        auto dependencies = scalarDependencies_.find(target);
        if (dependencies != scalarDependencies_.end())
        {
            std::set<AD::Variable> supported;
            for (AD::Variable dependency : dependencies->second)
                if (std::find(support.begin(), support.end(), dependency) !=
                    support.end())
                    supported.insert(dependency);
            if (supported.empty())
                scalarDependencies_.erase(dependencies);
            else
                dependencies->second = std::move(supported);
        }
    }
    const std::vector<AD::Variable> variables =
        carrierDependencyClosure({target});
    State summary = this->topState();
    for (AD::Variable variable : variables)
        summary.assignValueFrom(variable, sourceSummary, variable);
    // The unary summary written by updateValue may own target product facets
    // that are absent from the relational source.  Restore only those missing
    // facets: copying the old numerical coordinate would reintroduce a stale
    // bound when a shared callee or loop revisits this SSA definition with a
    // wider input summary.
    const auto intervalSummary = scalarDefinitions_.find(target);
    if (intervalSummary != scalarDefinitions_.end())
    {
        summary.restoreMissingNumericalInitializationFrom(
            intervalSummary->second, target);
        summary.restoreMissingAddressFrom(intervalSummary->second, target);
    }
    State projected = sourceSummary;
    projected.numerical().project(variables);
    summary.numerical().meetWith(projected.numerical());
    const auto pending = pendingDefinitionHistory_.find(target);
    if (pending != pendingDefinitionHistory_.end() &&
        pending->second.first == node)
    {
        summary.joinWith(pending->second.second);
        pendingDefinitionHistory_.erase(pending);
    }
    scalarDefinitions_.insert_or_assign(target, std::move(summary));
    if (std::getenv("SVF_AE_TRACE_SCALAR_SUMMARY"))
        std::cerr << "AE scalar summary merge target=" << target.id()
                  << " dependencies=";
    if (std::getenv("SVF_AE_TRACE_SCALAR_SUMMARY"))
    {
        for (AD::Variable variable : variables)
            std::cerr << variable.id() << ',';
        std::cerr << " state="
                  << scalarDefinitions_.at(target).numerical().toString()
                  << '\n';
    }
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
    const AD::Variable targetVariable = this->adapter_.variable(*target);
    if (Options::AEDomain() == AENumericalDomain::Box)
    {
        // The edge merge computes the return value in the transient flow
        // state.  Publish its scalar facet before finalizeAbstractState drops
        // active ValVars from that state.
        scalarState().assignValueFrom(
            targetVariable, this->state(returnSite), targetVariable);
        return;
    }
    for (const ICFGEdge* edge : returnSite->getInEdges())
    {
        const auto* ret = SVFUtil::dyn_cast<RetCFGEdge>(edge);
        const auto* binding = ret ? ret->getRetPE() : nullptr;
        const auto* source = binding
                             ? SVFUtil::dyn_cast<ValVar>(binding->getRHSVar())
                             : nullptr;
        if (source && this->adapter_.contains(*source))
            recordRelationalDependency(
                targetVariable, this->adapter_.variable(*source));
    }
    recordRelationalSummary(targetVariable, this->state(returnSite),
                            returnSite);
}

void SemiSparseAbstractInterpretation::assignRelationalStore(
    const ValVar* source, AD::Variable content, const ICFGNode* node)
{
    if (!source || !this->adapter_.contains(*source))
        return;
    State& local = this->ensureState(node);
    const AD::Variable sourceVariable = this->adapter_.variable(*source);
    materializeRelations(local, {sourceVariable}, node);
    if (!scalarState().numericalMayBeUninitialized(sourceVariable))
        local.numerical().assign(content,
                                 AD::LinearExpression(sourceVariable));
}

void SemiSparseAbstractInterpretation::assignRelationalLoad(
    const ValVar* target, AD::Variable content, const ICFGNode* node)
{
    if (!target || !this->adapter_.contains(*target))
        return;
    State& local = this->ensureState(node);
    if (local.numericalMayBeUninitialized(content))
        return;
    const AD::Variable targetVariable = this->adapter_.variable(*target);
    local.numerical().assign(targetVariable,
                             AD::LinearExpression(content));

    State projection = local;
    for (AD::Variable variable : projection.numerical().supportVariables())
        if (this->adapter_.contentObject(variable))
            projection.numerical().forget(variable);
    scalarState().numerical().meetWith(projection.numerical());
    for (AD::Variable variable : projection.numerical().supportVariables())
    {
        if (variable != targetVariable &&
            !this->adapter_.contentObject(variable))
            recordRelationalDependency(targetVariable, variable);
    }
    recordRelationalSummary(targetVariable, projection, node);
}

const AD::AbstractDomain* SemiSparseAbstractInterpretation::
getScalarAbstractState() const
{
    return findScalarState();
}

void SemiSparseAbstractInterpretation::handleGlobalNode()
{
    Base::handleGlobalNode();
    finalizeAbstractState(this->icfg->getGlobalICFGNode());
}

AD::Interval SemiSparseAbstractInterpretation::getInterval(
    const ValVar* value, const ICFGNode* node)
{
    const AD::Interval result = getDefinedInterval(value, node);
    if (!value || !this->adapter_.contains(*value))
        return result;
    State materialized = this->topState();
    const AD::Variable variable = this->adapter_.variable(*value);
    const auto definition = scalarDefinitions_.find(variable);
    if (definition != scalarDefinitions_.end())
        materialized.assignValueFrom(variable, definition->second, variable);
    else
        materialized.assignValueFrom(variable, scalarState(), variable);
    if (materialized.numericalMayBeUninitialized(variable))
    {
        if (std::getenv("SVF_AE_TRACE_UNINITIALIZED_READ"))
            std::cerr << "AE uninitialized scalar read node="
                      << (node ? node->getId() : 0)
                      << " variable=" << variable.id()
                      << " svfir=" << value->getId()
                      << " payload=" << result.toString() << '\n';
        return AD::Interval::top();
    }
    return result;
}

AD::Interval SemiSparseAbstractInterpretation::getDefinedInterval(
    const ValVar* value, const ICFGNode* node)
{
    if (const auto* integer = SVFUtil::dyn_cast<ConstIntValVar>(value))
        return AD::Interval::singleton(AD::Rational(integer->getSExtValue()));
    if (const auto* floating = SVFUtil::dyn_cast<ConstFPValVar>(value))
        return SVFIRAdapter::floatingConstant(floating->getFPValue());
    if (!value)
        return AD::Interval::top();
    if (value->getId() == this->svfir->getBlkPtr() ||
            SVFUtil::isa<BlackHoleValVar>(value))
        return Base::getDefinedInterval(value, node);
    if (!this->adapter_.contains(*value))
        return AD::Interval::top();

    // Synthetic copy helpers have no def-site. Original's sparse reader
    // resolves them at the global node, where their numerical value is Top,
    // rather than reading the value written at the generated load's node.
    // Keep this policy for subsequent generated stores as well as queries.
    if (SVFUtil::isa<DummyValVar>(value))
        return AD::Interval::top();

    const AD::Variable variable = this->adapter_.variable(*value);
    const auto available = scalarAvailability_.find(node);
    if (node && (available == scalarAvailability_.end() ||
                 available->second.count(variable) == 0))
        return value->isPointer() ? AD::Interval::bottom()
                                  : AD::Interval::top();
    // A forward reference in a global initializer has not executed its
    // AddrStmt yet. Match the unresolved-symbol policy of the dense reader.
    if ((SVFUtil::isa<FunValVar>(value) || SVFUtil::isa<GlobalValVar>(value)) &&
        !scalarState().hasValue(variable))
        return AD::Interval::top();
    AD::Interval result;
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
        const AD::Interval refined = local.numerical().bound(variable);
        if (!refined.isTop())
        {
            if (result.isBottom())
                result = refined;
            else
                result.meetWith(refined);
        }
    }
    if (Options::AEDomain() != AENumericalDomain::Box && node)
    {
        const auto refinement = refinementTrace_.find(node);
        if (refinement != refinementTrace_.end())
        {
            const AD::Interval refined =
                refinement->second.numerical().bound(variable);
            if (!refined.isTop())
            {
                if (result.isBottom())
                    result = refined;
                else
                    result.meetWith(refined);
            }
        }
    }
    return result;
}

AD::AddressSet SemiSparseAbstractInterpretation::getAddressSet(
    const ValVar* value, const ICFGNode* node)
{
    (void)node;
    if (!value)
        return AD::AddressSet::top();
    if (value->getId() == IRGraph::NullPtr ||
            SVFUtil::isa<ConstNullPtrValVar>(value))
        return AD::AddressSet::singleton(AD::Location::null());
    if (value->getId() == this->svfir->getBlkPtr() ||
            SVFUtil::isa<BlackHoleValVar>(value))
        return this->blackHoleAddressSet();
    if (SVFUtil::isa<DummyValVar>(value))
        return AD::AddressSet::bottom();
    if (!this->adapter_.contains(*value))
        return AD::AddressSet::bottom();
    const AD::Variable variable = this->adapter_.variable(*value);
    const auto available = scalarAvailability_.find(node);
    if (node && (available == scalarAvailability_.end() ||
                 available->second.count(variable) == 0))
        return value->isPointer() ? AD::AddressSet::top()
                                  : AD::AddressSet::bottom();
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
    const ValVar* value, const ICFGNode*) const
{
    if (!value || !this->adapter_.contains(*value))
        return true;
    const AD::Variable variable = this->adapter_.variable(*value);
    const auto definition = scalarDefinitions_.find(variable);
    if (definition != scalarDefinitions_.end())
        return definition->second.numericalMayBeUninitialized(variable);
    return !scalarState_ ||
           scalarState_->numericalMayBeUninitialized(variable);
}

void SemiSparseAbstractInterpretation::updateValue(
    const ValVar* value, const AD::Interval& interval,
    const AD::AddressSet& addresses, const ICFGNode* node)
{
    if (value && this->adapter_.contains(*value))
    {
        const AD::Variable variable = this->adapter_.variable(*value);
        this->assignValue(scalarState(), variable, interval, addresses);
        State summary = this->topState();
        this->assignValue(summary, variable, interval, addresses);
        if (Options::AEDomain() == AENumericalDomain::Box)
        {
            const auto previous = scalarDefinitions_.find(variable);
            if (previous != scalarDefinitions_.end())
                summary.joinWith(previous->second);
        }
        else
        {
            const auto previous = scalarDefinitions_.find(variable);
            if (previous != scalarDefinitions_.end())
                pendingDefinitionHistory_.insert_or_assign(
                    variable, std::make_pair(node, previous->second));
            else
                pendingDefinitionHistory_.erase(variable);
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
        const AD::Variable variable = this->adapter_.variable(*value);
        scalarState().addUninitializedNumericalAlternative(variable);
        const auto summary = scalarDefinitions_.find(variable);
        if (summary != scalarDefinitions_.end())
            summary->second.addUninitializedNumericalAlternative(variable);
    }
}

void SemiSparseAbstractInterpretation::copyAbstractState(
    const ICFGNode* source, const ICFGNode* destination)
{
    if (source == this->icfg->getGlobalICFGNode())
    {
        // Root parameters are initialized on the global state after
        // handleGlobalNode().  Snapshot here, immediately before the root
        // entry copy, so Post reconstruction sees both global initializers
        // and the configured entry arguments, but no later carrier updates.
        globalScalarSnapshot_ = scalarState();
    }
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

SemiSparseAbstractInterpretation::State SemiSparseAbstractInterpretation::
    reconstructPostState(const ICFGNode* node,
                         const std::set<AD::Variable>& availableScalars)
{
    const std::set<AD::Variable>& pointScalars = availableScalars;
    const State& local = this->state(node);
    State reconstructed = this->topState();

    // Copy the complete flow component onto a neutral product. A local reset
    // memory cell is semantically uninitialized, so it must be copied too;
    // treating it as absent would weaken the Post check.
    std::vector<AD::Variable> localNumericalVariables;
    localNumericalVariables.reserve(
        this->adapter_.memoryLayout().cells().size() + pointScalars.size());
    for (const auto& [location, content] :
         this->adapter_.memoryLayout().cells())
    {
        (void)location;
        reconstructed.assignValueFrom(content, local, content);
        localNumericalVariables.push_back(content);
    }
    localNumericalVariables.insert(localNumericalVariables.end(),
                                   pointScalars.begin(), pointScalars.end());
    std::sort(localNumericalVariables.begin(), localNumericalVariables.end());
    localNumericalVariables.erase(std::unique(localNumericalVariables.begin(),
                                              localNumericalVariables.end()),
                                  localNumericalVariables.end());
    reconstructed.lifetimes() = local.lifetimes();

    const State* globalScalars =
        node == this->icfg->getGlobalICFGNode() && globalScalarSnapshot_
        ? &*globalScalarSnapshot_ : nullptr;
    if (globalScalars)
    {
        for (AD::Variable variable : pointScalars)
            reconstructed.assignValueFrom(variable, *globalScalars, variable);
        State projected = *globalScalars;
        const std::vector<AD::Variable> variables(pointScalars.begin(),
                                                  pointScalars.end());
        projected.numerical().project(variables);
        reconstructed.numerical().meetWith(projected.numerical());
    }
    else if (Options::AEDomain() != AENumericalDomain::Box)
    {
        const std::vector<AD::Variable> variables(pointScalars.begin(),
                                                  pointScalars.end());
        materializeScalarDefinitions(reconstructed, variables, node);
    }
    else if (const State* carrier = findScalarState())
    {
        // Copy only point-available scalar facets. Whole-product meet would
        // conflate the carrier's "not stored" defaults with flow-state
        // uninitialized memory coordinates.
        for (AD::Variable variable : pointScalars)
        {
            const auto definition = scalarDefinitions_.find(variable);
            reconstructed.assignValueFrom(
                variable,
                definition != scalarDefinitions_.end()
                    ? definition->second : *carrier,
                variable);
        }
    }

    // Copying individual coordinates above intentionally discards relations.
    // Reapply the projected local component last so that scalar--memory and
    // memory--memory relations survive without meeting unrelated product
    // facets from the sparse carrier.
    State projectedLocal = local;
    projectedLocal.numerical().project(localNumericalVariables);
    reconstructed.numerical().meetWith(projectedLocal.numerical());

    const auto refinement = refinementTrace_.find(node);
    if (refinement != refinementTrace_.end())
    {
        State projected = refinement->second;
        std::vector<AD::Variable> variables(pointScalars.begin(),
                                            pointScalars.end());
        projected.numerical().project(variables);
        applyScalarRefinement(reconstructed, projected);
    }
    return reconstructed;
}

void SemiSparseAbstractInterpretation::normalizePostReplayState(
    State& denseState, const std::set<AD::Variable>& availableScalars) const
{
    const AD::Variable contentBegin =
        this->adapter_.firstObjectContentVariable();
    for (AD::Variable variable : nonDefaultVariables(denseState))
    {
        if (variable < contentBegin && availableScalars.count(variable) == 0)
            this->forgetValue(denseState, variable);
    }
    // Octagon carriers retain forgotten dimensions in their physical layout.
    // Project explicitly so relations through an out-of-scope callee scalar
    // cannot survive normalization and make replay stronger than the return
    // equation's caller-visible state.
    std::vector<AD::Variable> retainedNumerical;
    for (AD::Variable variable : denseState.numerical().supportVariables())
        if (!(variable < contentBegin) ||
                availableScalars.count(variable) != 0)
            retainedNumerical.push_back(variable);
    denseState.numerical().project(retainedNumerical);
}

void SemiSparseAbstractInterpretation::preparePostReplayState(
    State& denseState, const ICFGNode* target,
    const std::set<AD::Variable>& inputScalars) const
{
    (void)inputScalars;
    if (Options::AEDomain() != AENumericalDomain::Box)
        return;
    const auto available = scalarAvailability_.find(target);
    if (available == scalarAvailability_.end())
        return;
    std::set<AD::Variable> targetInputs = available->second;
    for (AD::Variable definition :
            definedScalarVariables(target, this->adapter_))
        targetInputs.erase(definition);
    std::vector<AD::Variable> missingInputs;
    for (AD::Variable variable : targetInputs)
    {
        // Preserve path-local facts already reconstructed at the source.
        // The global per-definition summary is only a fallback for an input
        // that semi-sparse transfer would materialize on demand but dense Post
        // replay cannot otherwise observe.
        if (!denseState.hasValue(variable))
            missingInputs.push_back(variable);
    }
    materializeScalarDefinitions(denseState, missingInputs, target);
}

void SemiSparseAbstractInterpretation::restorePostReplayCallerFrame(
    State& denseState, const RetICFGNode* returnSite,
    const State& callerState,
    const std::set<AD::Variable>& callerScalars) const
{
    if (isSharedCalleeReturn(returnSite))
    {
        // A context-insensitive shared-callee exit can contain scalar
        // coordinates from a different call. SSA caller values are not
        // mutated by the callee, so replace that frame from this call site.
        for (AD::Variable variable : callerScalars)
        {
            this->forgetValue(denseState, variable);
            denseState.assignValueFrom(variable, callerState, variable);
        }
        State projectedCaller = callerState;
        const std::vector<AD::Variable> callerVariables(
            callerScalars.begin(), callerScalars.end());
        projectedCaller.numerical().project(callerVariables);
        denseState.numerical().meetWith(projectedCaller.numerical());
    }
    else
    {
        for (AD::Variable variable : callerScalars)
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
    const AD::Variable contentBegin =
        this->adapter_.firstObjectContentVariable();
    std::set<AD::Variable> retainedScalars;
    if (Options::AEDomain() != AENumericalDomain::Box)
    {
        std::vector<AD::Variable> memorySeeds;
        for (AD::Variable variable : denseState.numerical().supportVariables())
            if (this->adapter_.contentObject(variable))
                memorySeeds.push_back(variable);
        for (AD::Variable variable :
                denseState.numerical().relationalClosure(memorySeeds))
            if (variable < contentBegin)
                retainedScalars.insert(variable);
    }
    for (AD::Variable variable :
            denseState.numerical().supportVariablesBefore(contentBegin))
        if (retainedScalars.count(variable) == 0)
            denseState.numerical().forget(variable);
    for (AD::Variable variable :
            denseState.addresses().nonDefaultVariablesBefore(contentBegin))
        denseState.addresses().forget(variable);
    for (AD::Variable variable : denseState.initializedVariablesBefore(contentBegin))
        if (retainedScalars.count(variable) == 0)
            denseState.resetValue(variable);
}

void SemiSparseAbstractInterpretation::forgetMemoryValues(
    State& denseState) const
{
    for (AD::Variable variable : nonDefaultVariables(denseState))
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
        const AD::AddressSet addresses = caller.addressSet(
                                             this->adapter_.variable(*actual));
        if (addresses.hasUnknownObject())
        {
            exposesAllObjects = true;
            continue;
        }
        for (AD::Location location : addresses)
            expose(this->objectAt(location));
    }

    auto restore = [&](AD::Variable content)
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
    for (AD::Variable content : nonDefaultVariables(caller))
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

void SemiSparseAbstractInterpretation::applyScalarRefinement(
    State& denseState, const State& checkpoint)
{
    if (Options::AEDomain() != AENumericalDomain::Box)
    {
        denseState.numerical().meetWith(checkpoint.numerical());
        return;
    }
    for (AD::Variable variable : checkpoint.numerical().supportVariables())
    {
        const ValVar* value = this->adapter_.value(variable);
        if (!value || value->isPointer())
            continue;
        this->constrainInterval(denseState, variable,
                                checkpoint.numerical().bound(variable));
        denseState.setAddressSet(variable, AD::AddressSet::bottom());
    }
}

void SemiSparseAbstractInterpretation::materializeValue(
    State& denseState, const ValVar* value, const ICFGNode* node)
{
    if (!value || !this->adapter_.contains(*value))
        return;
    const AD::Variable variable = this->adapter_.variable(*value);
    if (value->isPointer())
    {
        denseState.setAddressSet(variable, getAddressSet(value, node));
        denseState.setInterval(variable, AD::Interval::bottom());
    }
    else
    {
        this->constrainInterval(denseState, variable, getInterval(value, node));
        denseState.setAddressSet(variable, AD::AddressSet::bottom());
    }
}

void SemiSparseAbstractInterpretation::materializeRelations(
    State& denseState, const std::vector<AD::Variable>& variables,
    const ICFGNode* node)
{
    if (Options::AEDomain() == AENumericalDomain::Box || variables.empty())
        return;
    State projected = this->topState();
    materializeScalarDefinitions(projected, variables, node);
    denseState.numerical().meetWith(projected.numerical());
}

std::vector<AD::Variable> SemiSparseAbstractInterpretation::
    carrierDependencyClosure(const std::vector<AD::Variable>& seeds) const
{
    std::set<AD::Variable> closure(seeds.begin(), seeds.end());
    std::vector<AD::Variable> worklist(seeds.begin(), seeds.end());
    while (!worklist.empty())
    {
        const AD::Variable variable = worklist.back();
        worklist.pop_back();
        const auto dependencies = scalarDependencies_.find(variable);
        if (dependencies == scalarDependencies_.end())
            continue;
        for (AD::Variable dependency : dependencies->second)
        {
            if (closure.insert(dependency).second)
                worklist.push_back(dependency);
        }
    }
    return std::vector<AD::Variable>(closure.begin(), closure.end());
}

void SemiSparseAbstractInterpretation::materializeScalarDefinitions(
    State& destination, const std::vector<AD::Variable>& seeds,
    const ICFGNode* node) const
{
    if (!node || seeds.empty())
        return;
    const auto available = scalarAvailability_.find(node);
    if (available == scalarAvailability_.end())
        return;

    std::set<AD::Variable> retained;
    for (AD::Variable seed : seeds)
    {
        if (available->second.count(seed) == 0)
            continue;
        // Keep an available definition even when part of its transitive
        // carrier is out of scope at this point.  Projecting the saved
        // summary below existentially forgets those unavailable coordinates;
        // dropping the whole closure would also drop the seed's value and
        // initialization facet.
        retained.insert(seed);
        const std::vector<AD::Variable> closure =
            carrierDependencyClosure({seed});
        for (AD::Variable dependency : closure)
        {
            if (available->second.count(dependency) != 0)
                retained.insert(dependency);
        }
    }
    if (retained.empty())
        return;

    const std::vector<AD::Variable> variables(retained.begin(), retained.end());
    for (AD::Variable variable : variables)
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
    for (AD::Variable variable : variables)
    {
        const auto definition = scalarDefinitions_.find(variable);
        if (definition == scalarDefinitions_.end())
            continue;
        State projected = definition->second;
        projected.numerical().project(variables);
        destination.numerical().meetWith(projected.numerical());
    }
    if (std::getenv("SVF_AE_TRACE_SCALAR_SUMMARY"))
    {
        std::cerr << "AE scalar summary materialize node=" << node->getId()
                  << " seeds=";
        for (AD::Variable variable : seeds)
            std::cerr << variable.id() << ',';
        std::cerr << " retained=";
        for (AD::Variable variable : variables)
            std::cerr << variable.id() << ',';
        std::cerr << " state=" << destination.numerical().toString() << '\n';
    }
}

void SemiSparseAbstractInterpretation::loadValue(
    const ValVar* pointer, AD::Interval& interval, AD::AddressSet& addresses,
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
    const ValVar* pointer, const AD::Interval& interval,
    const AD::AddressSet& addresses, const ICFGNode* node)
{
    Base::storeValue(pointer, interval, addresses, node);
    if (pointer && this->adapter_.contains(*pointer))
        this->forgetValue(this->ensureState(node),
                          this->adapter_.variable(*pointer));
}

void SemiSparseAbstractInterpretation::filterPropagatedState(
    State& denseState) const
{
    (void)denseState;
}

bool SemiSparseAbstractInterpretation::mergeStatesFromPredecessors(
    const ICFGNode* node)
{
    const bool traceMerge = std::getenv("SVF_AE_TRACE_SPARSE_MERGE");
    if (traceMerge)
        std::cerr << "AE sparse merge target=" << node->getId() << '\n';
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
                if (traceMerge)
                    std::cerr << "  predecessor=" << predecessor->getId()
                              << " refinement=bottom\n";
                continue;
            }
        }

        if (traceMerge)
            std::cerr << "  predecessor=" << predecessor->getId()
                      << " refinement="
                      << (refinement ? refinement->numerical().toString()
                                     : std::string("top"))
                      << '\n';

        State source = this->state(predecessor);
        this->applyRelationalCallBoundary(source, edge, node);
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
        if (traceMerge)
            std::cerr << "  result=unreachable\n";
        return false;
    }
    if (const auto* returnSite = SVFUtil::dyn_cast<RetICFGNode>(node))
    {
        const CallICFGNode* call = returnSite->getCallICFGNode();
        const auto callerAvailability = scalarAvailability_.find(call);
        if (call && this->hasAbsState(call) &&
                callerAvailability != scalarAvailability_.end())
            restorePostReplayCallerFrame(
                merged, returnSite, this->state(call),
                callerAvailability->second);
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
    if (traceMerge)
        std::cerr << "  result=" << this->state(node).numerical().toString()
                  << '\n';
    return true;
}

std::unique_ptr<AD::AbstractDomain> SemiSparseAbstractInterpretation::
cloneCycleHeadState(const ICFGCycleWTO* cycle)
{
    const ICFGNode* head = cycle->head()->getICFGNode();
    State snapshot = this->state(head);
    const auto available = scalarAvailability_.find(head);
    std::vector<AD::Variable> cycleVariables;
    for (const ValVar* value : this->preAnalysis->getCycleValVars(cycle))
    {
        if (!value || !this->adapter_.contains(*value))
            continue;
        const AD::Variable variable = this->adapter_.variable(*value);
        if (available != scalarAvailability_.end() &&
            available->second.count(variable) == 0)
            continue;
        cycleVariables.push_back(variable);
    }
    if (Options::AEDomain() == AENumericalDomain::Box)
    {
        for (AD::Variable variable : cycleVariables)
            snapshot.assignValueFrom(variable, scalarState(), variable);
    }
    else
    {
        materializeScalarDefinitions(snapshot, cycleVariables, head);
    }
    const auto refinement = refinementTrace_.find(head);
    if (refinement != refinementTrace_.end())
    {
        State projected = refinement->second;
        projected.numerical().project(cycleVariables);
        applyScalarRefinement(snapshot, projected);
    }
    if (std::getenv("SVF_AE_TRACE_SPARSE_CYCLE"))
        std::cerr << "AE sparse cycle snapshot head=" << head->getId()
                  << " state=" << snapshot.numerical().toString() << '\n';
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
        const AD::Variable variable = this->adapter_.variable(*value);
        if (available != scalarAvailability_.end() &&
            available->second.count(variable) == 0)
            continue;
        // A widened cycle value is a complete product coordinate.  Copy its
        // initialization guards as well as its numerical/address payloads;
        // rebuilding it from the payload alone turns Top into Initialized
        // and loses the distinction between an unknown value and an
        // uninitialized facet.
        scalarState().assignValueFrom(variable, cycleState, variable);
        if (Options::AEDomain() == AENumericalDomain::Box)
        {
            State summary = this->topState();
            summary.assignValueFrom(variable, cycleState, variable);
            scalarDefinitions_.insert_or_assign(variable,
                                                std::move(summary));
        }
        else
        {
            const auto previous = scalarDefinitions_.find(variable);
            if (previous != scalarDefinitions_.end())
                pendingDefinitionHistory_.insert_or_assign(
                    variable, std::make_pair(head, previous->second));
            else
                pendingDefinitionHistory_.erase(variable);

            State summary = this->topState();
            summary.assignValueFrom(variable, cycleState, variable);
            scalarDefinitions_.insert_or_assign(variable,
                                                std::move(summary));
        }
    }
}

bool SemiSparseAbstractInterpretation::widenCycleState(
    const AD::AbstractDomain& previous, const AD::AbstractDomain& current,
    const ICFGCycleWTO* cycle)
{
    const bool fixpoint = Base::widenCycleState(previous, current, cycle);
    if (std::getenv("SVF_AE_TRACE_SPARSE_CYCLE"))
        std::cerr
            << "AE sparse cycle widen head="
            << cycle->head()->getICFGNode()->getId() << " fixpoint=" << fixpoint
            << " state="
            << this->state(cycle->head()->getICFGNode()).numerical().toString()
            << '\n';
    scatterCycleValues(cycle, this->state(cycle->head()->getICFGNode()));
    finalizeAbstractState(cycle->head()->getICFGNode());
    return fixpoint;
}

bool SemiSparseAbstractInterpretation::narrowCycleState(
    const AD::AbstractDomain& previous, const AD::AbstractDomain& current,
    const ICFGCycleWTO* cycle)
{
    const bool fixpoint = Base::narrowCycleState(previous, current, cycle);
    if (std::getenv("SVF_AE_TRACE_SPARSE_CYCLE"))
        std::cerr
            << "AE sparse cycle narrow head="
            << cycle->head()->getICFGNode()->getId() << " fixpoint=" << fixpoint
            << " state="
            << this->state(cycle->head()->getICFGNode()).numerical().toString()
            << '\n';
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
}

FullSparseAbstractInterpretation::
~FullSparseAbstractInterpretation() = default;

void FullSparseAbstractInterpretation::filterPropagatedState(
    State& denseState) const
{
    this->forgetActiveScalarValues(denseState);
    for (AD::Variable variable : nonDefaultVariables(denseState))
    {
        const ObjVar* object = this->adapter_.contentObject(variable);
        if (object && !SVFUtil::isa<GepObjVar>(object) &&
                !denseMemoryVariables_.count(variable))
            this->forgetValue(denseState, variable);
    }
}

void FullSparseAbstractInterpretation::recordBranchRefinement(
    NodeID objectId, const AD::Interval& narrowed, AD::AbstractDomain&,
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
    const ValVar* pointer, const AD::Interval& interval,
    const AD::AddressSet& valueAddresses, const ICFGNode* node)
{
    const AD::AddressSet addresses = Base::getAddressSet(pointer, node);
    auto refinement = memoryRefinementTrace_.find(node);
    if (refinement != memoryRefinementTrace_.end() &&
            !addresses.hasUnknownObject())
    {
        for (AD::Location location : addresses)
            if (const ObjVar* object = this->objectAt(location))
                refinement->second.erase(object->getId());
    }
    Base::storeValue(pointer, interval, valueAddresses, node);
    recordMemoryDefinition(node, addresses);
}

void FullSparseAbstractInterpretation::recordMemoryDefinition(
    const ICFGNode* node, const AD::AddressSet& targets)
{
    auto record = [&](AD::Location location)
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
            if (this->unknownTargetTelemetryEnabled())
                ++this->unknownTargetTelemetry()
                .sparseDefinitionCellsVisited;
            (void)content;
            record(location);
        }
    }
    else
    {
        for (AD::Location location : targets.locations())
            record(location);
    }
}

bool FullSparseAbstractInterpretation::hasMemoryDefinition(
    const ICFGNode* node, AD::Variable content) const
{
    const auto support = memoryDefinitionSupport_.find(node);
    return support != memoryDefinitionSupport_.end() &&
           support->second.count(content) != 0;
}

void FullSparseAbstractInterpretation::updateMemoryValue(
    AD::Location location, const AD::Interval& interval,
    const AD::AddressSet& addresses, const ICFGNode* node)
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
        filterPropagatedState(this->ensureState(node));
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
    const AD::AbstractDomain& previous, const AD::AbstractDomain& current,
    const ICFGCycleWTO* cycle)
{
    const ICFGNode* head = cycle->head()->getICFGNode();
    const auto old = previousMemoryDefinitionSupport_.find(head);
    const std::set<AD::Variable> empty;
    const std::set<AD::Variable>& oldSupport =
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
    const AD::AbstractDomain& previous, const AD::AbstractDomain& current,
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
    for (AD::Variable variable : nonDefaultVariables(destination))
    {
        const ObjVar* object = this->adapter_.contentObject(variable);
        if (object && SVFUtil::isa<GepObjVar>(object))
            denseLocalObjects.set(object->getId());
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
                    const AD::Variable content =
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
                    const AD::Variable destinationContent =
                        this->memoryVariable(*object, destination);
                    const AD::Variable sourceContent =
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
    Map<NodeID, AD::Interval> inherited;
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
        const AD::Variable content = this->adapter_.contentVariable(*object);
        if (!object->isPointer())
            this->constrainInterval(denseState, content, constraint);
    }
}

} // namespace SVF
