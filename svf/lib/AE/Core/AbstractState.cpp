//===- AbstractState.cpp -- Common abstract-state lattice API -----------===//

#include "AE/Core/AbstractState.h"
#include "AE/Core/NumericalDomain.h"

#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>
#include <vector>

using namespace SVF::AbstractDomain;

const char* SVF::AbstractDomain::toString(CheckResult result)
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

CheckResult AbstractState::isSubsetOf(const AbstractState& other) const
{
    requireCompatible(other);
    return leqState(other) ? CheckResult::True : CheckResult::False;
}

CheckResult AbstractState::isEquivalentTo(const AbstractState& other) const
{
    requireCompatible(other);
    return leqState(other) && other.leqState(*this) ? CheckResult::True
                                                    : CheckResult::False;
}

std::string AbstractState::toString() const
{
    return stateToString();
}

void NumericalState::assignParallel(
    const LinearAssignmentList& assignments)
{
    if (assignments.empty())
        return;

    const VariableEnvironment originalEnvironment = environment();
    std::set<Variable> targets;
    for (const LinearAssignment& assignment : assignments)
    {
        if (!originalEnvironment.contains(assignment.target))
            throw std::invalid_argument(
                "parallel assignment target is not in environment");
        if (!targets.insert(assignment.target).second)
            throw std::invalid_argument(
                "parallel assignment contains a duplicate target");
        for (const auto& [variable, coefficient] :
             assignment.expression.terms())
        {
            (void)coefficient;
            if (!originalEnvironment.contains(variable))
                throw std::invalid_argument(
                    "parallel assignment expression uses an unknown variable");
        }
    }
    if (isBottom())
        return;

    std::uint64_t nextId = 0;
    for (const VariableDeclaration& declaration :
         originalEnvironment.variables())
        nextId = std::max(nextId,
                          static_cast<std::uint64_t>(declaration.variable.id()) +
                              1);
    if (nextId + assignments.size() >
        static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) +
            1)
        throw std::overflow_error(
            "not enough temporary variable IDs for parallel assignment");

    std::map<Variable, Variable> oldValues;
    std::vector<VariableDeclaration> temporaries;
    temporaries.reserve(assignments.size());
    for (const LinearAssignment& assignment : assignments)
    {
        const Variable temporary(static_cast<std::uint32_t>(nextId++));
        oldValues.emplace(assignment.target, temporary);
        temporaries.push_back(
            {temporary, originalEnvironment.typeOf(assignment.target),
             "$parallel_old_" +
                 originalEnvironment.nameOf(assignment.target)});
    }

    changeEnvironment(originalEnvironment.add(std::move(temporaries)));
    for (const auto& [target, temporary] : oldValues)
        assign(temporary, LinearExpression(target));

    for (const LinearAssignment& assignment : assignments)
    {
        LinearExpression rewritten(assignment.expression.constant());
        for (const auto& [variable, coefficient] :
             assignment.expression.terms())
        {
            const auto old = oldValues.find(variable);
            const Variable source =
                old == oldValues.end() ? variable : old->second;
            rewritten.setCoefficient(
                source, rewritten.coefficient(source) + coefficient);
        }
        assign(assignment.target, rewritten);
    }
    changeEnvironment(originalEnvironment);
}

void NumericalState::assignParallel(const TreeAssignmentList& assignments)
{
    std::set<Variable> targets;
    LinearAssignmentList affine;
    std::vector<const TreeAssignment*> fallback;
    affine.reserve(assignments.size());
    fallback.reserve(assignments.size());
    for (const TreeAssignment& assignment : assignments)
    {
        if (!environment().contains(assignment.target))
            throw std::invalid_argument(
                "parallel tree assignment target is not in environment");
        if (!targets.insert(assignment.target).second)
            throw std::invalid_argument(
                "parallel tree assignment contains a duplicate target");
        if (const std::optional<LinearExpression> linear =
                assignment.expression.asLinear())
            affine.push_back({assignment.target, *linear});
        else
            fallback.push_back(&assignment);
    }

    // Affine right-hand sides run first while every unsupported target still
    // has its incoming value. Unsupported assignments then apply their normal
    // sound fallback, which forgets only their own targets.
    assignParallel(affine);
    for (const TreeAssignment* assignment : fallback)
        assign(assignment->target, assignment->expression);
}

void NumericalState::assumeAll(const LinearConstraintSet& constraints)
{
    if (constraints.size() < 2)
    {
        for (const LinearConstraint& constraint : constraints)
            assume(constraint);
        return;
    }

    // One pass per constraint and dimension bounds any propagation chain that
    // terminates at all; the equivalence test stops earlier in practice, and
    // immediately for a domain that is exact on linear constraints.
    const std::size_t limit =
        constraints.size() * (environment().size() + 1) + 1;
    for (std::size_t pass = 0; pass < limit; ++pass)
    {
        const std::unique_ptr<AbstractState> before = clone();
        for (const LinearConstraint& constraint : constraints)
            assume(constraint);
        if (isBottom() || isEquivalentTo(*before) == CheckResult::True)
            return;
    }
}
