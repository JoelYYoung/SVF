//===- VersionedSparseAbstractInterpretation.h -- D3 relational AE -*- C++ -*-//
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
//===----------------------------------------------------------------------===//

#ifndef SVF_AE_VERSIONED_SPARSE_ABSTRACT_INTERPRETATION_H
#define SVF_AE_VERSIONED_SPARSE_ABSTRACT_INTERPRETATION_H

#include <map>
#include <memory>
#include <set>
#include <vector>

#include "AE/Svfexe/SparseAbstractInterpretation.h"

namespace SVF
{

/// D3 relational sparse AE (-ae-sparsity=d3).
///
/// Scalars keep the semi-sparse definition-site summaries, dependency closure
/// and branch-refinement channel. Numerical memory contents are replaced by
/// MemorySSA content versions: every store defines one version per possibly
/// written object, merges whose predecessors disagree define a memory phi
/// version, and a load's summary is `target = reaching version` plus the
/// version's closure. Versions are immutable coordinates, so an overwritten
/// cell never invalidates a relation recorded against an older version.
///
/// The version map is computed before numerical solving by a reaching-version
/// dataflow over the ICFG; it carries version identities only. Persistent
/// flow states keep address, initialization and lifetime facets but no
/// numerical memory payload. Post replay rebuilds numerical contents from the
/// reaching versions.
///
/// v1 limits (sound, reported rather than hidden): a memory phi at a WTO
/// cycle head is not versioned (the object reads as Top inside the cycle);
/// memory phis are not grouped with scalar phis of the same block; external
/// calls invalidate the versions of objects reachable from pointer arguments;
/// a store whose points-to set contains the black-hole object invalidates
/// every version.
class VersionedSparseAbstractInterpretation
    : public SemiSparseAbstractInterpretation
{
public:
    using Base = SemiSparseAbstractInterpretation;
    using State = typename Base::State;

    VersionedSparseAbstractInterpretation();
    ~VersionedSparseAbstractInterpretation() override;

protected:
    void handleSVFStatement(const SVFStmt* stmt) override;
    bool mergeStatesFromPredecessors(const ICFGNode* node) override;
    void filterPropagatedState(State& state) const override;
    State reconstructPostState(
        const ICFGNode* node,
        const std::set<AbstractDomain::Variable>& availableScalars) override;
    /// Memory relations are owned by content versions, not the flow state.
    void assignRelationalStore(const ValVar* source,
                               AbstractDomain::Variable content,
                               const ICFGNode* node) override;
    void assignRelationalLoad(const ValVar* target,
                              AbstractDomain::Variable content,
                              const ICFGNode* node) override;

private:
    /// Object id -> content version reaching a program point.
    using VersionMap = std::map<NodeID, AbstractDomain::Variable>;
    using VersionMapPtr = std::shared_ptr<const VersionMap>;

    struct StoreVersion
    {
        NodeID object;
        AbstractDomain::Variable version;
        bool hasPrevious;
        AbstractDomain::Variable previous;
    };

    void collectCycleHeads();
    void buildMemoryVersions();
    void computeVersionAvailability();
    std::vector<const ObjVar*> staticTargets(const SVFVar* pointer,
                                             bool& havocAll) const;
    AbstractDomain::Variable versionFor(
        std::map<std::pair<const void*, NodeID>, AbstractDomain::Variable>&
            table,
        const void* site, NodeID object);
    VersionMapPtr mergeIncoming(const ICFGNode* node);
    VersionMapPtr transferNode(const ICFGNode* node, VersionMapPtr in);
    bool strongUpdatable(NodeID object) const;

    void versionStore(const StoreStmt* store);
    void versionLoad(const LoadStmt* load);
    void versionPhis(const ICFGNode* node);
    void recordVersionSummary(AbstractDomain::Variable version,
                              State summary, const ICFGNode* node);
    State materializedAt(const std::vector<AbstractDomain::Variable>& seeds,
                         const ICFGNode* node);
    void projectRefinementToAvailable(const ICFGNode* node);
    void reportTelemetry() const;

    std::set<const ICFGNode*> cycleHeads_;
    std::map<const ICFGNode*, VersionMapPtr> versionOut_;
    std::map<const StoreStmt*, std::vector<StoreVersion>> storeVersions_;
    std::map<const LoadStmt*, VersionMapPtr> loadVersions_;
    std::set<const StoreStmt*> havocStores_;
    std::map<const ICFGNode*, std::vector<std::pair<NodeID,
             AbstractDomain::Variable>>> phiVersions_;
    std::map<std::pair<const void*, NodeID>, AbstractDomain::Variable>
        storeVersionIds_;
    std::map<std::pair<const void*, NodeID>, AbstractDomain::Variable>
        phiVersionIds_;
    std::map<const ICFGNode*, std::set<AbstractDomain::Variable>>
        versionAvailability_;
    std::set<AbstractDomain::Variable> versionVariables_;

    mutable std::size_t cycleHeadDrops_ = 0;
    std::size_t externalInvalidations_ = 0;
    std::size_t storeEscapes_ = 0;
    std::size_t loadMisses_ = 0;
};

} // namespace SVF

#endif // SVF_AE_VERSIONED_SPARSE_ABSTRACT_INTERPRETATION_H
