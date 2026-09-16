//===- DimensionLayout.h -- Internal relational matrix coordinates -*- C++ -*-===//
//
//                     SVF: Static Value-Flow Analysis
//
// Copyright (C) <2013->  <Yulei Sui>
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
// Contributors: Jiawei Yang
//
//===----------------------------------------------------------------------===//

#ifndef SVF_AE_DETAIL_DIMENSION_LAYOUT_H
#define SVF_AE_DETAIL_DIMENSION_LAYOUT_H

#include "AE/Core/Variable.h"

#include <algorithm>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <vector>

namespace SVF::AbstractDomain::detail
{

using Dimension = std::size_t;

struct DimensionEntry
{
    Variable variable;
    explicit DimensionEntry(Variable value) : variable(value) {}
};

/// Physical coordinates, not a mathematical environment. Every domain denotes
/// a property over all typed Variables; variables absent here are unrestricted.
/// Immutable layouts are shared by copies. A relational operation extends or
/// aligns them internally before touching its matrix.
class DimensionLayout
{
public:
    DimensionLayout() : DimensionLayout(std::vector<DimensionEntry> {}) {}
    explicit DimensionLayout(std::vector<DimensionEntry> entries)
    {
        std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b)
        {
            return a.variable < b.variable;
        });
        for (std::size_t i = 1; i < entries.size(); ++i)
            if (entries[i - 1].variable == entries[i].variable)
                throw std::invalid_argument("duplicate relational coordinate");
        entries_ = std::make_shared<const std::vector<DimensionEntry>>(std::move(entries));
    }
    std::size_t size() const
    {
        return entries_->size();
    }
    bool empty() const
    {
        return entries_->empty();
    }
    bool contains(Variable variable) const
    {
        const auto it = find(variable);
        return it != entries_->end() && it->variable == variable;
    }
    Dimension dimensionOf(Variable variable) const
    {
        const auto it = find(variable);
        if (it == entries_->end() || it->variable != variable)
            throw std::out_of_range("missing relational coordinate");
        return static_cast<Dimension>(it - entries_->begin());
    }
    Variable variableOf(Dimension dimension) const
    {
        return entries_->at(dimension).variable;
    }
    const NumericType& typeOf(Variable variable) const
    {
        return entries_->at(dimensionOf(variable)).variable.type();
    }
    const std::vector<DimensionEntry>& variables() const
    {
        return *entries_;
    }
    DimensionLayout add(const std::vector<DimensionEntry>& added) const
    {
        auto entries = *entries_;
        entries.insert(entries.end(), added.begin(), added.end());
        return DimensionLayout(std::move(entries));
    }
    DimensionLayout remove(const std::vector<Variable>& removed) const
    {
        std::vector<DimensionEntry> entries;
        for (const auto& entry : *entries_)
            if (std::find(removed.begin(), removed.end(), entry.variable) == removed.end())
                entries.push_back(entry);
        return DimensionLayout(std::move(entries));
    }
    DimensionLayout merge(const DimensionLayout& other) const
    {
        if (*this == other)
            return *this;
        std::vector<DimensionEntry> entries;
        std::set_union(entries_->begin(), entries_->end(), other.entries_->begin(), other.entries_->end(),
                       std::back_inserter(entries), [](const auto& a, const auto& b)
        {
            return a.variable < b.variable;
        });
        return DimensionLayout(std::move(entries));
    }
    friend bool operator==(const DimensionLayout& a, const DimensionLayout& b)
    {
        return a.entries_ == b.entries_ || (a.size() == b.size() &&
                                            std::equal(a.entries_->begin(), a.entries_->end(), b.entries_->begin(),
                                                    [](const auto& x, const auto& y)
        {
            return x.variable == y.variable;
        }));
    }
    friend bool operator!=(const DimensionLayout& a, const DimensionLayout& b)
    {
        return !(a == b);
    }

private:
    std::vector<DimensionEntry>::const_iterator find(Variable variable) const
    {
        return std::lower_bound(entries_->begin(), entries_->end(), variable,
                                [](const auto& entry, Variable value)
        {
            return entry.variable < value;
        });
    }
    std::shared_ptr<const std::vector<DimensionEntry>> entries_;
};

} // namespace SVF::AbstractDomain::detail

#endif // SVF_AE_DETAIL_DIMENSION_LAYOUT_H
