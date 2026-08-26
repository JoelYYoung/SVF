//===- NonRelationalDomain.cpp -- Address and lifetime domains ----------===//

#include "AE/Core/NonRelationalDomain.h"

#include <algorithm>
#include <sstream>

namespace SVF::AbstractDomain
{

namespace
{

template <typename Key, typename Value>
std::set<Key> combinedKeys(const std::map<Key, Value>& lhs,
                           const std::map<Key, Value>& rhs)
{
    std::set<Key> keys;
    for (const auto& entry : lhs)
        keys.insert(entry.first);
    for (const auto& entry : rhs)
        keys.insert(entry.first);
    return keys;
}

} // namespace

AddressSet AddressSet::bottom()
{
    return AddressSet(false);
}

AddressSet AddressSet::top()
{
    return AddressSet(true);
}

AddressSet AddressSet::singleton(Location location)
{
    AddressSet result = bottom();
    result.insert(location);
    return result;
}

bool AddressSet::isBottom() const
{
    return !top_ && locations_.empty();
}

bool AddressSet::isTop() const
{
    return top_;
}

bool AddressSet::contains(Location location) const
{
    return top_ || locations_.count(location) != 0;
}

bool AddressSet::isSingleton() const
{
    return !top_ && locations_.size() == 1;
}

const std::set<Location>& AddressSet::locations() const
{
    if (top_)
        throw std::logic_error("top address set has no finite enumeration");
    return locations_;
}

void AddressSet::insert(Location location)
{
    if (!top_)
        locations_.insert(location);
}

void AddressSet::joinWith(const AddressSet& other)
{
    if (top_ || other.isBottom())
        return;
    if (other.top_)
    {
        *this = top();
        return;
    }
    locations_.insert(other.locations_.begin(), other.locations_.end());
}

void AddressSet::meetWith(const AddressSet& other)
{
    if (other.top_ || isBottom())
        return;
    if (top_)
    {
        *this = other;
        return;
    }
    std::set<Location> intersection;
    std::set_intersection(locations_.begin(), locations_.end(),
                          other.locations_.begin(), other.locations_.end(),
                          std::inserter(intersection, intersection.begin()));
    locations_ = std::move(intersection);
}

bool AddressSet::isSubsetOf(const AddressSet& other) const
{
    if (other.top_ || isBottom())
        return true;
    if (top_)
        return false;
    return std::includes(other.locations_.begin(), other.locations_.end(),
                         locations_.begin(), locations_.end());
}

std::string AddressSet::toString() const
{
    if (top_)
        return "top";
    if (locations_.empty())
        return "bottom";
    std::ostringstream output;
    output << "{";
    bool first = true;
    for (Location location : locations_)
    {
        if (!first)
            output << ",";
        first = false;
        output << location.id();
    }
    output << "}";
    return output.str();
}

AddressState AddressState::top()
{
    return AddressState(true);
}

AddressState AddressState::bottom()
{
    return AddressState(false);
}

std::unique_ptr<AbstractState> AddressState::clone() const
{
    return std::make_unique<AddressState>(*this);
}

const char* AddressState::name() const
{
    return "AddressState";
}

AddressSet AddressState::addressesOf(Variable variable) const
{
    const auto it = values_.find(variable);
    return it == values_.end() ? defaultValue() : it->second;
}

void AddressState::assign(Variable variable, AddressSet addresses)
{
    values_[variable] = std::move(addresses);
    normalize(variable);
}

void AddressState::forget(Variable variable)
{
    assign(variable, AddressSet::top());
}

bool AddressState::hasCompatibleDomain(const AbstractState& other) const
{
    return other.isState<AddressState>();
}

void AddressState::joinState(const AbstractState& other)
{
    const auto& state = static_cast<const AddressState&>(other);
    const std::set<Variable> variables = combinedKeys(values_, state.values_);
    const bool nextDefaultTop = defaultTop_ || state.defaultTop_;
    std::map<Variable, AddressSet> next;
    for (Variable variable : variables)
    {
        AddressSet value = addressesOf(variable);
        value.joinWith(state.addressesOf(variable));
        if (value !=
            (nextDefaultTop ? AddressSet::top() : AddressSet::bottom()))
            next.emplace(variable, std::move(value));
    }
    defaultTop_ = nextDefaultTop;
    values_ = std::move(next);
}

void AddressState::meetState(const AbstractState& other)
{
    const auto& state = static_cast<const AddressState&>(other);
    const std::set<Variable> variables = combinedKeys(values_, state.values_);
    const bool nextDefaultTop = defaultTop_ && state.defaultTop_;
    std::map<Variable, AddressSet> next;
    for (Variable variable : variables)
    {
        AddressSet value = addressesOf(variable);
        value.meetWith(state.addressesOf(variable));
        if (value !=
            (nextDefaultTop ? AddressSet::top() : AddressSet::bottom()))
            next.emplace(variable, std::move(value));
    }
    defaultTop_ = nextDefaultTop;
    values_ = std::move(next);
}

void AddressState::widenState(const AbstractState& next)
{
    joinState(next);
}

void AddressState::narrowState(const AbstractState& next)
{
    meetState(next);
}

bool AddressState::isBottomState() const
{
    return !defaultTop_ && values_.empty();
}

bool AddressState::isTopState() const
{
    return defaultTop_ && values_.empty();
}

bool AddressState::leqState(const AbstractState& other) const
{
    const auto& state = static_cast<const AddressState&>(other);
    if (defaultTop_ && !state.defaultTop_)
        return false;
    const std::set<Variable> variables = combinedKeys(values_, state.values_);
    return std::all_of(variables.begin(), variables.end(),
                       [&](Variable variable) {
                           return addressesOf(variable).isSubsetOf(
                               state.addressesOf(variable));
                       });
}

std::string AddressState::stateToString() const
{
    std::ostringstream output;
    output << "default=" << defaultValue().toString() << " {";
    bool first = true;
    for (const auto& [variable, value] : values_)
    {
        if (!first)
            output << ", ";
        first = false;
        output << variable.id() << "=" << value.toString();
    }
    output << "}";
    return output.str();
}

void AddressState::normalize(Variable variable)
{
    const auto it = values_.find(variable);
    if (it != values_.end() && it->second == defaultValue())
        values_.erase(it);
}

AddressSet AddressState::defaultValue() const
{
    return defaultTop_ ? AddressSet::top() : AddressSet::bottom();
}

Lifetime join(Lifetime lhs, Lifetime rhs)
{
    if (lhs == Lifetime::Bottom)
        return rhs;
    if (rhs == Lifetime::Bottom || lhs == rhs)
        return lhs;
    return Lifetime::MaybeFreed;
}

Lifetime meet(Lifetime lhs, Lifetime rhs)
{
    if (lhs == Lifetime::MaybeFreed)
        return rhs;
    if (rhs == Lifetime::MaybeFreed || lhs == rhs)
        return lhs;
    return Lifetime::Bottom;
}

bool isSubsetOf(Lifetime lhs, Lifetime rhs)
{
    return lhs == Lifetime::Bottom || rhs == Lifetime::MaybeFreed || lhs == rhs;
}

const char* toString(Lifetime lifetime)
{
    switch (lifetime)
    {
    case Lifetime::Bottom:
        return "bottom";
    case Lifetime::Alive:
        return "alive";
    case Lifetime::Freed:
        return "freed";
    case Lifetime::MaybeFreed:
        return "maybe-freed";
    }
    return "invalid";
}

LifetimeState LifetimeState::top()
{
    return LifetimeState(Lifetime::MaybeFreed);
}

LifetimeState LifetimeState::bottom()
{
    return LifetimeState(Lifetime::Bottom);
}

std::unique_ptr<AbstractState> LifetimeState::clone() const
{
    return std::make_unique<LifetimeState>(*this);
}

const char* LifetimeState::name() const
{
    return "LifetimeState";
}

Lifetime LifetimeState::statusOf(Location location) const
{
    const auto it = values_.find(location);
    return it == values_.end() ? defaultValue_ : it->second;
}

void LifetimeState::allocate(Location location)
{
    set(location, Lifetime::Alive);
}

void LifetimeState::release(Location location)
{
    const Lifetime current = statusOf(location);
    set(location, current == Lifetime::Alive || current == Lifetime::Freed
                      ? Lifetime::Freed
                      : Lifetime::MaybeFreed);
}

bool LifetimeState::mayBeFreed(Location location) const
{
    const Lifetime lifetime = statusOf(location);
    return lifetime == Lifetime::Freed || lifetime == Lifetime::MaybeFreed;
}

bool LifetimeState::mustBeFreed(Location location) const
{
    return statusOf(location) == Lifetime::Freed;
}

bool LifetimeState::hasCompatibleDomain(const AbstractState& other) const
{
    return other.isState<LifetimeState>();
}

void LifetimeState::joinState(const AbstractState& other)
{
    const auto& state = static_cast<const LifetimeState&>(other);
    const std::set<Location> locations = combinedKeys(values_, state.values_);
    const Lifetime nextDefault = join(defaultValue_, state.defaultValue_);
    std::map<Location, Lifetime> next;
    for (Location location : locations)
    {
        const Lifetime value =
            join(statusOf(location), state.statusOf(location));
        if (value != nextDefault)
            next.emplace(location, value);
    }
    defaultValue_ = nextDefault;
    values_ = std::move(next);
}

void LifetimeState::meetState(const AbstractState& other)
{
    const auto& state = static_cast<const LifetimeState&>(other);
    const std::set<Location> locations = combinedKeys(values_, state.values_);
    const Lifetime nextDefault = meet(defaultValue_, state.defaultValue_);
    std::map<Location, Lifetime> next;
    for (Location location : locations)
    {
        const Lifetime value =
            meet(statusOf(location), state.statusOf(location));
        if (value != nextDefault)
            next.emplace(location, value);
    }
    defaultValue_ = nextDefault;
    values_ = std::move(next);
}

void LifetimeState::widenState(const AbstractState& next)
{
    joinState(next);
}

void LifetimeState::narrowState(const AbstractState& next)
{
    meetState(next);
}

bool LifetimeState::isBottomState() const
{
    return defaultValue_ == Lifetime::Bottom && values_.empty();
}

bool LifetimeState::isTopState() const
{
    return defaultValue_ == Lifetime::MaybeFreed && values_.empty();
}

bool LifetimeState::leqState(const AbstractState& other) const
{
    const auto& state = static_cast<const LifetimeState&>(other);
    if (!SVF::AbstractDomain::isSubsetOf(defaultValue_, state.defaultValue_))
        return false;
    const std::set<Location> locations = combinedKeys(values_, state.values_);
    return std::all_of(locations.begin(), locations.end(),
                       [&](Location location) {
                           return SVF::AbstractDomain::isSubsetOf(
                               statusOf(location), state.statusOf(location));
                       });
}

std::string LifetimeState::stateToString() const
{
    std::ostringstream output;
    output << "default=" << SVF::AbstractDomain::toString(defaultValue_)
           << " {";
    bool first = true;
    for (const auto& [location, value] : values_)
    {
        if (!first)
            output << ", ";
        first = false;
        output << location.id() << "=" << SVF::AbstractDomain::toString(value);
    }
    output << "}";
    return output.str();
}

void LifetimeState::set(Location location, Lifetime lifetime)
{
    if (lifetime == defaultValue_)
        values_.erase(location);
    else
        values_[location] = lifetime;
}

Variable MemoryLayout::contentOf(Location location) const
{
    const auto it = cells_->find(location);
    if (it == cells_->end())
        throw std::out_of_range("location has no content symbol");
    return it->second;
}

} // namespace SVF::AbstractDomain
