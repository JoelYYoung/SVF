//===- BoxDomain.cpp -- Exact-rational interval box state ----------------===//

#include "AE/Core/BoxDomain.h"

#include <algorithm>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>

using namespace SVF::AbstractDomain;

namespace
{

int compareLower(const Bound& lhs, const Bound& rhs)
{
    if (lhs.kind() != rhs.kind())
        return static_cast<int>(lhs.kind()) < static_cast<int>(rhs.kind())
                   ? -1
                   : 1;
    if (!lhs.isFinite())
        return 0;
    if (lhs.value() < rhs.value())
        return -1;
    if (rhs.value() < lhs.value())
        return 1;
    if (lhs.isStrict() == rhs.isStrict())
        return 0;
    return lhs.isStrict() ? 1 : -1;
}

Bound minLower(const Bound& lhs, const Bound& rhs)
{
    return compareLower(lhs, rhs) <= 0 ? lhs : rhs;
}

Bound maxLower(const Bound& lhs, const Bound& rhs)
{
    return compareLower(lhs, rhs) >= 0 ? lhs : rhs;
}

Bound scaleBound(const Bound& bound, const Rational& coefficient)
{
    if (coefficient.isZero())
        return Bound::finite(Rational());
    if (bound.isMinusInfinity())
        return coefficient.sign() > 0 ? Bound::minusInfinity()
                                      : Bound::plusInfinity();
    if (bound.isPlusInfinity())
        return coefficient.sign() > 0 ? Bound::plusInfinity()
                                      : Bound::minusInfinity();
    return Bound::finite(bound.value() * coefficient, bound.isStrict());
}

Interval scaleInterval(const Interval& interval, const Rational& coefficient)
{
    if (coefficient.isZero())
        return Interval::singleton(Rational());
    if (coefficient.sign() > 0)
        return Interval(scaleBound(interval.lower(), coefficient),
                        scaleBound(interval.upper(), coefficient));
    return Interval(scaleBound(interval.upper(), coefficient),
                    scaleBound(interval.lower(), coefficient));
}

Interval addIntervals(const Interval& lhs, const Interval& rhs)
{
    return Interval(Bound::add(lhs.lower(), rhs.lower()),
                    Bound::add(lhs.upper(), rhs.upper()));
}

Interval joinIntervals(const Interval& lhs, const Interval& rhs)
{
    return Interval(minLower(lhs.lower(), rhs.lower()),
                    Bound::max(lhs.upper(), rhs.upper()));
}

Interval meetIntervals(const Interval& lhs, const Interval& rhs)
{
    return Interval(maxLower(lhs.lower(), rhs.lower()),
                    Bound::min(lhs.upper(), rhs.upper()));
}

bool intervalIncluded(const Interval& lhs, const Interval& rhs)
{
    if (lhs.isBottom())
        return true;
    if (rhs.isBottom())
        return false;
    return compareLower(lhs.lower(), rhs.lower()) >= 0 &&
           Bound::compare(lhs.upper(), rhs.upper()) <= 0;
}

Interval evaluate(const BoxState& state, const LinearExpression& expression,
                  std::optional<Variable> excluded = std::nullopt)
{
    Interval result = Interval::singleton(expression.constant());
    for (const auto& [variable, coefficient] : expression.terms())
    {
        if (excluded && variable == *excluded)
            continue;
        result = addIntervals(
            result, scaleInterval(state.bound(variable), coefficient));
    }
    return result;
}

Bound integerLower(Bound bound)
{
    if (!bound.isFinite())
        return bound;
    const Rational value = bound.isStrict()
                               ? bound.value().floor() + Rational(1)
                               : bound.value().ceil();
    return Bound::finite(value);
}

Bound integerUpper(Bound bound)
{
    if (!bound.isFinite())
        return bound;
    const Rational value = bound.isStrict() ? bound.value().ceil() - Rational(1)
                                            : bound.value().floor();
    return Bound::finite(value);
}

LinearConstraint normalizedLessEqual(const LinearConstraint& constraint,
                                     bool& strict)
{
    strict = constraint.kind() == ConstraintKind::LessThan ||
             constraint.kind() == ConstraintKind::GreaterThan;
    if (constraint.kind() == ConstraintKind::GreaterEqual ||
            constraint.kind() == ConstraintKind::GreaterThan)
        return LinearConstraint(-constraint.expression(),
                                strict ? ConstraintKind::LessThan
                                       : ConstraintKind::LessEqual);
    return LinearConstraint(constraint.expression(),
                            strict ? ConstraintKind::LessThan
                                   : ConstraintKind::LessEqual);
}

} // namespace

