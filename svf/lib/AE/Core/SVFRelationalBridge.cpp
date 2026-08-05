//===- SVFRelationalBridge.cpp -- NodeID to relational Core -------------===//

#include "AE/Core/SVFRelationalBridge.h"

#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

using namespace SVF;

namespace
{

relational::Environment makeEnvironment(
    const std::vector<TrackedRelationalVariable>& variables)
{
    std::vector<relational::VariableDeclaration> declarations;
    declarations.reserve(variables.size());
    for (const TrackedRelationalVariable& variable : variables)
    {
        declarations.push_back(
            {relational::Variable(variable.id), variable.type, variable.name});
    }
    return relational::Environment(std::move(declarations));
}

std::shared_ptr<relational::AbstractDomain> requireDomain(
    std::shared_ptr<relational::AbstractDomain> domain)
{
    if (!domain)
        throw std::invalid_argument(
            "SVF relational bridge needs an abstract domain");
    return domain;
}

std::optional<s64_t> exactSigned64(const relational::Rational& value)
{
    const std::string text = value.toString();
    std::size_t consumed = 0;
    try
    {
        const long long converted = std::stoll(text, &consumed, 10);
        if (consumed != text.size())
            return std::nullopt;
        return static_cast<s64_t>(converted);
    }
    catch (const std::exception&)
    {
        return std::nullopt;
    }
}

} // namespace

SVFRelationalBridge::SVFRelationalBridge(
    std::vector<TrackedRelationalVariable> variables,
    std::shared_ptr<relational::AbstractDomain> domain)
    : domain_(requireDomain(std::move(domain))),
      environment_(makeEnvironment(variables)),
      state_(domain_->top(environment_))
{
}

bool SVFRelationalBridge::tracks(NodeID id) const
{
    return environment_.contains(relational::Variable(id));
}

relational::Variable SVFRelationalBridge::variable(NodeID id) const
{
    const relational::Variable result(id);
    if (!environment_.contains(result))
        throw std::invalid_argument("SVF NodeID is not relationally tracked");
    return result;
}

const relational::Environment& SVFRelationalBridge::environment() const
{
    return environment_;
}

void SVFRelationalBridge::changeTrackedVariables(
    std::vector<TrackedRelationalVariable> variables, bool projectNewVariables)
{
    relational::Environment nextEnvironment = makeEnvironment(variables);
    state_.changeEnvironment(nextEnvironment, projectNewVariables);
    environment_ = std::move(nextEnvironment);
}

void SVFRelationalBridge::assignConstant(NodeID target,
                                         relational::Rational constant)
{
    state_.assign(variable(target),
                  relational::LinearExpression(std::move(constant)));
}

relational::LinearExpression SVFRelationalBridge::expression(
    const std::vector<AffineTerm>& terms, relational::Rational constant) const
{
    relational::LinearExpression result(std::move(constant));
    for (const auto& [id, coefficient] : terms)
    {
        const relational::Variable value = variable(id);
        result.setCoefficient(value, result.coefficient(value) + coefficient);
    }
    return result;
}

void SVFRelationalBridge::assignAffine(NodeID target,
                                       std::vector<AffineTerm> terms,
                                       relational::Rational constant)
{
    state_.assign(variable(target), expression(terms, std::move(constant)));
}

void SVFRelationalBridge::assumeAffine(std::vector<AffineTerm> terms,
                                       relational::Rational constant,
                                       relational::ConstraintKind kind)
{
    state_.assume(relational::LinearConstraint(
        expression(terms, std::move(constant)), kind));
}

void SVFRelationalBridge::constrainInterval(NodeID target,
                                            const IntervalValue& interval)
{
    if (interval.isBottom())
    {
        state_ = domain_->bottom(environment_);
        return;
    }
    if (!interval.lb().is_minus_infinity())
    {
        assumeAffine({{target, relational::Rational(1)}},
                     relational::Rational(-interval.lb().getIntNumeral()),
                     relational::ConstraintKind::GreaterEqual);
    }
    if (!interval.ub().is_plus_infinity())
    {
        assumeAffine({{target, relational::Rational(1)}},
                     relational::Rational(-interval.ub().getIntNumeral()),
                     relational::ConstraintKind::LessEqual);
    }
}

void SVFRelationalBridge::assignInterval(NodeID target,
                                         const IntervalValue& interval)
{
    forget(target);
    constrainInterval(target, interval);
}

void SVFRelationalBridge::meetInterval(NodeID target,
                                       const IntervalValue& interval)
{
    constrainInterval(target, interval);
}

void SVFRelationalBridge::forget(NodeID id)
{
    state_.forget(variable(id));
}

void SVFRelationalBridge::requireCompatible(
    const SVFRelationalBridge& other) const
{
    if (domain_.get() != other.domain_.get() ||
        environment_ != other.environment_)
        throw std::invalid_argument(
            "SVF relational bridges have incompatible domains or layouts");
}

void SVFRelationalBridge::joinWith(const SVFRelationalBridge& other)
{
    requireCompatible(other);
    state_.joinWith(other.state_);
}

void SVFRelationalBridge::meetWith(const SVFRelationalBridge& other)
{
    requireCompatible(other);
    state_.meetWith(other.state_);
}

void SVFRelationalBridge::widenWith(const SVFRelationalBridge& next,
                                    const relational::WideningPolicy& policy)
{
    requireCompatible(next);
    state_.widenWith(next.state_, policy);
}

void SVFRelationalBridge::narrowWith(const SVFRelationalBridge& next)
{
    requireCompatible(next);
    state_.narrowWith(next.state_);
}

bool SVFRelationalBridge::equals(const SVFRelationalBridge& other) const
{
    requireCompatible(other);
    return state_.equals(other.state_) == relational::CheckResult::True;
}

bool SVFRelationalBridge::includedIn(const SVFRelationalBridge& other) const
{
    requireCompatible(other);
    return state_.leq(other.state_) == relational::CheckResult::True;
}

bool SVFRelationalBridge::isBottom() const
{
    return state_.isBottom();
}

relational::Interval SVFRelationalBridge::bound(NodeID id) const
{
    return state_.bound(variable(id));
}

IntervalValue SVFRelationalBridge::projectInterval(NodeID id) const
{
    const relational::Variable value = variable(id);
    if (environment_.typeOf(value).kind != relational::NumericKind::Integer)
        return IntervalValue::top();

    const relational::Interval projected = state_.bound(value);
    if (projected.isBottom())
        return IntervalValue::bottom();

    BoundedInt lower = BoundedInt::minus_infinity();
    BoundedInt upper = BoundedInt::plus_infinity();
    if (projected.lower().isFinite())
    {
        const relational::Rational integerLower =
            projected.lower().isStrict()
                ? projected.lower().value().floor() + relational::Rational(1)
                : projected.lower().value().ceil();
        const std::optional<s64_t> converted = exactSigned64(integerLower);
        if (!converted)
            return IntervalValue::top();
        lower = BoundedInt(*converted);
    }
    if (projected.upper().isFinite())
    {
        const relational::Rational integerUpper =
            projected.upper().isStrict()
                ? projected.upper().value().ceil() - relational::Rational(1)
                : projected.upper().value().floor();
        const std::optional<s64_t> converted = exactSigned64(integerUpper);
        if (!converted)
            return IntervalValue::top();
        upper = BoundedInt(*converted);
    }

    if (!lower.leq(upper))
        return IntervalValue::bottom();
    return IntervalValue(std::move(lower), std::move(upper));
}
