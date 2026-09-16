//===- InitializationDomain.cpp -- Initialization facts ------------------===//
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

#include "AE/Core/InitializationDomain.h"

#include <algorithm>
#include <sstream>
#include <stdexcept>

namespace SVF::AbstractDomain
{

std::shared_ptr<InitializationDomain::Directory> InitializationDomain::emptyPages()
{
    static const auto empty = std::make_shared<Directory>();
    return empty;
}

InitializationDomain::InitializationDomain(InitializationState state)
    : default_(state), pages_(emptyPages())
{
    if (static_cast<unsigned>(state) > 3)
        throw std::invalid_argument("invalid initialization state");
}

InitializationDomain InitializationDomain::top()
{
    return uniform(InitializationState::Top);
}

InitializationDomain InitializationDomain::bottom()
{
    return uniform(InitializationState::Bottom);
}

InitializationDomain InitializationDomain::uniform(InitializationState state)
{
    return InitializationDomain(state);
}

std::unique_ptr<AbstractDomain> InitializationDomain::clone() const
{
    return std::make_unique<InitializationDomain>(*this);
}

InitializationState InitializationDomain::defaultState() const
{
    return default_;
}

InitializationState InitializationDomain::value(Variable variable) const
{
    const Variable key(variable.id() / ValuesPerPage, variable.type());
    const auto it = std::lower_bound(pages_->begin(), pages_->end(), key,
                                     [](const PageEntry& entry, Variable target)
    {
        return entry.key < target;
    });
    if (it == pages_->end() || it->key != key)
        return default_;
    const auto bit = std::uint64_t{1} << (variable.id() % ValuesPerPage);
    const unsigned state = ((it->page->uninitialized & bit) ? 1U : 0U) |
                           ((it->page->initialized & bit) ? 2U : 0U);
    return state ? static_cast<InitializationState>(state) : default_;
}

void InitializationDomain::assign(Variable variable, InitializationState state)
{
    if (static_cast<unsigned>(state) > 3)
        throw std::invalid_argument("invalid initialization state");
    if (isBottomDomain() || value(variable) == state)
        return;
    if (state == InitializationState::Bottom)
    {
        *this = bottom();
        return;
    }
    if (pages_.use_count() != 1)
        pages_ = std::make_shared<Directory>(*pages_);
    const Variable key(variable.id() / ValuesPerPage, variable.type());
    auto it = std::lower_bound(pages_->begin(), pages_->end(), key,
                               [](const PageEntry& entry, Variable target)
    {
        return entry.key < target;
    });
    if (it == pages_->end() || it->key != key)
        it = pages_->insert(it, {key, std::make_shared<Page>()});
    else if (it->page.use_count() != 1)
        it->page = std::make_shared<Page>(*it->page);
    const auto bit = std::uint64_t{1} << (variable.id() % ValuesPerPage);
    const unsigned stored = state == default_ ? 0 : static_cast<unsigned>(state);
    auto& page = *it->page;
    page.uninitialized = (page.uninitialized & ~bit) | ((stored & 1U) ? bit : 0);
    page.initialized = (page.initialized & ~bit) | ((stored & 2U) ? bit : 0);
    if (!(page.uninitialized | page.initialized))
        pages_->erase(it);
}

void InitializationDomain::forget(Variable variable)
{
    assign(variable, InitializationState::Top);
}

std::vector<Variable> InitializationDomain::nonDefaultVariables() const
{
    return nonDefaultVariables(nullptr);
}

std::vector<Variable> InitializationDomain::nonDefaultVariablesBefore(
    Variable upperBound) const
{
    return nonDefaultVariables(&upperBound);
}

std::vector<Variable> InitializationDomain::nonDefaultVariables(
    const Variable* upperBound) const
{
    std::vector<Variable> variables;
    for (auto it = pages_->begin(); it != pages_->end();)
    {
        const auto first = variables.size();
        const auto index = it->key.id();
        if (upperBound && index > upperBound->id() / ValuesPerPage)
            break;
        unsigned types = 0;
        do
        {
            auto bits = it->page->uninitialized | it->page->initialized;
            while (bits)
            {
                const unsigned slot = static_cast<unsigned>(__builtin_ctzll(bits));
                const Variable variable(index * ValuesPerPage + slot, it->key.type());
                if (upperBound && !(variable < *upperBound))
                    break;
                variables.push_back(variable);
                bits &= bits - 1;
            }
            ++types;
            ++it;
        }
        while (it != pages_->end() && it->key.id() == index);
        // Different types can share the same numeric ID. Preserve the public
        // Variable ordering (ID first), not the internal page/type ordering.
        if (types > 1)
            std::sort(variables.begin() + first, variables.end());
    }
    return variables;
}

bool InitializationDomain::hasCompatibleDomain(const AbstractDomain& other) const
{
    return other.isDomain<InitializationDomain>();
}

InitializationDomain::Page InitializationDomain::expanded(
    const Page* page, InitializationState defaultState)
{
    Page result = page ? *page : Page{};
    const auto missing = ~(result.uninitialized | result.initialized);
    if (static_cast<unsigned>(defaultState) & 1U)
        result.uninitialized |= missing;
    if (static_cast<unsigned>(defaultState) & 2U)
        result.initialized |= missing;
    return result;
}

InitializationDomain::Page InitializationDomain::exceptions(
    Page values, InitializationState defaultState)
{
    const auto same = ((static_cast<unsigned>(defaultState) & 1U)
                       ? values.uninitialized : ~values.uninitialized) &
                      ((static_cast<unsigned>(defaultState) & 2U)
                       ? values.initialized : ~values.initialized);
    values.uninitialized &= ~same;
    values.initialized &= ~same;
    return values;
}

void InitializationDomain::combine(const InitializationDomain& other, bool join)
{
    if (pages_ == other.pages_ && default_ == other.default_)
        return;
    if (other.isBottomDomain())
    {
        if (!join)
            *this = bottom();
        return;
    }
    if (isBottomDomain())
    {
        if (join)
            *this = other;
        return;
    }
    const auto op = [join](InitializationState lhs, InitializationState rhs)
    {
        const auto a = static_cast<unsigned>(lhs);
        const auto b = static_cast<unsigned>(rhs);
        return static_cast<InitializationState>(join ? a | b : a & b);
    };
    InitializationDomain result(op(default_, other.default_));
    if (!result.isBottomDomain())
    {
        result.pages_ = std::make_shared<Directory>();
        result.pages_->reserve(std::max(pages_->size(), other.pages_->size()));
        auto left = pages_->begin();
        auto right = other.pages_->begin();
        while (left != pages_->end() || right != other.pages_->end())
        {
            const auto key = right == other.pages_->end() ? left->key :
                             left == pages_->end() ? right->key :
                             std::min(left->key, right->key);
            const PageEntry* a = left != pages_->end() && left->key == key
                                 ? &*left++ : nullptr;
            const PageEntry* b = right != other.pages_->end() && right->key == key
                                 ? &*right++ : nullptr;
            if (a && b && a->page == b->page && default_ == other.default_)
            {
                result.pages_->push_back(*a);
                continue;
            }
            const Page av = expanded(a ? a->page.get() : nullptr, default_);
            const Page bv = expanded(b ? b->page.get() : nullptr, other.default_);
            Page values;
            values.uninitialized = join ? av.uninitialized | bv.uninitialized
                                   : av.uninitialized & bv.uninitialized;
            values.initialized = join ? av.initialized | bv.initialized
                                 : av.initialized & bv.initialized;
            if (~(values.uninitialized | values.initialized))
            {
                *this = bottom();
                return;
            }
            values = exceptions(values, result.default_);
            if (!(values.uninitialized | values.initialized))
                continue;
            const auto matches = [&values](const PageEntry* entry)
            {
                return entry && entry->page->uninitialized == values.uninitialized &&
                       entry->page->initialized == values.initialized;
            };
            if (matches(a))
                result.pages_->push_back(*a);
            else if (matches(b))
                result.pages_->push_back(*b);
            else
                result.pages_->push_back({key, std::make_shared<Page>(values)});
        }
    }
    *this = std::move(result);
}

void InitializationDomain::joinDomain(const AbstractDomain& other)
{
    combine(static_cast<const InitializationDomain&>(other), true);
}

void InitializationDomain::meetDomain(const AbstractDomain& other)
{
    combine(static_cast<const InitializationDomain&>(other), false);
}

void InitializationDomain::widenDomain(const AbstractDomain& next)
{
    joinDomain(next);
}

void InitializationDomain::narrowDomain(const AbstractDomain& next)
{
    meetDomain(next);
}

bool InitializationDomain::isBottomDomain() const
{
    return default_ == InitializationState::Bottom;
}

bool InitializationDomain::isTopDomain() const
{
    return default_ == InitializationState::Top && pages_->empty();
}

bool InitializationDomain::leqDomain(const AbstractDomain& other) const
{
    const auto& rhs = static_cast<const InitializationDomain&>(other);
    if (isBottomDomain())
        return true;
    const auto included = [](InitializationState a, InitializationState b)
    {
        return (static_cast<unsigned>(a) & static_cast<unsigned>(b)) ==
               static_cast<unsigned>(a);
    };
    if (!included(default_, rhs.default_))
        return false;
    if (default_ == rhs.default_ && pages_ == rhs.pages_)
        return true;
    auto left = pages_->begin();
    auto right = rhs.pages_->begin();
    while (left != pages_->end() || right != rhs.pages_->end())
    {
        const auto key = right == rhs.pages_->end() ? left->key :
                         left == pages_->end() ? right->key :
                         std::min(left->key, right->key);
        const Page* a = left != pages_->end() && left->key == key
                        ? (left++)->page.get() : nullptr;
        const Page* b = right != rhs.pages_->end() && right->key == key
                        ? (right++)->page.get() : nullptr;
        if (a == b && default_ == rhs.default_)
            continue;
        const Page av = expanded(a, default_);
        const Page bv = expanded(b, rhs.default_);
        if ((av.uninitialized & ~bv.uninitialized) ||
                (av.initialized & ~bv.initialized))
            return false;
    }
    return true;
}

std::string InitializationDomain::domainToString() const
{
    static const char* names[] = {"bottom", "uninitialized", "initialized", "top"};
    std::ostringstream out;
    out << "default=" << names[static_cast<unsigned>(default_)] << '{';
    bool first = true;
    for (Variable variable : nonDefaultVariables())
    {
        if (!first)
            out << ',';
        first = false;
        out << variable.id() << '=' << names[static_cast<unsigned>(value(variable))];
    }
    return out.str() + '}';
}

} // namespace SVF::AbstractDomain
