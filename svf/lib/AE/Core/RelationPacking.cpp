// SPDX-License-Identifier: AGPL-3.0-or-later
#include "AE/Core/RelationPacking.h"
#include <algorithm>
#include <map>
#include <numeric>

namespace SVF::AbstractDomain
{
RelationPacking connectedComponentPacking(
    const std::vector<Variable>& vocabulary,
    const std::vector<RelationEdge>& edges)
{
    std::vector<Variable> variables(vocabulary);
    std::sort(variables.begin(), variables.end());
    variables.erase(std::unique(variables.begin(), variables.end()),
                    variables.end());
    std::map<Variable, std::size_t> indices;
    for (std::size_t i = 0; i < variables.size(); ++i)
        indices.emplace(variables[i], i);
    std::vector<std::size_t> parents(variables.size());
    std::iota(parents.begin(), parents.end(), 0);
    const auto root = [&](std::size_t index)
    {
        while (parents[index] != index)
        {
            parents[index] = parents[parents[index]];
            index = parents[index];
        }
        return index;
    };
    for (const auto& edge : edges)
    {
        const auto lhs = indices.find(edge.first);
        const auto rhs = indices.find(edge.second);
        if (lhs == indices.end() || rhs == indices.end())
            continue;
        const auto leftRoot = root(lhs->second);
        const auto rightRoot = root(rhs->second);
        // The minimum coordinate represents each component, independent of
        // edge order. Path compression bounds repeated graph traversals.
        parents[std::max(leftRoot, rightRoot)] = std::min(leftRoot, rightRoot);
    }
    std::map<std::size_t, std::vector<Variable>> groups;
    for (std::size_t i = 0; i < variables.size(); ++i)
        groups[root(i)].push_back(variables[i]);
    RelationPacking result;
    for (auto& group : groups)
        result.push_back(std::move(group.second));
    return result;
}
}
