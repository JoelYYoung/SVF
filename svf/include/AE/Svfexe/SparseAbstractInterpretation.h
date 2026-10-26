//===- SparseAbstractInterpretation.h -- Sparse box/address AE -*- C++ -*-===//
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

#ifndef SVF_AE_SPARSE_ABSTRACT_INTERPRETATION_H
#define SVF_AE_SPARSE_ABSTRACT_INTERPRETATION_H

#include <map>
#include <memory>
#include <optional>
#include <set>

#include "AE/Svfexe/AbstractInterpretation.h"

namespace SVF
{

class IndirectSVFGEdge;
class SVFGBuilder;
class VFGNode;

/// Semi-sparse AE backed by BoxAddressDomain. Box values use one module-wide
/// scalar carrier. Persistent ICFG states carry memory and lifetime values,
/// while transfers materialize scalar operands only temporarily.
class SemiSparseAbstractInterpretation : public AbstractInterpretation
{
public:
    using Base = AbstractInterpretation;
    using State = typename Base::State;

    SemiSparseAbstractInterpretation();
    ~SemiSparseAbstractInterpretation() override = default;
    const SVF::AbstractDomain* getScalarAbstractState()
    const override;

protected:
    void handleGlobalNode() override;
    SVF::Interval getInterval(const ValVar* var,
                              const ICFGNode* node) override;
    SVF::Interval getDefinedInterval(
        const ValVar* var, const ICFGNode* node) override;
    SVF::AddressSet getAddressSet(const ValVar* var,
                                  const ICFGNode* node) override;
    using Base::getAddressSet;
    using Base::getInterval;
    bool hasAbsValue(const ValVar* var, const ICFGNode* node) const override;
    using Base::hasAbsValue;
    bool numericalValueMayBeUninitialized(
        const ValVar* var, const ICFGNode* node) const override;
    void updateValue(const ValVar* var,
                     const SVF::Interval& interval,
                     const SVF::AddressSet& addresses,
                     const ICFGNode* node) override;
    void addUninitializedNumericalAlternative(
        const ValVar* var, const ICFGNode* node) override;
    using Base::updateValue;

    void copyAbstractState(const ICFGNode* source,
                           const ICFGNode* destination) override;
    void resetAbstractState(const ICFGNode* node) override;
    void finalizeAbstractState(const ICFGNode* node) override;
    bool mergeStatesFromPredecessors(const ICFGNode* node) override;
    void restoreCallerFrame(
        State& state, const RetICFGNode* returnSite,
        const State& callerState,
        const std::set<SVF::Variable>& callerScalars) const;

    std::unique_ptr<SVF::AbstractDomain> cloneCycleHeadState(
        const ICFGCycleWTO* cycle) override;
    bool widenCycleState(const SVF::AbstractDomain& previous,
                         const SVF::AbstractDomain& current,
                         const ICFGCycleWTO* cycle) override;
    bool narrowCycleState(const SVF::AbstractDomain& previous,
                          const SVF::AbstractDomain& current,
                          const ICFGCycleWTO* cycle) override;

    void materializeValue(State& state, const ValVar* value,
                          const ICFGNode* node) override;
    void materializeRelations(
        State& state, const std::vector<SVF::Variable>& variables,
        const ICFGNode* node) override;
    State& scalarTransferState(const ICFGNode* node) override;
    State phiAlternativeState(const ICFGNode* predecessor) override;
    void recordMergedReturnSummary(
        const RetICFGNode* returnSite) override;
    void loadValue(const ValVar* pointer, SVF::Interval& interval,
                   SVF::AddressSet& addresses,
                   bool& numericalMayBeUninitialized,
                   const ICFGNode* node) override;
    void storeValue(const ValVar* pointer,
                    const SVF::Interval& interval,
                    const SVF::AddressSet& addresses,
                    const ICFGNode* node) override;

    /// Select the state facets carried by ordinary ICFG edges.
    virtual State propagatedState(const State& state) const;
    virtual void filterPropagatedState(State& state) const;

    State& scalarState();
    const State* findScalarState() const;
    State flowState(bool bottom = false) const;
    void forgetActiveScalarValues(State& state) const;
    void forgetMemoryValues(State& state) const;
    void restoreCallerFrameAfterSharedCallee(
        State& state, const RetICFGNode* returnSite,
        const State* callerOverride = nullptr) const;
    bool isSharedFunction(const FunObjVar* function) const;
    bool isSharedCalleeReturn(const RetICFGNode* returnSite) const;
    /// Shared-callee admission used by production restoration and derived
    /// executors. Preservation still applies only to available SSA names
    /// owned by the current caller; this is not a memory-frame permission.
    bool preservesCallerFrame(const RetICFGNode* returnSite) const;
    bool callerFrameCannotBeReentered(const RetICFGNode* returnSite) const;
    mutable Map<const RetICFGNode*, bool> nonReentrantReturnCache_;
    void applyScalarRefinement(State& state, const State& checkpoint);
    void scatterCycleValues(const ICFGCycleWTO* cycle, const State& state);
    void initializeScalarAvailability();
    std::vector<SVF::Variable> carrierDependencyClosure(
        const std::vector<SVF::Variable>& seeds) const;
    void materializeScalarDefinitions(
        State& state, const std::vector<SVF::Variable>& seeds,
        const ICFGNode* node,
        const std::set<SVF::Variable>* availableOverride = nullptr) const;
    /// Coordinates available at `node` beyond the scalar availability sets
    /// (for example D3 content versions). They are materialized only when a
    /// seed or dependency names them, never as an "all available" seed.
    virtual bool extraAvailable(const ICFGNode*, SVF::Variable) const
    {
        return false;
    }

