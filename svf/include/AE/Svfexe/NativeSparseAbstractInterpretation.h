//===- NativeSparseAbstractInterpretation.h -- Domain sparse AE -*- C++ -*-===//

#ifndef SVF_AE_NATIVE_SPARSE_ABSTRACT_INTERPRETATION_H
#define SVF_AE_NATIVE_SPARSE_ABSTRACT_INTERPRETATION_H

#include <memory>

#include "AE/Svfexe/DenseAbstractInterpretation.h"

namespace SVF
{

class IndirectSVFGEdge;
class SVFGBuilder;
class VFGNode;

/// Semi-sparse AE backed by DomainProductState. Scalar SSA values live at
/// their definition sites; CFG states carry memory, lifetime, and the
/// temporary relational slices required for branch and cycle operations.
template <typename NumericalStateT>
class NativeSemiSparseAbstractInterpretation
    : public DenseAbstractInterpretation<NumericalStateT>
{
public:
    using Base = DenseAbstractInterpretation<NumericalStateT>;
    using DenseState = typename Base::DenseState;

    NativeSemiSparseAbstractInterpretation();
    ~NativeSemiSparseAbstractInterpretation() override = default;

protected:
    AbstractValue getAbsValue(const ValVar* var, const ICFGNode* node) override;
    using Base::getAbsValue;
    bool hasAbsValue(const ValVar* var, const ICFGNode* node) const override;
    using Base::hasAbsValue;
    void updateAbsValue(const ValVar* var, const AbstractValue& value,
                        const ICFGNode* node) override;
    using Base::updateAbsValue;

    void copyAbstractState(const ICFGNode* source,
                           const ICFGNode* destination) override;
    bool mergeStatesFromPredecessors(const ICFGNode* node) override;

    std::unique_ptr<AbstractDomain::AbstractState> cloneCycleHeadState(
        const ICFGCycleWTO* cycle) override;
    bool widenCycleState(const AbstractDomain::AbstractState& previous,
                         const AbstractDomain::AbstractState& current,
                         const ICFGCycleWTO* cycle) override;
    bool narrowCycleState(const AbstractDomain::AbstractState& previous,
                          const AbstractDomain::AbstractState& current,
                          const ICFGCycleWTO* cycle) override;

    void assignDomainInterval(const ICFGNode* node, const SVFVar* target,
                              const IntervalValue& interval) override;
    void materializeValue(DenseState& state, const ValVar* value,
                          const ICFGNode* node) override;

    /// Keep only the state facets that should flow along ordinary ICFG
    /// edges. Full-sparse overrides this to remove MemorySSA-managed objects.
    virtual void filterPropagatedState(DenseState& state) const;

    /// Optional memory refinement hook after a conditional edge has been
    /// proven feasible by the native numerical state.
    virtual void collectMemoryBranchRefinement(const IntraCFGEdge* edge);

    const ICFGNode* definitionNode(const ValVar* value,
                                   const ICFGNode* fallback) const;
    void forgetMemoryValues(DenseState& state) const;
    void applyScalarCheckpoint(DenseState& state, const DenseState& checkpoint);
    void scatterCycleValues(const ICFGCycleWTO* cycle, const DenseState& state);

    Map<const ICFGNode*, DenseState> refinementTrace_;
};

/// Full-sparse AE backed by DomainProductState. Scalar SSA values remain at
/// definition sites as in semi-sparse mode. Base/Dummy ObjVar contents move
/// along MemorySSA/SVFG def-use edges; GepObjVar snapshots and lifetime facts
/// continue to flow along the ICFG because they are not fully represented by
/// those edges.
template <typename NumericalStateT>
class NativeFullSparseAbstractInterpretation
    : public NativeSemiSparseAbstractInterpretation<NumericalStateT>
{
public:
    using Base = NativeSemiSparseAbstractInterpretation<NumericalStateT>;
    using DenseState = typename Base::DenseState;

    NativeFullSparseAbstractInterpretation();
    ~NativeFullSparseAbstractInterpretation() override;

protected:
    bool mergeStatesFromPredecessors(const ICFGNode* node) override;
    void storeValue(const ValVar* pointer, const AbstractValue& value,
                    const ICFGNode* node) override;
    void filterPropagatedState(DenseState& state) const override;
    void collectMemoryBranchRefinement(const IntraCFGEdge* edge) override;
    void recordBranchRefinement(NodeID objectId, const IntervalValue& narrowed,
                                IntervalState& state, const ICFGNode* loadNode,
                                const ICFGNode* successor) override;

private:
    void pullObjectValueFlows(const ICFGNode* node);
    bool isIndirectSVFGEdgeFeasible(const IndirectSVFGEdge* edge,
                                    const VFGNode* destination);
    bool isIntraEdgeBranchFeasible(const IntraCFGEdge* edge,
                                   const ICFGNode* source);
    void propagateAndApplyMemoryRefinement(const ICFGNode* node);

    Map<const ICFGNode*, Map<NodeID, IntervalValue>> memoryRefinementTrace_;
    std::unique_ptr<SVFGBuilder> svfgBuilder_;
};

extern template class NativeSemiSparseAbstractInterpretation<
    AbstractDomain::BoxState>;
extern template class NativeSemiSparseAbstractInterpretation<
    AbstractDomain::OctagonState>;
extern template class NativeFullSparseAbstractInterpretation<
    AbstractDomain::BoxState>;
extern template class NativeFullSparseAbstractInterpretation<
    AbstractDomain::OctagonState>;

} // namespace SVF

#endif // SVF_AE_NATIVE_SPARSE_ABSTRACT_INTERPRETATION_H
