// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef SVF_AE_RELATION_PACKING_H
#define SVF_AE_RELATION_PACKING_H

#include "AE/Core/Variable.h"
#include <utility>
#include <vector>

namespace SVF::AbstractDomain
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
