//===- AbstractDomain.cpp -- Common abstract-property lattice API ------===//

#include "AE/Core/AbstractDomain.h"

#include <stdexcept>

namespace SVF::AbstractDomain
{

const char* toString(CheckResult result)
{
    switch (result)
    {
    case CheckResult::False:
        return "false";
    case CheckResult::True:
        return "true";
    case CheckResult::Unknown:
        return "unknown";
    }
    return "unknown";
}

AbstractDomain::~AbstractDomain() = default;

void AbstractDomain::requireCompatible(const AbstractDomain& other) const
{
    if (!hasCompatibleDomain(other))
        throw std::invalid_argument(
            "abstract properties use incompatible domains or configurations");
}

void AbstractDomain::joinWith(const AbstractDomain& other)
{
    requireCompatible(other);
    joinDomain(other);
}

void AbstractDomain::meetWith(const AbstractDomain& other)
{
    requireCompatible(other);
    meetDomain(other);
}

void AbstractDomain::widenWith(const AbstractDomain& next)
{
    requireCompatible(next);
    widenDomain(next);
}

void AbstractDomain::narrowWith(const AbstractDomain& next)
{
    requireCompatible(next);
    if (!next.leqDomain(*this))
        throw std::invalid_argument(
            "narrowing requires next to be included in current");
    narrowDomain(next);
}

bool AbstractDomain::isBottom() const
{
    return isBottomDomain();
}

bool AbstractDomain::isTop() const
{
    return isTopDomain();
}

CheckResult AbstractDomain::isSubsetOf(const AbstractDomain& other) const
{
    requireCompatible(other);
    return leqDomain(other) ? CheckResult::True : CheckResult::False;
}

CheckResult AbstractDomain::isEquivalentTo(const AbstractDomain& other) const
{
    requireCompatible(other);
    return leqDomain(other) && other.leqDomain(*this) ? CheckResult::True
                                                      : CheckResult::False;
}

std::string AbstractDomain::toString() const
{
    return domainToString();
}

} // namespace SVF::AbstractDomain
