// SPDX-License-Identifier: AGPL-3.0-or-later
// Paged implementation of Sparrow octImpactDom semantics.
// Upstream: https://github.com/ropas/sparrow
// Revision: 3ec055b8c87b5c8340ef3ed6cde34f5835865b31
// Sparrow-derived portions retain the following upstream notice:
// BSD 3-Clause License
//
// Copyright (c) 2007-present,
// Programming Research Laboratory (ROPAS), Seoul National University, Korea
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// * Redistributions of source code must retain the above copyright notice, this
//   list of conditions and the following disclaimer.
//
// * Redistributions in binary form must reproduce the above copyright notice,
//   this list of conditions and the following disclaimer in the documentation
//   and/or other materials provided with the distribution.
//
// * Neither the name of the copyright holder nor the names of its
//   contributors may be used to endorse or promote products derived from
//   this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
// FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
// DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
// SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
// CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
// OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "AE/Core/ImpactPagedGraph.h"
#include <algorithm>

namespace SVF::AbstractDomain
{
namespace
{
template <class Directory>
std::shared_ptr<Directory> mutableDirectory(std::shared_ptr<const Directory>& directory,
                                            std::size_t minimum)
{
    auto copy = directory ? std::make_shared<Directory>(*directory) : std::make_shared<Directory>();
    if (copy->size() < minimum)
        copy->resize(minimum);
    directory = copy;
    return copy;
}
} // namespace

ImpactPagedGraph ImpactPagedGraph::top()
{
    ImpactPagedGraph graph;
    graph.bottom = false;
    return graph;
}

bool ImpactPagedGraph::test(const std::shared_ptr<const BitDirectory>& bits, Index x)
{
    if (!bits || x / BitsPerPage >= bits->size())
        return false;
    const auto& page = (*bits)[x / BitsPerPage];
    return page && ((*page)[(x % BitsPerPage) / 64] >> (x % 64) & 1u);
}

void ImpactPagedGraph::assign(std::shared_ptr<const BitDirectory>& bits, Index x, bool value)
{
    if (test(bits, x) == value)
        return;
    auto directory = mutableDirectory(bits, x / BitsPerPage + 1);
    auto& slot = (*directory)[x / BitsPerPage];
    auto page = slot ? std::make_shared<BitPage>(*slot) : std::make_shared<BitPage>(BitPage{});
    if (value)
        (*page)[(x % BitsPerPage) / 64] |= std::uint64_t(1) << (x % 64);
    else
        (*page)[(x % BitsPerPage) / 64] &= ~(std::uint64_t(1) << (x % 64));
    slot = page;
}

bool ImpactPagedGraph::contains(const std::shared_ptr<const AdjacencyDirectory>& adjacency,
                                Index owner, Index neighbour)
{
    if (!adjacency || owner / NodesPerAdjacencyPage >= adjacency->size())
        return false;
    const auto& page = (*adjacency)[owner / NodesPerAdjacencyPage];
    return page && std::binary_search(page->begin(), page->end(), Edge{owner, neighbour});
}

void ImpactPagedGraph::insert(std::shared_ptr<const AdjacencyDirectory>& adjacency,
                              Index owner, Index neighbour)
{
    if (contains(adjacency, owner, neighbour))
        return;
    auto directory = mutableDirectory(adjacency, owner / NodesPerAdjacencyPage + 1);
    auto& slot = (*directory)[owner / NodesPerAdjacencyPage];
    auto page = slot ? std::make_shared<AdjacencyPage>(*slot) : std::make_shared<AdjacencyPage>();
    const Edge edge{owner, neighbour};
    page->insert(std::lower_bound(page->begin(), page->end(), edge), edge);
    slot = page;
}

void ImpactPagedGraph::erase(std::shared_ptr<const AdjacencyDirectory>& adjacency,
                             Index owner, Index neighbour)
{
    if (!contains(adjacency, owner, neighbour))
        return;
    auto directory = mutableDirectory(adjacency, 0);
    auto& slot = (*directory)[owner / NodesPerAdjacencyPage];
    auto page = std::make_shared<AdjacencyPage>(*slot);
    page->erase(std::lower_bound(page->begin(), page->end(), Edge{owner, neighbour}));
    slot = page->empty() ? nullptr : std::shared_ptr<const AdjacencyPage>(page);
}

std::vector<ImpactPagedGraph::Index> ImpactPagedGraph::removeOwner(
    std::shared_ptr<const AdjacencyDirectory>& adjacency, Index owner)
{
    std::vector<Index> neighbours;
    if (!adjacency || owner / NodesPerAdjacencyPage >= adjacency->size())
        return neighbours;
    const auto& current = (*adjacency)[owner / NodesPerAdjacencyPage];
    if (!current)
        return neighbours;
    const auto first = std::lower_bound(current->begin(), current->end(), Edge{owner, 0});
    auto last = first;
    while (last != current->end() && last->first == owner)
        neighbours.push_back((last++)->second);
    if (neighbours.empty())
        return neighbours;
    auto page = std::make_shared<AdjacencyPage>(current->begin(), first);
    page->insert(page->end(), last, current->end());
    auto directory = mutableDirectory(adjacency, 0);
    (*directory)[owner / NodesPerAdjacencyPage] =
        page->empty() ? nullptr : std::shared_ptr<const AdjacencyPage>(page);
    return neighbours;
}

void ImpactPagedGraph::addEdge(Index source, Index target)
{
    insert(out_, source, target);
    insert(in_, target, source);
}

void ImpactPagedGraph::forget(Index x)
{
    if (bottom)
        return;
    // G.remove_vertex; PowOctLoc.remove x c; PowOctLoc.add x d
    for (Index target : removeOwner(out_, x))
        erase(in_, target, x);
    for (Index source : removeOwner(in_, x))
        erase(out_, source, x);
    assign(constants_, x, false);
    assign(defined_, x, true);
}

void ImpactPagedGraph::setConstant(Index x, bool strong)
{
    if (bottom)
        return;
    if (strong)
        assign(constants_, x, true);
    assign(defined_, x, true);
}

void ImpactPagedGraph::setVariable(Index x, Index y, bool strong)
{
    if (bottom || x == y || !test(defined_, y))
        return;
    if (strong)
    {
        addEdge(x, y);
        addEdge(y, x);
    }
    assign(defined_, x, true);
}

void ImpactPagedGraph::assume(Index x, Index y)
{
    if (bottom || x == y || !test(defined_, x) || !test(defined_, y))
        return;
    addEdge(y, x);
}

std::shared_ptr<const ImpactPagedGraph::AdjacencyDirectory> ImpactPagedGraph::joinAdjacency(
    const std::shared_ptr<const AdjacencyDirectory>& left,
    const std::shared_ptr<const AdjacencyDirectory>& right,
    const ImpactPagedGraph& leftGraph, const ImpactPagedGraph& rightGraph) const
{
    // AbsOct.join after extend: an edge survives if every side either has it
    // or leaves one of its endpoints undefined. The rule is symmetric in the
    // endpoints, so it applies unchanged to the in-adjacency.
    if (left == right)
        return left;
    const std::size_t pages = std::max(left ? left->size() : 0, right ? right->size() : 0);
    auto result = std::make_shared<AdjacencyDirectory>(pages);
    const auto undefinedIn = [](const ImpactPagedGraph& graph, const Edge& edge)
    {
        return !graph.isDefined(edge.first) || !graph.isDefined(edge.second);
    };
    bool any = false;
    for (std::size_t p = 0; p < pages; ++p)
    {
        const auto l = left && p < left->size() ? (*left)[p] : nullptr;
        const auto r = right && p < right->size() ? (*right)[p] : nullptr;
        if (l == r)
        {
            (*result)[p] = l;
            any |= l != nullptr;
            continue;
        }
        static const AdjacencyPage empty;
        const AdjacencyPage& a = l ? *l : empty;
        const AdjacencyPage& b = r ? *r : empty;
        AdjacencyPage merged;
        merged.reserve(std::max(a.size(), b.size()));
        auto i = a.begin(), j = b.begin();
        while (i != a.end() || j != b.end())
        {
            if (j == b.end() || (i != a.end() && *i < *j))
            {
                if (undefinedIn(rightGraph, *i))
                    merged.push_back(*i);
                ++i;
            }
            else if (i == a.end() || *j < *i)
            {
                if (undefinedIn(leftGraph, *j))
                    merged.push_back(*j);
                ++j;
            }
            else
            {
                merged.push_back(*i);
                ++i;
                ++j;
            }
        }
        if (merged.empty())
            continue;
        any = true;
        if (merged == a)
            (*result)[p] = l;
        else if (merged == b)
            (*result)[p] = r;
        else
            (*result)[p] = std::make_shared<const AdjacencyPage>(std::move(merged));
    }
    if (!any)
        return nullptr;
    if (left && *result == *left)
        return left;
    if (right && *result == *right)
        return right;
    return result;
}

ImpactPagedGraph ImpactPagedGraph::join(const ImpactPagedGraph& other) const
{
    if (bottom)
        return other;
    if (other.bottom)
        return *this;
    ImpactPagedGraph result = top();
    result.out_ = joinAdjacency(out_, other.out_, *this, other);
    result.in_ = joinAdjacency(in_, other.in_, *this, other);
    // const: meet; defined: join
    const auto combine = [](const std::shared_ptr<const BitDirectory>& a,
                            const std::shared_ptr<const BitDirectory>& b, bool unite)
                         -> std::shared_ptr<const BitDirectory>
    {
        if (a == b)
            return a;
        if (!a || !b)
            return unite ? (a ? a : b) : nullptr;
        const std::size_t pages = unite ? std::max(a->size(), b->size()) : std::min(a->size(), b->size());
        auto result = std::make_shared<BitDirectory>(pages);
        for (std::size_t p = 0; p < pages; ++p)
        {
            const auto x = p < a->size() ? (*a)[p] : nullptr;
            const auto y = p < b->size() ? (*b)[p] : nullptr;
            if (x == y || (unite ? !y : !x))
            {
                (*result)[p] = x;
                continue;
            }
            if (unite ? !x : !y)
            {
                (*result)[p] = y;
                continue;
            }
            BitPage page;
            bool nonzero = false;
            for (unsigned w = 0; w < WordsPerPage; ++w)
            {
                page[w] = unite ? ((*x)[w] | (*y)[w]) : ((*x)[w] & (*y)[w]);
                nonzero |= page[w] != 0;
            }
            if (page == *x)
                (*result)[p] = x;
            else if (page == *y)
                (*result)[p] = y;
            else if (nonzero)
                (*result)[p] = std::make_shared<const BitPage>(page);
        }
        if (*result == *a)
            return a;
        if (*result == *b)
            return b;
        return result;
    };
    result.constants_ = combine(constants_, other.constants_, false);
    result.defined_ = combine(defined_, other.defined_, true);
    return result;
}

namespace
{
template <class Directory, class Covers>
bool pagesCover(const std::shared_ptr<const Directory>& small,
                const std::shared_ptr<const Directory>& large, Covers covers)
{
    if (small == large || !small)
        return true;
    for (std::size_t p = 0; p < small->size(); ++p)
    {
        const auto s = (*small)[p];
        const auto l = large && p < large->size() ? (*large)[p] : nullptr;
        if (s == l || !s)
            continue;
        if (!l || !covers(*s, *l))
            return false;
    }
    return true;
}
} // namespace

bool ImpactPagedGraph::le(const ImpactPagedGraph& other) const
{
    // AbsOct.le: d1 <= d2, every edge of the right is on the left, c2 <= c1.
    if (bottom)
        return true;
    if (other.bottom)
        return false;
    const auto bitsSubset = [](const BitPage& small, const BitPage& large)
    {
        for (unsigned w = 0; w < WordsPerPage; ++w)
            if (small[w] & ~large[w])
                return false;
        return true;
    };
    const auto edgesSubset = [](const AdjacencyPage& small, const AdjacencyPage& large)
    {
        return std::includes(large.begin(), large.end(), small.begin(), small.end());
    };
    return pagesCover(defined_, other.defined_, bitsSubset) &&
           pagesCover(other.out_, out_, edgesSubset) &&
           pagesCover(other.constants_, constants_, bitsSubset);
}

bool ImpactPagedGraph::operator==(const ImpactPagedGraph& other) const
{
    if (bottom || other.bottom)
        return bottom == other.bottom;
    const auto sameBits = [](const std::shared_ptr<const BitDirectory>& a,
                             const std::shared_ptr<const BitDirectory>& b)
    {
        const auto subset = [](const BitPage& small, const BitPage& large)
        {
            for (unsigned w = 0; w < WordsPerPage; ++w)
                if (small[w] & ~large[w])
                    return false;
            return true;
        };
        return pagesCover(a, b, subset) && pagesCover(b, a, subset);
    };
    const auto sameEdges = [](const std::shared_ptr<const AdjacencyDirectory>& a,
                              const std::shared_ptr<const AdjacencyDirectory>& b)
    {
        const auto subset = [](const AdjacencyPage& small, const AdjacencyPage& large)
        {
            return std::includes(large.begin(), large.end(), small.begin(), small.end());
        };
        return pagesCover(a, b, subset) && pagesCover(b, a, subset);
    };
    return sameBits(defined_, other.defined_) && sameBits(constants_, other.constants_) &&
           sameEdges(out_, other.out_);
}

bool ImpactPagedGraph::hasPath(Index source, Index target) const
{
    if (bottom || !out_)
        return false;
    std::vector<Index> pending{source};
    std::vector<Index> seen{source};
    while (!pending.empty())
    {
        const Index current = pending.back();
        pending.pop_back();
        if (current / NodesPerAdjacencyPage >= out_->size())
            continue;
        const auto& page = (*out_)[current / NodesPerAdjacencyPage];
        if (!page)
            continue;
        for (auto it = std::lower_bound(page->begin(), page->end(), Edge{current, 0});
                it != page->end() && it->first == current; ++it)
        {
            if (it->second == target)
                return true;
            if (std::find(seen.begin(), seen.end(), it->second) == seen.end())
            {
                seen.push_back(it->second);
                pending.push_back(it->second);
            }
        }
    }
    return false;
}

std::vector<ImpactPagedGraph::Edge> ImpactPagedGraph::edges() const
{
    std::vector<Edge> result;
    if (out_)
        for (const auto& page : *out_)
            if (page)
                result.insert(result.end(), page->begin(), page->end());
    return result;
}

namespace
{
template <class Directory>
std::vector<std::uint32_t> listBits(const std::shared_ptr<const Directory>& bits, unsigned perPage)
{
    std::vector<std::uint32_t> result;
    if (!bits)
        return result;
    for (std::size_t p = 0; p < bits->size(); ++p)
        if (const auto& page = (*bits)[p])
            for (unsigned w = 0; w < page->size(); ++w)
                for (unsigned b = 0; b < 64; ++b)
                    if ((*page)[w] >> b & 1u)
                        result.push_back(static_cast<std::uint32_t>(p * perPage + w * 64 + b));
    return result;
}
} // namespace

std::vector<ImpactPagedGraph::Index> ImpactPagedGraph::defined() const
{
    return listBits(defined_, BitsPerPage);
}

std::vector<ImpactPagedGraph::Index> ImpactPagedGraph::constants() const
{
    return listBits(constants_, BitsPerPage);
}
}
