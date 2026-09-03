//===- AddressDomain.h -- Address-set abstract property --------*- C++ -*-===//

#ifndef SVF_AE_ADDRESS_DOMAIN_H
#define SVF_AE_ADDRESS_DOMAIN_H

#include "AE/Core/AbstractDomain.h"
#include "AE/Core/VariableEnvironment.h"

#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>

namespace SVF::AbstractDomain
{

class Location
{
public:
    explicit Location(std::uint32_t id = 0) : id_(id) {}

    std::uint32_t id() const
    {
        return id_;
    }

    friend bool operator==(Location lhs, Location rhs)
    {
        return lhs.id_ == rhs.id_;
    }
    friend bool operator!=(Location lhs, Location rhs)
    {
        return !(lhs == rhs);
    }
    friend bool operator<(Location lhs, Location rhs)
    {
        return lhs.id_ < rhs.id_;
    }

private:
    std::uint32_t id_;
};

/// Scalar projection of one pointer variable.
class AddressSet
{
public:
    AddressSet() = default;

    static AddressSet bottom();
    static AddressSet top();
    static AddressSet singleton(Location location);

    bool isBottom() const;
    bool isTop() const;
    bool isSingleton() const;
    bool contains(Location location) const;
    const std::set<Location>& locations() const;

    void insert(Location location);
    void joinWith(const AddressSet& other);
    void meetWith(const AddressSet& other);
    bool isSubsetOf(const AddressSet& other) const;
    std::string toString() const;

    friend bool operator==(const AddressSet& lhs, const AddressSet& rhs)
    {
        return lhs.top_ == rhs.top_ && lhs.locations_ == rhs.locations_;
    }
    friend bool operator!=(const AddressSet& lhs, const AddressSet& rhs)
    {
        return !(lhs == rhs);
    }

private:
    explicit AddressSet(bool top) : top_(top) {}

    bool top_ = false;
    std::set<Location> locations_;
};

/// Complete address-domain property over an immutable variable vocabulary.
/// Missing entries have the single default selected by top()/bottom().
class AddressDomain final : public AbstractDomain
{
public:
    static AddressDomain top(const VariableEnvironment& environment);
    static AddressDomain bottom(const VariableEnvironment& environment);

    DomainKind kind() const noexcept override
    {
        return DomainKind::Address;
    }
    std::unique_ptr<AbstractDomain> clone() const override;

    const VariableEnvironment& environment() const
    {
        return environment_;
    }
    AddressSet addressSet(Variable variable) const;
    void assign(Variable variable, AddressSet addresses);
    void forget(Variable variable);
    void changeEnvironment(const VariableEnvironment& environment);

private:
    using Values = std::map<Variable, AddressSet>;

    AddressDomain(VariableEnvironment environment, bool defaultTop)
        : environment_(std::move(environment)), defaultTop_(defaultTop),
          values_(std::make_shared<Values>())
    {
    }

    const void* dynamicTypeToken() const noexcept override
    {
        return staticTypeToken<AddressDomain>();
    }
    bool hasCompatibleDomain(const AbstractDomain& other) const override;
    void joinDomain(const AbstractDomain& other) override;
    void meetDomain(const AbstractDomain& other) override;
    void widenDomain(const AbstractDomain& next) override;
    void narrowDomain(const AbstractDomain& next) override;
    bool isBottomDomain() const override;
    bool isTopDomain() const override;
    bool leqDomain(const AbstractDomain& other) const override;
    std::string domainToString() const override;

    const AddressDomain& requireAddress(const AbstractDomain& other) const;
    void normalize(Variable variable);
    Values& writableValues();
    AddressSet defaultValue() const;

    VariableEnvironment environment_;
    bool defaultTop_ = false;
    std::shared_ptr<Values> values_;
};

} // namespace SVF::AbstractDomain

#endif // SVF_AE_ADDRESS_DOMAIN_H
