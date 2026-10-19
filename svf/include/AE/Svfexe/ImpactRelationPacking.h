//===- ImpactRelationPacking.h -- Impact Relation Packing --------===//
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
#ifndef SVF_AE_IMPACT_RELATION_PACKING_H
#define SVF_AE_IMPACT_RELATION_PACKING_H
#include "AE/Core/RelationPacking.h"
#include "Util/GeneralType.h"
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>

namespace SVF
{
class SVFIR;
class SVFIRAdapter;
class PointerAnalysis;
class ICFGNode;
class AllocationSizeModel;
struct ImpactPackingQuery
{
    unsigned node;
    std::string kind;
    bool supported;
    bool promising;
    std::vector<SVF::RelationEdge> snapshot;
};
struct ImpactPackingResult
{
    SVF::RelationPacking packs;
    std::vector<SVF::RelationEdge> edges;
    std::vector<ImpactPackingQuery> queries;
    std::size_t transfers = 0;
    std::size_t commands = 0;
    std::size_t phiFallbacks = 0;
    double preanalysisSeconds = 0;
    double extractionSeconds = 0;
};

/// Port of the public Sparrow octImpact pre-analysis (see source attribution in ImpactRelationPacking.cpp;
/// pinned upstream revision). Each ICFG node and edge is
/// compiled once into Sparrow commands (set/forget/assume over V, including
/// Size(o) coordinates); a reverse-postorder worklist runs them with
/// Sparrow's accumulate-and-widen iteration. Only Andersen points-to is used.
class ImpactRelationPacking
{
public:
    ImpactRelationPacking(SVFIR& ir, const SVFIRAdapter& adapter,
                          PointerAnalysis& pointsTo,
                          std::vector<const ICFGNode*> entries = {},
                          const AllocationSizeModel* sizes = nullptr);
    ~ImpactRelationPacking();
    ImpactPackingResult pack(const std::vector<SVF::Variable>& vocabulary) const;

    /// LLVM debug-info source variable of an SSA value (0 when unknown),
    /// registered by the LLVM front end. Sparrow forgets x for x := x + 1;
    /// after mem2reg both SSA names map to the same source variable.
    static void setSourceVariables(std::map<NodeID, std::uintptr_t> sourceVariables);

private:
    SVFIR& ir_;
    const SVFIRAdapter& adapter_;
    PointerAnalysis& pointsTo_;
    std::vector<const ICFGNode*> entries_;
    const AllocationSizeModel* sizes_;
    std::unique_ptr<AllocationSizeModel> ownedSizes_;
};
}
#endif
