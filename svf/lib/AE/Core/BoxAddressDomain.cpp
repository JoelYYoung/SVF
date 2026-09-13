//===- BoxAddressDomain.cpp -- Box/address reduced product -------------===//
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
// Contributors: Xiao Cheng, Jiawei Wang, Jiawei Yang
//
//===----------------------------------------------------------------------===//

#include "AE/Core/BoxAddressDomain.h"

#include <algorithm>
#include <sstream>
#include <stdexcept>

namespace SVF::AbstractDomain
{

LifetimeDomain LifetimeDomain::top()
{
    return LifetimeDomain(true);
}

LifetimeDomain LifetimeDomain::bottom()
{
    return LifetimeDomain(false);
}


std::unique_ptr<AbstractDomain> LifetimeDomain::clone() const
{
    return std::make_unique<LifetimeDomain>(*this);
}

std::shared_ptr<LifetimeDomain::LocationIDs>
LifetimeDomain::emptyLocationIDs()
{
    static const auto empty = std::make_shared<LocationIDs>();
    return empty;
}

void LifetimeDomain::allocate(Location location)
{
    setMayBeFreed(location, false);
}

void LifetimeDomain::release(Location location)
{
    setMayBeFreed(location, true);
}

bool LifetimeDomain::mayBeFreed(Location location) const
{
    return mayBeFreed(location.id());
}

bool LifetimeDomain::mayBeFreed(std::uint32_t locationID) const
{
    return defaultMayBeFreed_ !=
           (exceptions_->count(locationID) != 0);
}

bool LifetimeDomain::hasCompatibleDomain(const AbstractDomain& other) const
{
    return other.isDomain<LifetimeDomain>();
}

void LifetimeDomain::joinDomain(const AbstractDomain& other)
{
    const auto& state = static_cast<const LifetimeDomain&>(other);
    if (defaultMayBeFreed_ == state.defaultMayBeFreed_ &&
            (exceptions_ == state.exceptions_ ||
             *exceptions_ == *state.exceptions_))
        return;
    if (state.isBottomDomain())
        return;
    if (isBottomDomain())
    {
        *this = state;
        return;
    }
    if (state.leqDomain(*this))
        return;
    if (leqDomain(state))
    {
        *this = state;
        return;
    }
    combineWith(state, true);
}

void LifetimeDomain::meetDomain(const AbstractDomain& other)
{
    const auto& state = static_cast<const LifetimeDomain&>(other);
    if (defaultMayBeFreed_ == state.defaultMayBeFreed_ &&
            (exceptions_ == state.exceptions_ ||
             *exceptions_ == *state.exceptions_))
        return;
    if (state.isTopDomain())
        return;
    if (isTopDomain())
    {
        *this = state;
        return;
    }
    if (leqDomain(state))
        return;
    if (state.leqDomain(*this))
    {
        *this = state;
        return;
    }
    combineWith(state, false);
}

void LifetimeDomain::widenDomain(const AbstractDomain& next)
{
    joinDomain(next);
}

void LifetimeDomain::narrowDomain(const AbstractDomain& next)
{
    meetDomain(next);
}

bool LifetimeDomain::isBottomDomain() const
{
    return !defaultMayBeFreed_ && exceptions_->empty();
}

bool LifetimeDomain::isTopDomain() const
{
    return defaultMayBeFreed_ && exceptions_->empty();
}

bool LifetimeDomain::leqDomain(const AbstractDomain& other) const
{
    const auto& state = static_cast<const LifetimeDomain&>(other);
    if (defaultMayBeFreed_ == state.defaultMayBeFreed_ &&
            (exceptions_ == state.exceptions_ ||
             *exceptions_ == *state.exceptions_))
        return true;
    if (defaultMayBeFreed_ && !state.defaultMayBeFreed_)
        return false;

    for (std::uint32_t locationID : *exceptions_)
    {
        if (mayBeFreed(locationID) && !state.mayBeFreed(locationID))
            return false;
    }
    for (std::uint32_t locationID : *state.exceptions_)
    {
        if (exceptions_->count(locationID) == 0 &&
                mayBeFreed(locationID) && !state.mayBeFreed(locationID))
            return false;
    }
    return true;
}

std::string LifetimeDomain::domainToString() const
{
    std::vector<std::uint32_t> locations(
        exceptions_->begin(), exceptions_->end());
    std::sort(locations.begin(), locations.end());
    std::ostringstream output;
    output << "default="
           << (defaultMayBeFreed_ ? "may-freed" : "not-freed") << " {";
    bool first = true;
    for (std::uint32_t locationID : locations)
    {
        if (!first)
            output << ", ";
        first = false;
        output << locationID << "="
               << (mayBeFreed(locationID) ? "may-freed" : "not-freed");
    }
    output << "}";
    return output.str();
}

void LifetimeDomain::setMayBeFreed(Location location, bool mayBeFreedValue)
{
    const bool differsFromDefault =
        mayBeFreedValue != defaultMayBeFreed_;
    const bool stored = exceptions_->count(location.id()) != 0;
    if (differsFromDefault == stored)
        return;
    if (differsFromDefault)
        writableExceptions().insert(location.id());
    else
        writableExceptions().erase(location.id());
}

void LifetimeDomain::combineWith(
    const LifetimeDomain& other, bool join)
{
    const bool nextDefault = join
                             ? defaultMayBeFreed_ || other.defaultMayBeFreed_
                             : defaultMayBeFreed_ && other.defaultMayBeFreed_;
    LocationIDs next;
    next.reserve(exceptions_->size() + other.exceptions_->size());
    const auto addIfExceptional = [&](std::uint32_t locationID)
    {
        const bool value = join
                           ? mayBeFreed(locationID) || other.mayBeFreed(locationID)
                           : mayBeFreed(locationID) && other.mayBeFreed(locationID);
        if (value != nextDefault)
            next.insert(locationID);
    };
    for (std::uint32_t locationID : *exceptions_)
        addIfExceptional(locationID);
    for (std::uint32_t locationID : *other.exceptions_)
    {
        if (exceptions_->count(locationID) == 0)
            addIfExceptional(locationID);
    }
    defaultMayBeFreed_ = nextDefault;
    exceptions_ = std::make_shared<LocationIDs>(std::move(next));
}

LifetimeDomain::LocationIDs& LifetimeDomain::writableExceptions()
{
    if (exceptions_.use_count() != 1)
        exceptions_ = std::make_shared<LocationIDs>(*exceptions_);
    return *exceptions_;
}

Variable MemoryLayout::contentOf(Location location) const
{
    const auto it = cells_->find(location);
    if (it == cells_->end())
        throw std::out_of_range("location has no content symbol");
    return it->second;
}

void MemoryLayout::extend(Location location, Variable content)
{
    const auto [iterator, inserted] = cells_->emplace(location, content);
    if (!inserted && iterator->second != content)
        throw std::invalid_argument(
            "location already has a different content symbol");
}

void BoxAddressDomain::restoreMissingMemoryFrom(
    const BoxAddressDomain& caller, Variable content)
{
    if (isBottom() || caller.isBottom())
        return;
    if (numerical_.bound(content).isTop())
    {
        const Interval interval = caller.numerical_.bound(content);
        if (!interval.isTop())
            numerical_.setBound(content, interval);
    }
    restoreMissingAddressFrom(caller, content);
}

void BoxAddressDomain::restoreMissingAddressFrom(
    const BoxAddressDomain& caller, Variable content)
{
    if (isBottom() || caller.isBottom())
        return;
    if (addresses_.addressSet(content).isTop())
    {
        const AddressSet addresses = caller.addresses_.addressSet(content);
        if (!addresses.isTop())
            addresses_.assign(content, addresses);
    }
}

} // namespace SVF::AbstractDomain
