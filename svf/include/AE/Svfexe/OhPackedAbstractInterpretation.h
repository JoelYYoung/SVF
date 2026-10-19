//===- OhPackedAbstractInterpretation.h -- Oh Packed Abstract Interpretation --------===//
//
//                     SVF: Static Value-Flow Analysis
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
    OhPackedAbstractInterpretation() = default;
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
    bool widenCycleState(const SVF::AbstractDomain&,
                         const SVF::AbstractDomain&, const ICFGCycleWTO*) override;
    bool narrowCycleState(const SVF::AbstractDomain&,
                          const SVF::AbstractDomain&, const ICFGCycleWTO*) override;
private:
    using Pack = SVF::OctagonDomain;
    using Defs = std::set<std::size_t>;
    std::map<const ICFGNode*, Defs> definitions_;
    std::map<const ICFGNode*, std::map<std::size_t,const ICFGNode*>> routes_;
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