BoxState::BoxState(VariableEnvironment environment, BoxConfig config, bool bottom)
    : environment_(std::move(environment)), config_(std::move(config)),
      bounds_(environment_.size(), Interval::top()), bottom_(bottom)
{
}

BoxState BoxState::top(const VariableEnvironment& environment, const BoxConfig& config)
{
    return BoxState(environment, config, false);
}

BoxState BoxState::bottom(const VariableEnvironment& environment,
                          const BoxConfig& config)
{
    return BoxState(environment, config, true);
}

BoxState BoxState::fromBox(const VariableEnvironment& environment,
                           const IntervalBox& box,
                           const BoxConfig& config)
{
    BoxState result = top(environment, config);
    for (const auto& [variable, interval] : box.bounds)
    {
        if (!environment.contains(variable))
            throw std::invalid_argument("box contains an unknown variable");
        result.setBound(environment.dimensionOf(variable), interval);
    }
    return result;
}

BoxState BoxState::fromConstraints(const VariableEnvironment& environment,
                                   const LinearConstraintSet& constraints,
                                   const BoxConfig& config)
{
    BoxState result = top(environment, config);
    result.assumeAll(constraints);
    return result;
}

std::unique_ptr<AbstractState> BoxState::clone() const
{
    return std::make_unique<BoxState>(*this);
}

const char* BoxState::name() const
{
    return "BoxState";
}

DomainCapabilities BoxState::capabilities() const
{
    DomainCapabilities result;
    result.strictInequalities = true;
    result.integerTightening = config_.integerTightening;
    result.thresholdWidening = true;
    result.narrowing = true;
    result.parallelAssignments = true;
    result.expressionBounds = true;
    result.backwardAssignments = true;
    result.topologicalClosure = true;
    result.canonicalization = true;
    result.nonlinearTreeExpressions = false;
    return result;
}

void BoxState::assign(Variable target, const LinearExpression& expression)
{
    if (!environment_.contains(target))
        throw std::invalid_argument("assignment target is not in environment");
    if (bottom_)
        return;
    for (const auto& [variable, coefficient] : expression.terms())
    {
        (void)coefficient;
        if (!environment_.contains(variable))
            throw std::invalid_argument(
                "assignment expression uses an unknown variable");
    }
    setBound(environment_.dimensionOf(target), evaluate(*this, expression));
}

void BoxState::assign(Variable target, const TreeExpression& expression)
{
    const std::optional<LinearExpression> linear = expression.asLinear();
    if (linear)
    {
        assign(target, *linear);
        return;
    }
    forget(target);
    report(OperationKind::Assignment,
           ApproximationKind::UnsupportedFallback,
           "non-affine tree assignment forgets the target");
}

void BoxState::assignParallel(const LinearAssignmentList& assignments)
{
    std::set<Variable> targets;
    for (const LinearAssignment& assignment : assignments)
    {
        if (!environment_.contains(assignment.target))
            throw std::invalid_argument(
                "parallel assignment target is not in environment");
        if (!targets.insert(assignment.target).second)
            throw std::invalid_argument(
                "parallel assignment contains a duplicate target");
        for (const auto& [variable, coefficient] :
             assignment.expression.terms())
        {
            (void)coefficient;
            if (!environment_.contains(variable))
                throw std::invalid_argument(
                    "parallel assignment expression uses an unknown variable");
        }
    }
    if (bottom_)
        return;

    std::vector<std::pair<Dimension, Interval>> updates;
    updates.reserve(assignments.size());
    for (const LinearAssignment& assignment : assignments)
        updates.emplace_back(environment_.dimensionOf(assignment.target),
                             evaluate(*this, assignment.expression));
    for (auto& [dimension, value] : updates)
        setBound(dimension, std::move(value));
}

