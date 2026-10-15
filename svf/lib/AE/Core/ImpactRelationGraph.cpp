// SPDX-License-Identifier: AGPL-3.0-or-later
// Adapted from ROPAS Sparrow src/domain/octImpactDom.ml.
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

#include "AE/Core/ImpactRelationGraph.h"
#include <algorithm>
#include <iterator>

namespace SVF::AbstractDomain
{
ImpactRelationGraph ImpactRelationGraph::top()
{
    ImpactRelationGraph result;
    result.bottom = false;
    return result;
}

void ImpactRelationGraph::forget(Variable variable)
{
    if (bottom) return;
    for (auto it = edges.begin(); it != edges.end();)
        if (it->first == variable || it->second == variable)
            it = edges.erase(it);
        else
            ++it;
    constants.erase(variable);
    defined.insert(variable);
}

void ImpactRelationGraph::setConstant(Variable variable, bool strong)
{
    if (bottom) return;
    defined.insert(variable);
    if (strong) constants.insert(variable);
}

void ImpactRelationGraph::setVariable(Variable target, Variable source, bool strong)
{
    if (bottom) return;
    if (target == source || defined.count(source) == 0) return;
    defined.insert(target);
    if (strong)
    {
        edges.emplace(target, source);
        edges.emplace(source, target);
    }
}

void ImpactRelationGraph::assume(Variable lhs, Variable rhs)
{
    if (bottom) return;
    if (lhs != rhs && defined.count(lhs) && defined.count(rhs))
        edges.emplace(rhs, lhs);
}

bool ImpactRelationGraph::finiteDifference(Variable lhs, Variable rhs) const
{
    if (bottom || !defined.count(lhs) || !defined.count(rhs)) return false;
    std::set<Variable> seen;
    std::vector<Variable> pending{rhs};
    while (!pending.empty())
    {
        const auto current = pending.back();
        pending.pop_back();
        if (!seen.insert(current).second) continue;
        if (current == lhs) return true;
        for (const auto& edge : edges)
            if (edge.first == current) pending.push_back(edge.second);
    }
    return false;
}

bool ImpactRelationGraph::finiteUpper(Variable variable) const
{
    for (auto constant : constants)
        if (finiteDifference(variable, constant)) return true;
    return false;
}

bool ImpactRelationGraph::finiteLower(Variable variable) const
{
    for (auto constant : constants)
        if (finiteDifference(constant, variable)) return true;
    return false;
}

ImpactRelationGraph ImpactRelationGraph::join(const ImpactRelationGraph& other) const
{
    if (bottom) return other;
    if (other.bottom) return *this;
    // Public Sparrow extends each side by edges whose endpoint is undefined
    // on that side, before intersecting. Plain graph intersection would lose
    // useful branch-local SSA coordinates at joins.
    auto left = edges;
    auto right = other.edges;
    for (const auto& edge : other.edges)
        if (!defined.count(edge.first) || !defined.count(edge.second))
            left.insert(edge);
    for (const auto& edge : edges)
        if (!other.defined.count(edge.first) || !other.defined.count(edge.second))
            right.insert(edge);
    auto result = top();
    result.defined = defined;
    result.defined.insert(other.defined.begin(), other.defined.end());
    std::set_intersection(left.begin(), left.end(), right.begin(), right.end(),
                          std::inserter(result.edges, result.edges.end()));
    std::set_intersection(constants.begin(), constants.end(),
                          other.constants.begin(), other.constants.end(),
                          std::inserter(result.constants, result.constants.end()));
    return result;
}

bool ImpactRelationGraph::le(const ImpactRelationGraph& other) const
{
    if (bottom) return true;
    if (other.bottom) return false;
    return std::includes(other.defined.begin(), other.defined.end(),
                         defined.begin(), defined.end()) &&
           std::includes(edges.begin(), edges.end(),
                         other.edges.begin(), other.edges.end()) &&
           std::includes(constants.begin(), constants.end(),
                         other.constants.begin(), other.constants.end());
}

bool ImpactRelationGraph::hasPath(Variable source, Variable target) const
{
    if (bottom) return false;
    std::set<Variable> seen{source};
    std::vector<Variable> pending{source};
    while (!pending.empty())
    {
        const auto current = pending.back();
        pending.pop_back();
        for (auto it = edges.lower_bound({current, Variable(0)});
                it != edges.end() && it->first == current; ++it)
        {
            if (it->second == target) return true;
            if (seen.insert(it->second).second) pending.push_back(it->second);
        }
    }
    return false;
}

bool ImpactRelationGraph::operator==(const ImpactRelationGraph& other) const
{
    return bottom == other.bottom && edges == other.edges &&
           constants == other.constants && defined == other.defined;
}
}