    Map<const ICFGNode*, State> refinementTrace_;
    bool isScalarAvailable(const ICFGNode* node,
                           SVF::Variable variable) const;
    std::vector<SVF::Variable> availableScalarVariables(
        const ICFGNode* node) const;

    Map<const ICFGNode*, NodeBS> scalarAvailability_;
    std::map<SVF::Variable, NodeID> scalarSymbols_;
    std::vector<SVF::Variable> scalarVariables_;
    std::map<SVF::Variable, std::set<SVF::Variable>>
    scalarDependencies_;
    std::map<SVF::Variable, State> scalarDefinitions_;
    std::map<SVF::Variable, std::pair<const ICFGNode*, State>>
    pendingDefinitionHistory_;
    std::optional<State> scalarState_;
};

/// Full-sparse AE backed by BoxAddressDomain. Scalar SSA values share the same
/// module-wide scalar carrier as semi-sparse mode. Base/Dummy ObjVar contents
/// move along MemorySSA/SVFG def-use edges; GepObjVar snapshots and lifetime
/// facts continue to flow along the ICFG because those edges do not fully
/// represent them.
class FullSparseAbstractInterpretation
    : public SemiSparseAbstractInterpretation
{
public:
    using Base = SemiSparseAbstractInterpretation;
    using State = typename Base::State;

    FullSparseAbstractInterpretation();
    ~FullSparseAbstractInterpretation() override;

protected:
    bool mergeStatesFromPredecessors(const ICFGNode* node) override;
    bool widenCycleState(const SVF::AbstractDomain& previous,
                         const SVF::AbstractDomain& current,
                         const ICFGCycleWTO* cycle) override;
    bool narrowCycleState(const SVF::AbstractDomain& previous,
                          const SVF::AbstractDomain& current,
                          const ICFGCycleWTO* cycle) override;
    void storeValue(const ValVar* pointer,
                    const SVF::Interval& interval,
                    const SVF::AddressSet& addresses,
                    const ICFGNode* node) override;
    void updateMemoryValue(SVF::Location location,
                           const SVF::Interval& interval,
                           const SVF::AddressSet& addresses,
                           const ICFGNode* node) override;
    State propagatedState(const State& state) const override;
    void classifyMemoryVariables() const;
    void recordBranchRefinement(NodeID objectId,
                                const SVF::Interval& narrowed,
                                SVF::AbstractDomain& state,
                                const ICFGNode* loadNode,
                                const ICFGNode* successor) override;

private:
    void recordMemoryDefinition(const ICFGNode* node,
                                const SVF::AddressSet& targets);
    bool hasMemoryDefinition(const ICFGNode* node,
                             SVF::Variable content) const;
    void pullObjectValueFlows(const ICFGNode* node);
    bool isIndirectSVFGEdgeFeasible(const IndirectSVFGEdge* edge,
                                    const VFGNode* destination);
    bool isIntraEdgeBranchFeasible(const IntraCFGEdge* edge,
                                   const ICFGNode* source);
    void propagateAndApplyMemoryRefinement(const ICFGNode* node);

    Map<const ICFGNode*, Map<NodeID, SVF::Interval>>
    memoryRefinementTrace_;
    /// Contents changed through analyzer-side models rather than StoreStmt.
    /// SVFG has no defining edge for them, so they retain ICFG propagation.
    std::set<SVF::Variable> denseMemoryVariables_;
    mutable SVF::Location classifiedMemoryLocation_;
    mutable std::set<SVF::Variable> svfgMemoryVariables_;
    mutable std::map<SVF::Variable, NodeID> fieldVariables_;
    /// SVFG definition support is separate from abstract values. A numeric
    /// Top has no Box slot, but a StoreStmt that produces it must still be
    /// distinguishable from an SVFG source that contributed no definition.
    Map<const ICFGNode*, std::set<SVF::Variable>>
    memoryDefinitionSupport_;
    Map<const ICFGNode*, std::set<SVF::Variable>>
    previousMemoryDefinitionSupport_;
    std::unique_ptr<SVFGBuilder> svfgBuilder_;
};

} // namespace SVF

#endif // SVF_AE_SPARSE_ABSTRACT_INTERPRETATION_H