void BoxState::substitute(Variable target,
                          const LinearExpression& expression)
{
    substituteParallel({{target, expression}});
}

void BoxState::substituteParallel(
    const LinearAssignmentList& assignments)
{
    std::map<Variable, LinearExpression> replacements;
    for (const LinearAssignment& assignment : assignments)
    {
        if (!environment_.contains(assignment.target))
            throw std::invalid_argument(
                "substitution target is not in environment");
        if (!replacements.emplace(assignment.target, assignment.expression)
                 .second)
            throw std::invalid_argument(
                "parallel substitution contains a duplicate target");
        for (const auto& [variable, coefficient] :
             assignment.expression.terms())
        {
            (void)coefficient;
            if (!environment_.contains(variable))
                throw std::invalid_argument(
                    "substitution expression uses an unknown variable");
        }
    }
    if (assignments.empty() || bottom_)
        return;

    LinearConstraintSet preimage;
    for (const LinearConstraint& constraint : toConstraints())
        preimage.emplace_back(
            constraint.expression().substituted(replacements),
            constraint.kind());
    *this = fromConstraints(environment_, preimage, config_);
}

void BoxState::assume(const LinearConstraint& constraint)
{
    if (bottom_)
        return;
    for (const auto& [variable, coefficient] : constraint.expression().terms())
    {
        (void)coefficient;
        if (!environment_.contains(variable))
            throw std::invalid_argument(
                "constraint uses an unknown variable");
    }

    if (constraint.kind() == ConstraintKind::NotEqual)
    {
        const Interval value = evaluate(*this, constraint.expression());
        if (!value.lower().isFinite() || !value.upper().isFinite() ||
                value.lower().value() != Rational() ||
                value.upper().value() != Rational() ||
                value.lower().isStrict() || value.upper().isStrict())
            return;
        bottom_ = true;
        return;
    }

    if (constraint.kind() == ConstraintKind::Equal)
    {
        assume(LinearConstraint(constraint.expression(),
                                ConstraintKind::LessEqual));
        assume(LinearConstraint(-constraint.expression(),
                                ConstraintKind::LessEqual));
        return;
    }

    bool strict = false;
    const LinearConstraint normalized = normalizedLessEqual(constraint, strict);
    const LinearExpression& expression = normalized.expression();

    // Repeating interval propagation lets bounds inferred for one dimension
    // tighten another without introducing an unbounded worklist.
    for (std::size_t pass = 0; pass <= environment_.size(); ++pass)
    {
        bool changed = false;
        for (const auto& [variable, coefficient] : expression.terms())
        {
            if (coefficient.isZero())
                continue;
            const Interval rest = evaluate(*this, expression, variable);
            if (!rest.lower().isFinite())
                continue;

            const Rational rhs = -rest.lower().value() / coefficient;
            const bool resultStrict = strict || rest.lower().isStrict();
            const Dimension dimension = environment_.dimensionOf(variable);
            Interval next = bounds_[dimension];
            if (coefficient.sign() > 0)
            {
                next = meetIntervals(
                    next,
                    Interval(Bound::minusInfinity(),
                             Bound::finite(rhs, resultStrict)));
            }
            else
            {
                next = meetIntervals(
                    next,
                    Interval(Bound::finite(rhs, resultStrict),
                             Bound::plusInfinity()));
            }
            const Interval previous = bounds_[dimension];
            setBound(dimension, next);
            if (bottom_)
                return;
            changed = changed ||
                      !intervalIncluded(previous, bounds_[dimension]) ||
                      !intervalIncluded(bounds_[dimension], previous);
        }
        if (!changed)
            break;
    }

    const Interval value = evaluate(*this, expression);
    if (value.lower().isFinite())
    {
        const int sign = value.lower().value().sign();
        if (sign > 0 || (sign == 0 && (strict || value.lower().isStrict())))
            bottom_ = true;
    }
}

