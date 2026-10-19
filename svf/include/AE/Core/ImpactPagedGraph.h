//===- ImpactPagedGraph.h -- Impact Paged Graph --------===//
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
#ifndef SVF_AE_IMPACT_PAGED_GRAPH_H
#define SVF_AE_IMPACT_PAGED_GRAPH_H
#include <array>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace SVF
{
/// Production representation of Sparrow's AbsOct (octImpactDom.ml) over dense
/// coordinate indices. Every component uses a two-level
/// copy-on-write structure, so copying a state copies three pointers and an
/// update clones one directory and one page. Out- and in-adjacency are both
/// kept so forget(x) touches only x's neighbours.
class ImpactPagedGraph
{
public:
    using Index = std::uint32_t;
    using Edge = std::pair<Index, Index>;

    bool bottom = true;

    static ImpactPagedGraph top();
    void forget(Index x);
    void setConstant(Index x, bool strong);
    void setVariable(Index x, Index y, bool strong);
    void assume(Index x, Index y);
    ImpactPagedGraph join(const ImpactPagedGraph& other) const;
    bool le(const ImpactPagedGraph& other) const;
    bool hasPath(Index source, Index target) const;
    bool operator==(const ImpactPagedGraph& other) const;
    bool operator!=(const ImpactPagedGraph& other) const
    {
        return !(*this == other);
    }

    std::vector<Edge> edges() const;
    std::vector<Index> defined() const;
    std::vector<Index> constants() const;
    bool isDefined(Index x) const
    {
        return test(defined_, x);
    }

private:
    static constexpr unsigned WordsPerPage = 64;          // 4096 bits
    static constexpr unsigned BitsPerPage = WordsPerPage * 64;
    static constexpr unsigned NodesPerAdjacencyPage = 256;
    using BitPage = std::array<std::uint64_t, WordsPerPage>;
    using BitDirectory = std::vector<std::shared_ptr<const BitPage>>;
    /// Sorted (owner, neighbour) pairs whose owner falls in the page range.
    using AdjacencyPage = std::vector<Edge>;
    using AdjacencyDirectory = std::vector<std::shared_ptr<const AdjacencyPage>>;

    static bool test(const std::shared_ptr<const BitDirectory>& bits, Index x);
    static void assign(std::shared_ptr<const BitDirectory>& bits, Index x, bool value);
    static bool contains(const std::shared_ptr<const AdjacencyDirectory>& adjacency,
                         Index owner, Index neighbour);
    static void insert(std::shared_ptr<const AdjacencyDirectory>& adjacency,
                       Index owner, Index neighbour);
    static std::vector<Index> removeOwner(std::shared_ptr<const AdjacencyDirectory>& adjacency,
                                          Index owner);
    static void erase(std::shared_ptr<const AdjacencyDirectory>& adjacency,
                      Index owner, Index neighbour);
    void addEdge(Index source, Index target);
    std::shared_ptr<const AdjacencyDirectory> joinAdjacency(
        const std::shared_ptr<const AdjacencyDirectory>& left,
        const std::shared_ptr<const AdjacencyDirectory>& right,
        const ImpactPagedGraph& leftGraph, const ImpactPagedGraph& rightGraph) const;

    std::shared_ptr<const BitDirectory> defined_;
    std::shared_ptr<const BitDirectory> constants_;
    std::shared_ptr<const AdjacencyDirectory> out_;
    std::shared_ptr<const AdjacencyDirectory> in_;
};
}
#endif
