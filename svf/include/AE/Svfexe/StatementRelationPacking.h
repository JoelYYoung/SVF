//===- StatementRelationPacking.h -- Statement Relation Packing --------===//
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
#ifndef SVF_AE_STATEMENT_RELATION_PACKING_H
#define SVF_AE_STATEMENT_RELATION_PACKING_H

#include "AE/Core/RelationPacking.h"

namespace SVF
{
class SVFIR;
class SVFIRAdapter;
class PointerAnalysis;
class AllocationSizeModel;

struct StatementPackingEdge
{
    SVF::RelationEdge variables;
    unsigned statement;
    const char* kind;
};

struct StatementPackingResult
{
    SVF::RelationPacking packs;
    std::vector<StatementPackingEdge> edges;
};

/// Bind the program context once, then pack any permitted numerical vocabulary.
/// Points-to identifies possible content dependencies, never numeric facts.
class StatementRelationPacking
{
public:
    /// With `sizes`, D-005 adds Size(o)-size and Size(o)-index edges.
    StatementRelationPacking(SVFIR& ir, const SVFIRAdapter& adapter,
                             PointerAnalysis& pointsTo,
                             const AllocationSizeModel* sizes = nullptr)
        : ir_(ir), adapter_(adapter), pointsTo_(pointsTo), sizes_(sizes) {}
    StatementPackingResult pack(
        const std::vector<SVF::Variable>& vocabulary) const;
private:
    SVFIR& ir_;
    const SVFIRAdapter& adapter_;
    PointerAnalysis& pointsTo_;
    const AllocationSizeModel* sizes_;
};
}
#endif