void BoxState::assume(const TreeConstraint& constraint)
{
    const std::optional<LinearExpression> linear =
        constraint.expression().asLinear();
    if (linear)
    {
        assume(LinearConstraint(*linear, constraint.kind()));
        return;
    }
    report(OperationKind::Assumption,
           ApproximationKind::UnsupportedFallback,
           "non-affine tree constraint is ignored");
}

void BoxState::forget(Variable variable)
{
    if (!environment_.contains(variable))
        throw std::invalid_argument("forgotten variable is not in environment");
    if (!bottom_)
        bounds_[environment_.dimensionOf(variable)] = Interval::top();
}

void BoxState::changeEnvironment(const VariableEnvironment& environment,
                                 bool initializeNewVariablesToZero)
{
    for (const VariableDeclaration& declaration : environment.variables())
    {
        if (environment_.contains(declaration.variable) &&
                environment_.typeOf(declaration.variable) != declaration.type)
            throw std::invalid_argument(
                "environment change modifies a variable's numeric type");
    }
    std::vector<Interval> next(environment.size(), Interval::top());
    for (const VariableDeclaration& declaration : environment.variables())
    {
        if (environment_.contains(declaration.variable))
        {
            next[environment.dimensionOf(declaration.variable)] =
                bounds_[environment_.dimensionOf(declaration.variable)];
        }
        else if (initializeNewVariablesToZero)
        {
            next[environment.dimensionOf(declaration.variable)] =
                Interval::singleton(Rational());
        }
    }
    environment_ = environment;
    bounds_ = std::move(next);
    for (Dimension dimension = 0; dimension < environment_.size(); ++dimension)
        canonicalize(dimension);
}

CheckResult BoxState::entails(const LinearConstraint& constraint) const
{
    if (bottom_)
        return CheckResult::True;
    const Interval value = evaluate(*this, constraint.expression());
    const auto upperAtMostZero = [&]()
    {
        if (!value.upper().isFinite())
            return false;
        return value.upper().value().sign() <= 0;
    };
    const auto upperBelowZero = [&]()
    {
        return value.upper().isFinite() &&
               (value.upper().value().sign() < 0 ||
                (value.upper().value().isZero() && value.upper().isStrict()));
    };
    const auto lowerAtLeastZero = [&]()
    {
        return value.lower().isFinite() &&
               value.lower().value().sign() >= 0;
    };
    const auto lowerAboveZero = [&]()
    {
        return value.lower().isFinite() &&
               (value.lower().value().sign() > 0 ||
                (value.lower().value().isZero() && value.lower().isStrict()));
    };

    switch (constraint.kind())
    {
    case ConstraintKind::LessEqual:
        return upperAtMostZero() ? CheckResult::True : CheckResult::Unknown;
    case ConstraintKind::LessThan:
        return upperBelowZero() ? CheckResult::True : CheckResult::Unknown;
    case ConstraintKind::GreaterEqual:
        return lowerAtLeastZero() ? CheckResult::True : CheckResult::Unknown;
    case ConstraintKind::GreaterThan:
        return lowerAboveZero() ? CheckResult::True : CheckResult::Unknown;
    case ConstraintKind::Equal:
        return upperAtMostZero() && lowerAtLeastZero() ? CheckResult::True
                                                       : CheckResult::Unknown;
    case ConstraintKind::NotEqual:
        return upperBelowZero() || lowerAboveZero() ? CheckResult::True
                                                    : CheckResult::Unknown;
    }
    return CheckResult::Unknown;
}

Interval BoxState::bound(Variable variable) const
{
    if (!environment_.contains(variable))
        throw std::invalid_argument("bounded variable is not in environment");
    if (bottom_)
        return Interval(Bound::plusInfinity(), Bound::minusInfinity());
    return bounds_[environment_.dimensionOf(variable)];
}

