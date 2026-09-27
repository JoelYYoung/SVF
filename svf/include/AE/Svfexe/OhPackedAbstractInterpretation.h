// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "AE/Svfexe/AbstractInterpretation.h"
#include "AE/Core/PackedRelationalDomain.h"
#include <map>
#include <set>

namespace SVF
{
/// Static pack-location Def/Use propagation. Numeric identity components are
/// routed to reaching pack definitions; Box, addresses and guards flow densely.
/// The shared WTO driver fixes scheduling/widening. This is independent of the
/// ValVar def-site reconstruction in SemiSparseAbstractInterpretation.
class OhPackedAbstractInterpretation final : public AbstractInterpretation
{
public:
    OhPackedAbstractInterpretation() : AbstractInterpretation(false) {}
    void runOnModule() override;
protected:
    void initializeExecutionPolicy() override;
    void beginAbstractState(const ICFGNode*) override;
    void finalizeAbstractState(const ICFGNode*) override;
    State& ensureState(const ICFGNode*) override;
    const State& state(const ICFGNode*) const override;
    bool mergeStatesFromPredecessors(const ICFGNode*) override;
    void resetAbstractState(const ICFGNode*) override;
    void copyAbstractState(const ICFGNode*, const ICFGNode*) override;
    void handleGlobalNode() override;
    bool widenCycleState(const AbstractDomain::AbstractDomain&,
        const AbstractDomain::AbstractDomain&, const ICFGCycleWTO*) override;
    bool narrowCycleState(const AbstractDomain::AbstractDomain&,
        const AbstractDomain::AbstractDomain&, const ICFGCycleWTO*) override;
private:
    using Pack = AbstractDomain::OctagonDomain;
    using Defs = std::set<std::size_t>;
    std::map<const ICFGNode*, Defs> definitions_;
    std::map<const ICFGNode*, std::vector<const ICFGNode*>> routes_;
    std::map<const ICFGNode*, std::map<std::size_t, Pack>> storedPacks_;
    mutable std::map<const ICFGNode*, std::unique_ptr<State>> readCache_;
    // Preserve references returned before a mutable borrow until the next
    // node/merge boundary; new reads must reconstruct from current definitions.
    std::vector<std::unique_ptr<State>> retiredReadCache_;
    mutable std::size_t peakReadCacheNodes_ = 0;
    std::map<const ICFGNode*, unsigned> active_;
    std::set<const ICFGNode*> expanded_;
    std::size_t restored_ = 0, omittedChecks_ = 0;
    double graphSeconds_ = 0;
    State reconstruct(const ICFGNode*) const;
    const Pack& definition(const ICFGNode*, std::size_t) const;
    void compact(const ICFGNode*);
    void compactBorrowed();
    void invalidateReadCache();
    void checkCacheInvalidation();
};
}
