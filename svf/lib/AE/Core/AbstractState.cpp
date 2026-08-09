//===- AbstractState.cpp -- Common abstract-state lattice API -----------===//

#include "AE/Core/AbstractState.h"

#include <stdexcept>

using namespace SVF;

const char* SVF::toString(CheckResult result)
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

AbstractState::~AbstractState() = default;

void AbstractState::requireCompatible(const AbstractState& other) const
{
    if (!hasCompatibleDomain(other))
        throw std::invalid_argument(
            "abstract states use different domains or configurations");
}

std::unique_ptr<AbstractState>
AbstractState::joined(const AbstractState& other) const
{
    std::unique_ptr<AbstractState> result = clone();
    result->joinWith(other);
    return result;
}

std::unique_ptr<AbstractState>
AbstractState::met(const AbstractState& other) const
{
    std::unique_ptr<AbstractState> result = clone();
    result->meetWith(other);
    return result;
}

std::unique_ptr<AbstractState> AbstractState::widened(
    const AbstractState& next) const
{
    std::unique_ptr<AbstractState> result = clone();
    result->widenWith(next);
    return result;
}

std::unique_ptr<AbstractState>
AbstractState::narrowed(const AbstractState& next) const
{
    std::unique_ptr<AbstractState> result = clone();
    result->narrowWith(next);
    return result;
}

void AbstractState::joinWith(const AbstractState& other)
{
    requireCompatible(other);
    joinState(other);
}

void AbstractState::meetWith(const AbstractState& other)
{
    requireCompatible(other);
    meetState(other);
}

void AbstractState::widenWith(const AbstractState& next)
{
    requireCompatible(next);
    widenState(next);
}

void AbstractState::narrowWith(const AbstractState& next)
{
    requireCompatible(next);
    if (!next.leqState(*this))
        throw std::invalid_argument(
            "narrowing requires next to be included in current");
    narrowState(next);
}

bool AbstractState::isBottom() const
{
    return isBottomState();
}

bool AbstractState::isTop() const
{
    return isTopState();
}

CheckResult AbstractState::leq(const AbstractState& other) const
{
    requireCompatible(other);
    return leqState(other) ? CheckResult::True : CheckResult::False;
}

CheckResult AbstractState::equals(const AbstractState& other) const
{
    requireCompatible(other);
    return leqState(other) && other.leqState(*this) ? CheckResult::True
                                                    : CheckResult::False;
}

std::string AbstractState::toString() const
{
    return stateToString();
}
