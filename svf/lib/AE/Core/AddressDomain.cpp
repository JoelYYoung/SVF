//===- AddressDomain.cpp -- Address-set abstract property ----------------===//

#include "AE/Core/AddressDomain.h"

#include <algorithm>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <utility>

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

bool AddressSet::isSingleton() const
{
    return !top_ && locations_.size() == 1;
}

bool AddressSet::contains(Location location) const
{
    return top_ || locations_.count(location) != 0;
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

AddressDomain AddressDomain::top(const VariableEnvironment& environment)
{
    return AddressDomain(environment, true);
}

AddressDomain AddressDomain::bottom(const VariableEnvironment& environment)
{
    return AddressDomain(environment, false);
}

std::unique_ptr<AbstractDomain> AddressDomain::clone() const
{
    return std::make_unique<AddressDomain>(*this);
}

AddressSet AddressDomain::addressSet(Variable variable) const
{
    if (!environment_.contains(variable))
        throw std::invalid_argument("address variable is outside environment");
    const auto iterator = values_->find(variable);
    return iterator == values_->end() ? defaultValue() : iterator->second;
}

void AddressDomain::assign(Variable variable, AddressSet addresses)
{
    if (!environment_.contains(variable))
        throw std::invalid_argument("address variable is outside environment");
    writableValues()[variable] = std::move(addresses);
    normalize(variable);
}

void AddressDomain::forget(Variable variable)
{
    assign(variable, AddressSet::top());
}

void AddressDomain::changeEnvironment(const VariableEnvironment& environment)
{
    for (const VariableDeclaration& declaration : environment_.variables())
    {
        if (environment.contains(declaration.variable) &&
            environment.typeOf(declaration.variable) != declaration.type)
            throw std::invalid_argument(
                "address environment changed a variable type");
    }

    const bool hasOutOfScope =
        std::any_of(values_->begin(), values_->end(), [&](const auto& entry) {
            return !environment.contains(entry.first);
        });
    if (hasOutOfScope)
    {
        Values& values = writableValues();
        for (auto iterator = values.begin(); iterator != values.end();)
        {
            if (!environment.contains(iterator->first))
                iterator = values.erase(iterator);
            else
                ++iterator;
        }
    }
    environment_ = environment;
}

bool AddressDomain::hasCompatibleDomain(const AbstractDomain& other) const
{
    return other.isDomain<AddressDomain>() &&
           environment_ ==
               static_cast<const AddressDomain&>(other).environment_;
}

void AddressDomain::joinDomain(const AbstractDomain& other)
{
    const AddressDomain& address = requireAddress(other);
    const std::set<Variable> variables =
        combinedKeys(*values_, *address.values_);
    const bool nextDefaultTop = defaultTop_ || address.defaultTop_;
    Values next;
    for (Variable variable : variables)
    {
        AddressSet value = addressSet(variable);
        value.joinWith(address.addressSet(variable));
        const AddressSet nextDefault =
            nextDefaultTop ? AddressSet::top() : AddressSet::bottom();
        if (value != nextDefault)
            next.emplace(variable, std::move(value));
    }
    defaultTop_ = nextDefaultTop;
    values_ = std::make_shared<Values>(std::move(next));
}

void AddressDomain::meetDomain(const AbstractDomain& other)
{
    const AddressDomain& address = requireAddress(other);
    const std::set<Variable> variables =
        combinedKeys(*values_, *address.values_);
    const bool nextDefaultTop = defaultTop_ && address.defaultTop_;
    Values next;
    for (Variable variable : variables)
    {
        AddressSet value = addressSet(variable);
        value.meetWith(address.addressSet(variable));
        const AddressSet nextDefault =
            nextDefaultTop ? AddressSet::top() : AddressSet::bottom();
        if (value != nextDefault)
            next.emplace(variable, std::move(value));
    }
    defaultTop_ = nextDefaultTop;
    values_ = std::make_shared<Values>(std::move(next));
}

void AddressDomain::widenDomain(const AbstractDomain& next)
{
    joinDomain(next);
}

void AddressDomain::narrowDomain(const AbstractDomain& next)
{
    meetDomain(next);
}

bool AddressDomain::isBottomDomain() const
{
    return !defaultTop_ && values_->empty();
}

bool AddressDomain::isTopDomain() const
{
    return defaultTop_ && values_->empty();
}

bool AddressDomain::leqDomain(const AbstractDomain& other) const
{
    const AddressDomain& address = requireAddress(other);
    if (defaultTop_ == address.defaultTop_ &&
        (values_ == address.values_ || *values_ == *address.values_))
        return true;
    if (defaultTop_ && !address.defaultTop_)
        return false;
    const std::set<Variable> variables =
        combinedKeys(*values_, *address.values_);
    return std::all_of(variables.begin(), variables.end(),
                       [&](Variable variable) {
                           return addressSet(variable).isSubsetOf(
                               address.addressSet(variable));
                       });
}

std::string AddressDomain::domainToString() const
{
    std::ostringstream output;
    output << "default=" << defaultValue().toString() << " {";
    bool first = true;
    for (const auto& [variable, value] : *values_)
    {
        if (!first)
            output << ", ";
        first = false;
        output << variable.id() << "=" << value.toString();
    }
    output << "}";
    return output.str();
}

const AddressDomain& AddressDomain::requireAddress(
    const AbstractDomain& other) const
{
    requireCompatible(other);
    return static_cast<const AddressDomain&>(other);
}

void AddressDomain::normalize(Variable variable)
{
    const auto iterator = values_->find(variable);
    if (iterator != values_->end() && iterator->second == defaultValue())
        writableValues().erase(variable);
}

AddressDomain::Values& AddressDomain::writableValues()
{
    if (values_.use_count() != 1)
        values_ = std::make_shared<Values>(*values_);
    return *values_;
}

AddressSet AddressDomain::defaultValue() const
{
    return defaultTop_ ? AddressSet::top() : AddressSet::bottom();
}

} // namespace SVF::AbstractDomain