Interval BoxState::bound(const LinearExpression& expression) const
{
    for (const auto& [variable, coefficient] : expression.terms())
    {
        (void)coefficient;
        if (!environment_.contains(variable))
            throw std::invalid_argument(
                "bounded expression uses an unknown variable");
    }
    if (bottom_)
        return Interval(Bound::plusInfinity(), Bound::minusInfinity());
    return evaluate(*this, expression);
}

IntervalBox BoxState::toBox() const
{
    IntervalBox result;
    for (Dimension dimension = 0; dimension < environment_.size(); ++dimension)
        result.bounds.emplace(
            environment_.variableOf(dimension),
            bottom_ ? bound(environment_.variableOf(dimension))
                    : bounds_[dimension]);
    return result;
}

LinearConstraintSet BoxState::toConstraints() const
{
    LinearConstraintSet result;
    if (bottom_)
    {
        result.emplace_back(LinearExpression(Rational(1)),
                            ConstraintKind::LessEqual);
        return result;
    }
    for (Dimension dimension = 0; dimension < environment_.size(); ++dimension)
    {
        const Variable variable = environment_.variableOf(dimension);
        const Interval& interval = bounds_[dimension];
        if (interval.lower().isFinite())
        {
            result.emplace_back(
                LinearExpression(variable) -
                    LinearExpression(interval.lower().value()),
                interval.lower().isStrict() ? ConstraintKind::GreaterThan
                                            : ConstraintKind::GreaterEqual);
        }
        if (interval.upper().isFinite())
        {
            result.emplace_back(
                LinearExpression(variable) -
                    LinearExpression(interval.upper().value()),
                interval.upper().isStrict() ? ConstraintKind::LessThan
                                            : ConstraintKind::LessEqual);
        }
    }
    return result;
}

void BoxState::close()
{
    if (bottom_)
        return;
    for (Dimension dimension = 0; dimension < environment_.size(); ++dimension)
    {
        const Interval& interval = bounds_[dimension];
        const Bound lower = interval.lower().isFinite()
                                ? Bound::finite(interval.lower().value())
                                : interval.lower();
        const Bound upper = interval.upper().isFinite()
                                ? Bound::finite(interval.upper().value())
                                : interval.upper();
        setBound(dimension, Interval(lower, upper));
    }
}

void BoxState::canonicalize()
{
    for (Dimension dimension = 0; dimension < environment_.size(); ++dimension)
        canonicalize(dimension);
}

BoxState BoxState::join(const BoxState& other) const
{
    BoxState result(*this);
    result.joinState(other);
    return result;
}

BoxState BoxState::meet(const BoxState& other) const
{
    BoxState result(*this);
    result.meetState(other);
    return result;
}

BoxState BoxState::widen(const BoxState& next,
                              const WideningPolicy& policy) const
{
    requireBox(next);
    if (bottom_)
        return next;
    if (next.bottom_)
        return *this;
    BoxState result(*this);
    for (Dimension dimension = 0; dimension < environment_.size(); ++dimension)
    {
        Bound lower = bounds_[dimension].lower();
        Bound upper = bounds_[dimension].upper();
        const Interval& following = next.bounds_[dimension];
        if (compareLower(following.lower(), lower) < 0)
        {
            lower = Bound::minusInfinity();
            if (following.lower().isFinite())
            {
                for (const Rational& threshold : policy.thresholds)
                {
                    if (threshold <= following.lower().value() &&
                            (lower.isMinusInfinity() ||
                             lower.value() < threshold))
                        lower = Bound::finite(threshold);
                }
            }
        }
        if (Bound::compare(following.upper(), upper) > 0)
        {
            upper = Bound::plusInfinity();
            if (following.upper().isFinite())
            {
                for (const Rational& threshold : policy.thresholds)
                {
                    if (following.upper().value() <= threshold &&
                        (upper.isPlusInfinity() || threshold < upper.value()))
                        upper = Bound::finite(threshold);
                }
            }
        }
        result.setBound(dimension, Interval(lower, upper));
    }
    for (const LinearConstraint& threshold : policy.linearThresholds)
    {
        if (entails(threshold) == CheckResult::True &&
            next.entails(threshold) == CheckResult::True)
            result.assume(threshold);
    }
    return result;
}

