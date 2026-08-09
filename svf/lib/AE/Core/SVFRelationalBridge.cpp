//===- SVFRelationalBridge.cpp -- NodeID to relational Core -------------===//

#include "AE/Core/SVFRelationalBridge.h"

#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

using namespace SVF;

namespace
{

SVF::Environment makeEnvironment(
    const std::vector<TrackedRelationalVariable>& variables)
{
    std::vector<SVF::VariableDeclaration> declarations;
    declarations.reserve(variables.size());
    for (const TrackedRelationalVariable& variable : variables)
    {
        declarations.push_back(
            {SVF::Variable(variable.id), variable.type, variable.name});
    }
    return SVF::Environment(std::move(declarations));
}

std::optional<s64_t> exactSigned64(const SVF::Rational& value)
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
    SVF::OctagonConfig config)
    : environment_(makeEnvironment(variables)),
      state_(SVF::OctagonState::top(environment_, config))
{
}

bool SVFRelationalBridge::tracks(NodeID id) const
{
    return environment_.contains(SVF::Variable(id));
}

SVF::Variable SVFRelationalBridge::variable(NodeID id) const
{
    const SVF::Variable result(id);
    if (!environment_.contains(result))
        throw std::invalid_argument("SVF NodeID is not relationally tracked");
    return result;
}

const SVF::Environment& SVFRelationalBridge::environment() const
{
    return environment_;
}

void SVFRelationalBridge::changeTrackedVariables(
    std::vector<TrackedRelationalVariable> variables, bool projectNewVariables)
{
    SVF::Environment nextEnvironment = makeEnvironment(variables);
    state_.changeEnvironment(nextEnvironment, projectNewVariables);
    environment_ = std::move(nextEnvironment);
}

void SVFRelationalBridge::setTop()
{
    state_ = SVF::OctagonState::top(environment_, state_.config());
}

void SVFRelationalBridge::setBottom()
{
    state_ = SVF::OctagonState::bottom(environment_, state_.config());
}

void SVFRelationalBridge::assignConstant(NodeID target,
                                         SVF::Rational constant)
{
    state_.assign(variable(target),
                  SVF::LinearExpression(std::move(constant)));
}

SVF::LinearExpression SVFRelationalBridge::expression(
    const std::vector<AffineTerm>& terms, SVF::Rational constant) const
{
    SVF::LinearExpression result(std::move(constant));
    for (const auto& [id, coefficient] : terms)
    {
        const SVF::Variable value = variable(id);
        result.setCoefficient(value, result.coefficient(value) + coefficient);
    }
    return result;
}

void SVFRelationalBridge::assignAffine(NodeID target,
                                       std::vector<AffineTerm> terms,
                                       SVF::Rational constant)
{
    state_.assign(variable(target), expression(terms, std::move(constant)));
}

void SVFRelationalBridge::assumeAffine(std::vector<AffineTerm> terms,
                                       SVF::Rational constant,
                                       SVF::ConstraintKind kind)
{
    state_.assume(SVF::LinearConstraint(
        expression(terms, std::move(constant)), kind));
}

void SVFRelationalBridge::constrainInterval(NodeID target,
                                            const IntervalValue& interval)
{
    if (interval.isBottom())
    {
        state_ = SVF::OctagonState::bottom(environment_, state_.config());
        return;
    }
    if (!interval.lb().is_minus_infinity())
    {
        assumeAffine({{target, SVF::Rational(1)}},
                     SVF::Rational(-interval.lb().getIntNumeral()),
                     SVF::ConstraintKind::GreaterEqual);
    }
    if (!interval.ub().is_plus_infinity())
    {
        assumeAffine({{target, SVF::Rational(1)}},
                     SVF::Rational(-interval.ub().getIntNumeral()),
                     SVF::ConstraintKind::LessEqual);
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
    if (!state_.config().operationCompatible(other.state_.config()) ||
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
                                    const SVF::WideningPolicy& policy)
{
    requireCompatible(next);
    state_ = state_.widenedOctagon(next.state_, policy);
}

void SVFRelationalBridge::narrowWith(const SVFRelationalBridge& next)
{
    requireCompatible(next);
    state_.narrowWith(next.state_);
}

bool SVFRelationalBridge::equals(const SVFRelationalBridge& other) const
{
    requireCompatible(other);
    return state_.equals(other.state_) == SVF::CheckResult::True;
}

bool SVFRelationalBridge::includedIn(const SVFRelationalBridge& other) const
{
    requireCompatible(other);
    return state_.leq(other.state_) == SVF::CheckResult::True;
}

bool SVFRelationalBridge::isBottom() const
{
    return state_.isBottom();
}

SVF::Interval SVFRelationalBridge::bound(NodeID id) const
{
    return state_.bound(variable(id));
}

IntervalValue SVFRelationalBridge::projectInterval(NodeID id) const
{
    const SVF::Variable value = variable(id);
    if (environment_.typeOf(value).kind != SVF::NumericKind::Integer)
        return IntervalValue::top();

    const SVF::Interval projected = state_.bound(value);
    if (projected.isBottom())
        return IntervalValue::bottom();

    BoundedInt lower = BoundedInt::minus_infinity();
    BoundedInt upper = BoundedInt::plus_infinity();
    if (projected.lower().isFinite())
    {
        const SVF::Rational integerLower =
            projected.lower().isStrict()
                ? projected.lower().value().floor() + SVF::Rational(1)
                : projected.lower().value().ceil();
        const std::optional<s64_t> converted = exactSigned64(integerLower);
        if (!converted)
            return IntervalValue::top();
        lower = BoundedInt(*converted);
    }
    if (projected.upper().isFinite())
    {
        const SVF::Rational integerUpper =
            projected.upper().isStrict()
                ? projected.upper().value().ceil() - SVF::Rational(1)
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
