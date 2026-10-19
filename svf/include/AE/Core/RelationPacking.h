//===- RelationPacking.h -- Relation Packing --------===//
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
#ifndef SVF_AE_RELATION_PACKING_H
#define SVF_AE_RELATION_PACKING_H

#include "AE/Core/AbstractDomain.h"
#include <utility>
#include <vector>

namespace SVF
{
using RelationEdge = std::pair<Variable, Variable>;
using RelationPacking = std::vector<std::vector<Variable>>;

/// Connected components of the graph induced by vocabulary. Edges with either
/// endpoint outside it are ignored; excluded intermediates cannot merge packs.
/// Covers each distinct vocabulary member once, including isolated vertices.
/// Packs and their members are canonical, with no size cap or truncation.
RelationPacking connectedComponentPacking(
    const std::vector<Variable>& vocabulary,
    const std::vector<RelationEdge>& edges);
}
#endif