BoxState BoxState::narrow(const BoxState& next) const
{
    requireBox(next);
    if (bottom_ || next.bottom_)
        return bottom(environment_, config_);
    BoxState result(*this);
    for (Dimension dimension = 0; dimension < environment_.size(); ++dimension)
    {
        Bound lower = bounds_[dimension].lower();
        Bound upper = bounds_[dimension].upper();
        if (lower.isMinusInfinity())
            lower = next.bounds_[dimension].lower();
        if (upper.isPlusInfinity())
            upper = next.bounds_[dimension].upper();
        result.setBound(dimension, Interval(lower, upper));
    }
    return result;
}

bool BoxState::hasCompatibleDomain(const AbstractState& other) const
{
    const auto* box = dynamic_cast<const BoxState*>(&other);
    return box && environment_ == box->environment_ &&
           config_.operationCompatible(box->config_);
}

void BoxState::joinState(const AbstractState& other)
{
    const BoxState& box = requireBox(other);
    if (box.bottom_)
        return;
    if (bottom_)
    {
        *this = box;
        return;
    }
    for (Dimension dimension = 0; dimension < environment_.size(); ++dimension)
        bounds_[dimension] =
            joinIntervals(bounds_[dimension], box.bounds_[dimension]);
}

void BoxState::meetState(const AbstractState& other)
{
    const BoxState& box = requireBox(other);
    if (bottom_ || box.bottom_)
    {
        bottom_ = true;
        return;
    }
    for (Dimension dimension = 0; dimension < environment_.size(); ++dimension)
    {
        setBound(dimension,
                 meetIntervals(bounds_[dimension], box.bounds_[dimension]));
        if (bottom_)
            return;
    }
}

void BoxState::widenState(const AbstractState& next)
{
    *this = widen(requireBox(next));
}

void BoxState::narrowState(const AbstractState& next)
{
    *this = narrow(requireBox(next));
}

bool BoxState::isBottomState() const
{
    return bottom_;
}

bool BoxState::isTopState() const
{
    if (bottom_)
        return false;
    return std::all_of(bounds_.begin(), bounds_.end(),
                       [](const Interval& interval)
                       { return interval.isTop(); });
}

bool BoxState::leqState(const AbstractState& other) const
{
    const BoxState& box = requireBox(other);
    if (bottom_)
        return true;
    if (box.bottom_)
        return false;
    for (Dimension dimension = 0; dimension < environment_.size(); ++dimension)
    {
        if (!intervalIncluded(bounds_[dimension], box.bounds_[dimension]))
            return false;
    }
    return true;
}

std::string BoxState::stateToString() const
{
    if (bottom_)
        return "bottom";
    std::ostringstream output;
    output << "{";
    for (Dimension dimension = 0; dimension < environment_.size(); ++dimension)
    {
        if (dimension != 0)
            output << ", ";
        output << environment_.nameOf(environment_.variableOf(dimension)) << "="
               << bounds_[dimension].toString();
    }
    output << "}";
    return output.str();
}

const BoxState& BoxState::requireBox(const AbstractState& other) const
{
    requireCompatible(other);
    return static_cast<const BoxState&>(other);
}

void BoxState::canonicalize(Dimension dimension)
{
    if (bottom_)
        return;
    Interval interval = bounds_[dimension];
    const Variable variable = environment_.variableOf(dimension);
    if (config_.integerTightening &&
            environment_.typeOf(variable).kind == NumericKind::Integer)
    {
        interval = Interval(integerLower(interval.lower()),
                            integerUpper(interval.upper()));
        bounds_[dimension] = interval;
    }
    if (interval.isBottom())
        bottom_ = true;
}

void BoxState::setBound(Dimension dimension, Interval interval)
{
    bounds_[dimension] = std::move(interval);
    canonicalize(dimension);
}

void BoxState::report(OperationKind operation,
                      ApproximationKind approximation,
                      std::string reason) const
{
    if (config_.diagnostics)
        config_.diagnostics->report(
            {operation, approximation, std::move(reason)});
}
