//===- ConvexPolyhedraDomain.cpp -- Exact rational polyhedra ------------===//

#include "AE/Core/ConvexPolyhedraDomain.h"

#include <algorithm>
#include <map>
#include <numeric>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <utility>

using namespace SVF::AbstractDomain;

namespace
{

struct Inequality
{
    std::vector<Rational> coefficients;
    Rational bound;
    bool strict = false;
};

std::vector<Inequality> project(
    std::vector<Inequality> inequalities,
    const std::vector<std::size_t>& dimensions);
bool feasible(std::vector<Inequality> inequalities, std::size_t dimensions);
bool entails(const std::vector<Inequality>& premises, std::size_t dimensions,
             const Inequality& conclusion);
std::vector<Inequality> irredundant(std::vector<Inequality> inequalities,
                                    std::size_t dimensions);

bool falseConstant(const Inequality& inequality)
{
    const bool allZero = std::all_of(
        inequality.coefficients.begin(), inequality.coefficients.end(),
        [](const Rational& coefficient) { return coefficient.isZero(); });
    if (!allZero)
        return false;
    return inequality.bound.sign() < 0 ||
           (inequality.bound.isZero() && inequality.strict);
}

bool trueConstant(const Inequality& inequality)
{
    const bool allZero = std::all_of(
        inequality.coefficients.begin(), inequality.coefficients.end(),
        [](const Rational& coefficient) { return coefficient.isZero(); });
    return allZero && !falseConstant(inequality);
}

Inequality scaled(const Inequality& inequality, const Rational& factor)
{
    if (factor.sign() <= 0)
        throw std::invalid_argument("inequality scale must be positive");
    Inequality result = inequality;
    for (Rational& coefficient : result.coefficients)
        coefficient *= factor;
    result.bound *= factor;
    return result;
}

Inequality add(const Inequality& lhs, const Inequality& rhs)
{
    Inequality result;
    result.coefficients.resize(lhs.coefficients.size());
    for (std::size_t index = 0; index < result.coefficients.size(); ++index)
        result.coefficients[index] =
            lhs.coefficients[index] + rhs.coefficients[index];
    result.bound = lhs.bound + rhs.bound;
    result.strict = lhs.strict || rhs.strict;
    return result;
}

std::vector<Inequality> eliminate(std::vector<Inequality> inequalities,
                                  std::size_t dimension)
{
    std::vector<Inequality> positive;
    std::vector<Inequality> negative;
    std::vector<Inequality> zero;
    for (Inequality& inequality : inequalities)
    {
        const int sign = inequality.coefficients[dimension].sign();
        if (sign > 0)
            positive.push_back(std::move(inequality));
        else if (sign < 0)
            negative.push_back(std::move(inequality));
        else
            zero.push_back(std::move(inequality));
    }

    if (positive.empty() || negative.empty())
        return zero;

    zero.reserve(zero.size() + positive.size() * negative.size());
    for (const Inequality& upper : positive)
    {
        for (const Inequality& lower : negative)
        {
            const Rational upperFactor = -lower.coefficients[dimension];
            const Rational lowerFactor = upper.coefficients[dimension];
            Inequality combined =
                add(scaled(upper, upperFactor), scaled(lower, lowerFactor));
            combined.coefficients[dimension] = Rational();
            zero.push_back(std::move(combined));
        }
    }
    return zero;
}

bool tighter(const Inequality& lhs, const Inequality& rhs)
{
    if (lhs.bound < rhs.bound)
        return true;
    if (rhs.bound < lhs.bound)
        return false;
    return lhs.strict && !rhs.strict;
}

std::vector<Inequality> normalized(std::vector<Inequality> inequalities,
                                   bool& bottom)
{
    using Key = std::vector<Rational>;
    struct KeyLess
    {
        bool operator()(const Key& lhs, const Key& rhs) const
        {
            return std::lexicographical_compare(lhs.begin(), lhs.end(),
                                                rhs.begin(), rhs.end());
        }
    };

    std::map<Key, Inequality, KeyLess> unique;
    for (Inequality inequality : inequalities)
    {
        if (falseConstant(inequality))
        {
            bottom = true;
            return {};
        }
        if (trueConstant(inequality))
            continue;

        const auto first = std::find_if(
            inequality.coefficients.begin(), inequality.coefficients.end(),
            [](const Rational& coefficient) { return !coefficient.isZero(); });
        const Rational divisor = first->sign() > 0 ? *first : -*first;
        for (Rational& coefficient : inequality.coefficients)
            coefficient /= divisor;
        inequality.bound /= divisor;

        auto [it, inserted] =
            unique.emplace(inequality.coefficients, inequality);
        if (!inserted && tighter(inequality, it->second))
            it->second = std::move(inequality);
    }

    std::vector<Inequality> result;
    result.reserve(unique.size());
    for (auto& [key, inequality] : unique)
    {
        (void)key;
        result.push_back(std::move(inequality));
    }
    return result;
}

Inequality negateForCounterexample(const Inequality& inequality)
{
    Inequality result = inequality;
    for (Rational& coefficient : result.coefficients)
        coefficient = -coefficient;
    result.bound = -result.bound;
    result.strict = !inequality.strict;
    return result;
}

std::vector<Inequality> constraintRows(const VariableEnvironment& environment,
                                       const LinearConstraint& constraint)
{
    const auto makeRow = [&](const LinearExpression& expression, bool strict)
    {
        Inequality row;
        row.coefficients.resize(environment.size());
        for (const auto& [variable, coefficient] : expression.terms())
        {
            if (!environment.contains(variable))
                throw std::invalid_argument(
                    "polyhedron constraint uses an unknown variable");
            row.coefficients[environment.dimensionOf(variable)] = coefficient;
        }
        row.bound = -expression.constant();
        row.strict = strict;
        return row;
    };

    switch (constraint.kind())
    {
    case ConstraintKind::LessEqual:
        return {makeRow(constraint.expression(), false)};
    case ConstraintKind::LessThan:
        return {makeRow(constraint.expression(), true)};
    case ConstraintKind::GreaterEqual:
        return {makeRow(-constraint.expression(), false)};
    case ConstraintKind::GreaterThan:
        return {makeRow(-constraint.expression(), true)};
    case ConstraintKind::Equal:
        return {makeRow(constraint.expression(), false),
                makeRow(-constraint.expression(), false)};
    case ConstraintKind::NotEqual:
        return {};
    }
    return {};
}

LinearConstraint rowConstraint(const VariableEnvironment& environment,
                               const Inequality& inequality)
{
    LinearExpression expression(-inequality.bound);
    for (Dimension dimension = 0; dimension < environment.size(); ++dimension)
    {
        if (!inequality.coefficients[dimension].isZero())
            expression.setCoefficient(environment.variableOf(dimension),
                                      inequality.coefficients[dimension]);
    }
    return LinearConstraint(std::move(expression),
                            inequality.strict ? ConstraintKind::LessThan
                                              : ConstraintKind::LessEqual);
}

} // namespace

class ConvexPolyhedraState::Impl
{
public:
    std::vector<Inequality> inequalities;
    bool bottom = false;
};

ConvexPolyhedraState::ConvexPolyhedraState(
    VariableEnvironment environment, ConvexPolyhedraConfig config, bool bottom)
    : environment_(std::move(environment)), config_(std::move(config)),
      impl_(std::make_unique<Impl>())
{
    impl_->bottom = bottom;
}

ConvexPolyhedraState ConvexPolyhedraState::top(
    const VariableEnvironment& environment, const ConvexPolyhedraConfig& config)
{
    return ConvexPolyhedraState(environment, config, false);
}

ConvexPolyhedraState ConvexPolyhedraState::bottom(
    const VariableEnvironment& environment, const ConvexPolyhedraConfig& config)
{
    return ConvexPolyhedraState(environment, config, true);
}

ConvexPolyhedraState ConvexPolyhedraState::fromBox(
    const VariableEnvironment& environment, const IntervalBox& box,
    const ConvexPolyhedraConfig& config)
{
    ConvexPolyhedraState result = top(environment, config);
    for (const auto& [variable, interval] : box.bounds)
    {
        if (!environment.contains(variable))
            throw std::invalid_argument("box contains an unknown variable");
        if (interval.isBottom())
            return bottom(environment, config);
        if (interval.lower().isFinite())
        {
            result.assume(LinearConstraint(
                LinearExpression(variable) -
                    LinearExpression(interval.lower().value()),
                interval.lower().isStrict() ? ConstraintKind::GreaterThan
                                            : ConstraintKind::GreaterEqual));
        }
        if (interval.upper().isFinite())
        {
            result.assume(LinearConstraint(
                LinearExpression(variable) -
                    LinearExpression(interval.upper().value()),
                interval.upper().isStrict() ? ConstraintKind::LessThan
                                            : ConstraintKind::LessEqual));
        }
    }
    return result;
}

ConvexPolyhedraState ConvexPolyhedraState::fromConstraints(
    const VariableEnvironment& environment, const LinearConstraintSet& constraints,
    const ConvexPolyhedraConfig& config)
{
    ConvexPolyhedraState result = top(environment, config);
    for (const LinearConstraint& constraint : constraints)
        result.assume(constraint);
    return result;
}

ConvexPolyhedraState::ConvexPolyhedraState(
    const ConvexPolyhedraState& other)
    : NumericalState(other), environment_(other.environment_),
      config_(other.config_), impl_(std::make_unique<Impl>(*other.impl_))
{
}

ConvexPolyhedraState::ConvexPolyhedraState(
    ConvexPolyhedraState&& other) noexcept = default;

ConvexPolyhedraState& ConvexPolyhedraState::operator=(
    const ConvexPolyhedraState& other)
{
    if (this == &other)
        return *this;
    NumericalState::operator=(other);
    environment_ = other.environment_;
    config_ = other.config_;
    impl_ = std::make_unique<Impl>(*other.impl_);
    return *this;
}

ConvexPolyhedraState& ConvexPolyhedraState::operator=(
    ConvexPolyhedraState&& other) noexcept = default;

ConvexPolyhedraState::~ConvexPolyhedraState() = default;

std::unique_ptr<AbstractState> ConvexPolyhedraState::clone() const
{
    return std::make_unique<ConvexPolyhedraState>(*this);
}

const char* ConvexPolyhedraState::name() const
{
    return "ConvexPolyhedraState";
}

DomainCapabilities ConvexPolyhedraState::capabilities() const
{
    return {true, false, false, true, false};
}

void ConvexPolyhedraState::assign(
    Variable target, const LinearExpression& expression)
{
    if (!environment_.contains(target))
        throw std::invalid_argument("assignment target is not in environment");
    for (const auto& [variable, coefficient] : expression.terms())
    {
        (void)coefficient;
        if (!environment_.contains(variable))
            throw std::invalid_argument(
                "assignment expression uses an unknown variable");
    }
    if (impl_->bottom)
        return;

    const std::size_t dimensions = environment_.size();
    std::vector<Inequality> extended;
    extended.reserve(impl_->inequalities.size() + 2 * dimensions);

    // New variables occupy [0,n), old variables [n,2n).
    for (const Inequality& inequality : impl_->inequalities)
    {
        Inequality old;
        old.coefficients.resize(2 * dimensions);
        std::copy(inequality.coefficients.begin(),
                  inequality.coefficients.end(),
                  old.coefficients.begin() + dimensions);
        old.bound = inequality.bound;
        old.strict = inequality.strict;
        extended.push_back(std::move(old));
    }

    const Dimension targetDimension = environment_.dimensionOf(target);
    for (Dimension dimension = 0; dimension < dimensions; ++dimension)
    {
        Inequality equality;
        equality.coefficients.resize(2 * dimensions);
        equality.coefficients[dimension] = Rational(1);
        if (dimension == targetDimension)
        {
            for (const auto& [variable, coefficient] : expression.terms())
            {
                equality.coefficients[dimensions +
                    environment_.dimensionOf(variable)] -= coefficient;
            }
            equality.bound = expression.constant();
        }
        else
        {
            equality.coefficients[dimensions + dimension] = Rational(-1);
        }
        extended.push_back(equality);
        for (Rational& coefficient : equality.coefficients)
            coefficient = -coefficient;
        equality.bound = -equality.bound;
        extended.push_back(std::move(equality));
    }

    std::vector<std::size_t> oldDimensions(dimensions);
    std::iota(oldDimensions.begin(), oldDimensions.end(), dimensions);
    extended = project(std::move(extended), oldDimensions);
    for (Inequality& inequality : extended)
        inequality.coefficients.resize(dimensions);
    impl_->inequalities = std::move(extended);
    normalize();
}

void ConvexPolyhedraState::assign(Variable target,
                                  const TreeExpression& expression)
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

void ConvexPolyhedraState::assume(const LinearConstraint& constraint)
{
    if (impl_->bottom)
        return;
    if (constraint.kind() == ConstraintKind::NotEqual)
    {
        const LinearConstraint equality(constraint.expression(),
                                        ConstraintKind::Equal);
        if (entails(equality) == CheckResult::True)
            impl_->bottom = true;
        else
            report(OperationKind::Assumption,
                   ApproximationKind::SoundOverApproximation,
                   "non-convex disequality is ignored");
        return;
    }
    addConstraint(constraint);
    normalize();
}

void ConvexPolyhedraState::assume(const TreeConstraint& constraint)
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

void ConvexPolyhedraState::forget(Variable variable)
{
    if (!environment_.contains(variable))
        throw std::invalid_argument("forgotten variable is not in environment");
    if (impl_->bottom)
        return;
    impl_->inequalities = project(
        std::move(impl_->inequalities), {environment_.dimensionOf(variable)});
    normalize();
}

void ConvexPolyhedraState::changeEnvironment(const VariableEnvironment& environment,
                                             bool initializeNewVariablesToZero)
{
    for (const VariableDeclaration& declaration : environment.variables())
    {
        if (environment_.contains(declaration.variable) &&
                environment_.typeOf(declaration.variable) != declaration.type)
            throw std::invalid_argument(
                "environment change modifies a variable's numeric type");
    }
    if (impl_->bottom)
    {
        environment_ = environment;
        impl_->inequalities.clear();
        return;
    }

    std::vector<Variable> added;
    for (const VariableDeclaration& declaration : environment.variables())
    {
        if (!environment_.contains(declaration.variable))
            added.push_back(declaration.variable);
    }

    std::vector<std::size_t> removed;
    for (Dimension old = 0; old < environment_.size(); ++old)
    {
        if (!environment.contains(environment_.variableOf(old)))
            removed.push_back(old);
    }
    impl_->inequalities = project(std::move(impl_->inequalities), removed);

    std::vector<Inequality> remapped;
    remapped.reserve(impl_->inequalities.size());
    for (Inequality inequality : impl_->inequalities)
    {
        Inequality next;
        next.coefficients.resize(environment.size());
        next.bound = inequality.bound;
        next.strict = inequality.strict;
        for (Dimension old = 0; old < environment_.size(); ++old)
        {
            const Variable variable = environment_.variableOf(old);
            if (environment.contains(variable))
                next.coefficients[environment.dimensionOf(variable)] =
                    inequality.coefficients[old];
        }
        remapped.push_back(std::move(next));
    }
    environment_ = environment;
    impl_->inequalities = std::move(remapped);
    if (initializeNewVariablesToZero)
    {
        for (Variable variable : added)
            assume(equal(LinearExpression(variable),
                         LinearExpression(Rational())));
    }
    normalize();
}

CheckResult ConvexPolyhedraState::entails(
    const LinearConstraint& constraint) const
{
    if (impl_->bottom)
        return CheckResult::True;
    if (constraint.kind() == ConstraintKind::NotEqual)
    {
        // A disequality is provable when one strict side is entailed.
        const LinearConstraint less(constraint.expression(),
                                    ConstraintKind::LessThan);
        const LinearConstraint greater(constraint.expression(),
                                       ConstraintKind::GreaterThan);
        return entails(less) == CheckResult::True ||
                       entails(greater) == CheckResult::True
                   ? CheckResult::True
                   : CheckResult::Unknown;
    }
    if (constraint.kind() == ConstraintKind::Equal)
    {
        const LinearConstraint lower(constraint.expression(),
                                     ConstraintKind::LessEqual);
        const LinearConstraint upper(-constraint.expression(),
                                     ConstraintKind::LessEqual);
        return entails(lower) == CheckResult::True &&
                       entails(upper) == CheckResult::True
                   ? CheckResult::True
                   : CheckResult::Unknown;
    }
    const std::vector<Inequality> rows =
        constraintRows(environment_, constraint);
    if (rows.size() != 1)
        return CheckResult::Unknown;
    return ::entails(impl_->inequalities, environment_.size(), rows.front())
               ? CheckResult::True
               : CheckResult::Unknown;
}

Interval ConvexPolyhedraState::bound(Variable variable) const
{
    if (!environment_.contains(variable))
        throw std::invalid_argument("bounded variable is not in environment");
    if (impl_->bottom)
        return Interval(Bound::plusInfinity(), Bound::minusInfinity());

    const Dimension target = environment_.dimensionOf(variable);
    std::vector<std::size_t> removed;
    for (Dimension dimension = 0; dimension < environment_.size(); ++dimension)
    {
        if (dimension != target)
            removed.push_back(dimension);
    }
    const std::vector<Inequality> projected =
        project(impl_->inequalities, removed);
    Bound lower = Bound::minusInfinity();
    Bound upper = Bound::plusInfinity();
    for (const Inequality& inequality : projected)
    {
        const Rational coefficient = inequality.coefficients[target];
        if (coefficient.sign() > 0)
        {
            const Bound candidate = Bound::finite(
                inequality.bound / coefficient, inequality.strict);
            upper = Bound::min(upper, candidate);
        }
        else if (coefficient.sign() < 0)
        {
            const Bound candidate = Bound::finite(
                inequality.bound / coefficient, inequality.strict);
            if (lower.isMinusInfinity() ||
                    lower.value() < candidate.value() ||
                    (lower.value() == candidate.value() &&
                     candidate.isStrict() && !lower.isStrict()))
                lower = candidate;
        }
    }
    return Interval(lower, upper);
}

IntervalBox ConvexPolyhedraState::toBox() const
{
    IntervalBox result;
    for (const VariableDeclaration& declaration : environment_.variables())
        result.bounds.emplace(declaration.variable,
                              bound(declaration.variable));
    return result;
}

LinearConstraintSet ConvexPolyhedraState::toConstraints() const
{
    LinearConstraintSet result;
    if (impl_->bottom)
    {
        result.emplace_back(LinearExpression(Rational(1)),
                            ConstraintKind::LessEqual);
        return result;
    }
    result.reserve(impl_->inequalities.size());
    for (const Inequality& inequality : impl_->inequalities)
        result.push_back(rowConstraint(environment_, inequality));
    return result;
}

ConvexPolyhedraState ConvexPolyhedraState::join(
    const ConvexPolyhedraState& other) const
{
    requirePolyhedron(other);
    if (impl_->bottom)
        return other;
    if (other.impl_->bottom)
        return *this;

    const std::size_t dimensions = environment_.size();
    const std::size_t yOffset = dimensions;
    const std::size_t zOffset = 2 * dimensions;
    const std::size_t lambda = 3 * dimensions;
    const std::size_t extendedDimensions = lambda + 1;
    std::vector<Inequality> extended;

    // A*y <= lambda*b. Strict facets are closed here: closure(conv(P,Q)) is
    // a sound convex over-approximation for non-closed inputs.
    for (const Inequality& inequality : impl_->inequalities)
    {
        Inequality row;
        row.coefficients.resize(extendedDimensions);
        std::copy(inequality.coefficients.begin(),
                  inequality.coefficients.end(),
                  row.coefficients.begin() + yOffset);
        row.coefficients[lambda] = -inequality.bound;
        row.bound = Rational();
        extended.push_back(std::move(row));
    }
    // B*z <= (1-lambda)*d.
    for (const Inequality& inequality : other.impl_->inequalities)
    {
        Inequality row;
        row.coefficients.resize(extendedDimensions);
        std::copy(inequality.coefficients.begin(),
                  inequality.coefficients.end(),
                  row.coefficients.begin() + zOffset);
        row.coefficients[lambda] = inequality.bound;
        row.bound = inequality.bound;
        extended.push_back(std::move(row));
    }
    // x = y + z.
    for (Dimension dimension = 0; dimension < dimensions; ++dimension)
    {
        Inequality row;
        row.coefficients.resize(extendedDimensions);
        row.coefficients[dimension] = Rational(1);
        row.coefficients[yOffset + dimension] = Rational(-1);
        row.coefficients[zOffset + dimension] = Rational(-1);
        row.bound = Rational();
        extended.push_back(row);
        for (Rational& coefficient : row.coefficients)
            coefficient = -coefficient;
        extended.push_back(std::move(row));
    }
    // 0 <= lambda <= 1.
    Inequality lambdaLower;
    lambdaLower.coefficients.resize(extendedDimensions);
    lambdaLower.coefficients[lambda] = Rational(-1);
    lambdaLower.bound = Rational();
    extended.push_back(std::move(lambdaLower));
    Inequality lambdaUpper;
    lambdaUpper.coefficients.resize(extendedDimensions);
    lambdaUpper.coefficients[lambda] = Rational(1);
    lambdaUpper.bound = Rational(1);
    extended.push_back(std::move(lambdaUpper));

    std::vector<std::size_t> removed;
    for (std::size_t dimension = dimensions;
         dimension < extendedDimensions; ++dimension)
        removed.push_back(dimension);
    extended = project(std::move(extended), removed);
    for (Inequality& inequality : extended)
        inequality.coefficients.resize(dimensions);

    ConvexPolyhedraState result = top(environment_, config_);
    result.impl_->inequalities = std::move(extended);
    result.normalize();
    return result;
}

ConvexPolyhedraState ConvexPolyhedraState::meet(
    const ConvexPolyhedraState& other) const
{
    ConvexPolyhedraState result(*this);
    result.meetState(other);
    return result;
}

ConvexPolyhedraState ConvexPolyhedraState::widen(
    const ConvexPolyhedraState& next) const
{
    requirePolyhedron(next);
    if (impl_->bottom)
        return next;
    if (next.impl_->bottom)
        return *this;
    ConvexPolyhedraState result = top(environment_, config_);
    for (const Inequality& inequality : impl_->inequalities)
    {
        if (::entails(next.impl_->inequalities, environment_.size(),
                      inequality))
            result.impl_->inequalities.push_back(inequality);
    }
    result.normalize();
    return result;
}

ConvexPolyhedraState ConvexPolyhedraState::narrow(
    const ConvexPolyhedraState& next) const
{
    return meet(next);
}

bool ConvexPolyhedraState::hasCompatibleDomain(
    const AbstractState& other) const
{
    const auto* polyhedron = dynamic_cast<const ConvexPolyhedraState*>(&other);
    return polyhedron && environment_ == polyhedron->environment_ &&
           config_.operationCompatible(polyhedron->config_);
}

void ConvexPolyhedraState::joinState(const AbstractState& other)
{
    *this = join(requirePolyhedron(other));
}

void ConvexPolyhedraState::meetState(const AbstractState& other)
{
    const ConvexPolyhedraState& polyhedron = requirePolyhedron(other);
    if (impl_->bottom || polyhedron.impl_->bottom)
    {
        impl_->bottom = true;
        impl_->inequalities.clear();
        return;
    }
    impl_->inequalities.insert(impl_->inequalities.end(),
                         polyhedron.impl_->inequalities.begin(),
                         polyhedron.impl_->inequalities.end());
    normalize();
}

void ConvexPolyhedraState::widenState(const AbstractState& next)
{
    *this = widen(requirePolyhedron(next));
}

void ConvexPolyhedraState::narrowState(const AbstractState& next)
{
    *this = narrow(requirePolyhedron(next));
}

bool ConvexPolyhedraState::isBottomState() const
{
    return impl_->bottom;
}

bool ConvexPolyhedraState::isTopState() const
{
    return !impl_->bottom && impl_->inequalities.empty();
}

bool ConvexPolyhedraState::leqState(const AbstractState& other) const
{
    const ConvexPolyhedraState& polyhedron = requirePolyhedron(other);
    if (impl_->bottom)
        return true;
    if (polyhedron.impl_->bottom)
        return false;
    return std::all_of(
        polyhedron.impl_->inequalities.begin(),
        polyhedron.impl_->inequalities.end(),
        [&](const Inequality& inequality)
        {
            return ::entails(impl_->inequalities, environment_.size(),
                             inequality);
        });
}

std::string ConvexPolyhedraState::stateToString() const
{
    if (impl_->bottom)
        return "bottom";
    std::ostringstream output;
    output << "{";
    for (std::size_t index = 0; index < impl_->inequalities.size(); ++index)
    {
        if (index != 0)
            output << ", ";
        output << rowConstraint(environment_, impl_->inequalities[index])
                      .toString(&environment_);
    }
    output << "}";
    return output.str();
}

const ConvexPolyhedraState& ConvexPolyhedraState::requirePolyhedron(
    const AbstractState& other) const
{
    requireCompatible(other);
    return static_cast<const ConvexPolyhedraState&>(other);
}

void ConvexPolyhedraState::addConstraint(
    const LinearConstraint& constraint)
{
    std::vector<Inequality> rows = constraintRows(environment_, constraint);
    impl_->inequalities.insert(impl_->inequalities.end(),
                         std::make_move_iterator(rows.begin()),
                         std::make_move_iterator(rows.end()));
}

void ConvexPolyhedraState::normalize()
{
    if (impl_->bottom)
    {
        impl_->inequalities.clear();
        return;
    }
    impl_->inequalities =
        normalized(std::move(impl_->inequalities), impl_->bottom);
    if (!impl_->bottom && !feasible(impl_->inequalities, environment_.size()))
    {
        impl_->bottom = true;
        impl_->inequalities.clear();
        return;
    }
    if (!impl_->bottom)
        impl_->inequalities =
            irredundant(std::move(impl_->inequalities), environment_.size());
}

void ConvexPolyhedraState::report(OperationKind operation,
                                  ApproximationKind approximation,
                                  std::string reason) const
{
    if (config_.diagnostics)
        config_.diagnostics->report(
            {operation, approximation, std::move(reason)});
}

namespace
{

std::vector<Inequality> project(
    std::vector<Inequality> inequalities,
    const std::vector<std::size_t>& dimensions)
{
    const std::size_t width =
        inequalities.empty() ? 0 : inequalities.front().coefficients.size();
    std::vector<std::size_t> order = dimensions;
    std::sort(order.begin(), order.end());
    order.erase(std::unique(order.begin(), order.end()), order.end());

    // The caller fixes which dimensions go, not the order they go in, and one
    // elimination step emits #positive * #negative rows. Taking the cheapest
    // dimension first (the Cha-Chan-Loo rule) keeps the intermediate systems
    // small, which matters far more than the step count.
    const auto stepCost = [&](std::size_t dimension)
    {
        std::size_t positive = 0;
        std::size_t negative = 0;
        for (const Inequality& inequality : inequalities)
        {
            const int sign = inequality.coefficients[dimension].sign();
            if (sign > 0)
                ++positive;
            else if (sign < 0)
                ++negative;
        }
        return positive * negative;
    };

    while (!order.empty())
    {
        auto cheapest = order.begin();
        std::size_t best = stepCost(*cheapest);
        for (auto candidate = std::next(order.begin()); candidate != order.end();
             ++candidate)
        {
            const std::size_t cost = stepCost(*candidate);
            if (cost < best)
            {
                best = cost;
                cheapest = candidate;
            }
        }
        const std::size_t dimension = *cheapest;
        order.erase(cheapest);

        inequalities = eliminate(std::move(inequalities), dimension);
        bool bottom = false;
        inequalities = normalized(std::move(inequalities), bottom);
        if (bottom)
        {
            Inequality contradiction;
            contradiction.coefficients.resize(width);
            contradiction.bound = Rational(-1);
            return {std::move(contradiction)};
        }
        // Reduce between elimination steps, not only at the end: each step
        // costs one row per (upper, lower) pair of the previous step, so
        // carrying implied rows forward is what makes the elimination
        // superexponential rather than merely exponential. Reduction itself
        // costs one linear program per row, so it only pays once a step can
        // actually produce a large product; below that the next elimination is
        // cheaper than the test would be.
        constexpr std::size_t reductionThreshold = 16;
        if (inequalities.size() > reductionThreshold)
            inequalities = irredundant(std::move(inequalities), width);
    }
    return inequalities;
}

// ---------------------------------------------------------------------------
// Exact rational simplex.
//
// Feasibility used to be decided by running Fourier-Motzkin to completion,
// which costs one row per (upper, lower) pair at every step. That is fine as a
// definition and unusable as a subroutine: the redundancy test below calls it
// once per candidate row, so an exponential feasibility check makes redundancy
// removal cost more than the redundancy it removes. A two-phase primal simplex
// over GMP rationals decides the same question exactly, with Bland's rule for
// guaranteed termination.
// ---------------------------------------------------------------------------

/// Tableau: `rows` equality rows plus one objective row, `columns` variable
/// columns plus one right-hand-side column. `basis[i]` is the column basic in
/// row i. Maximizes the objective; returns false when it is unbounded.
class Tableau
{
public:
    Tableau(std::size_t rows, std::size_t columns)
        : columns_(columns), cells_(rows + 1,
                                    std::vector<mpq_class>(columns + 1)),
          basis_(rows)
    {
    }

    std::size_t rows() const { return basis_.size(); }
    std::size_t columns() const { return columns_; }
    mpq_class& at(std::size_t row, std::size_t column)
    {
        return cells_[row][column];
    }
    const mpq_class& at(std::size_t row, std::size_t column) const
    {
        return cells_[row][column];
    }
    mpq_class& rhs(std::size_t row) { return cells_[row][columns_]; }
    const mpq_class& rhs(std::size_t row) const { return cells_[row][columns_]; }
    std::size_t& basis(std::size_t row) { return basis_[row]; }
    std::size_t basis(std::size_t row) const { return basis_[row]; }
    /// The objective row lives one past the last constraint row.
    std::size_t objective() const { return basis_.size(); }

    void pivot(std::size_t row, std::size_t column)
    {
        const mpq_class inverse = 1 / cells_[row][column];
        for (mpq_class& cell : cells_[row])
            cell *= inverse;
        for (std::size_t other = 0; other < cells_.size(); ++other)
        {
            if (other == row || cells_[other][column] == 0)
                continue;
            const mpq_class factor = cells_[other][column];
            for (std::size_t index = 0; index <= columns_; ++index)
                cells_[other][index] -= factor * cells_[row][index];
        }
        basis_[row] = column;
    }

    /// Primal simplex with Bland's rule: always take the lowest eligible
    /// entering column and break ratio ties on the lowest basic column index,
    /// which makes cycling impossible.
    bool maximize()
    {
        const std::size_t last = objective();
        while (true)
        {
            std::size_t entering = columns_;
            for (std::size_t column = 0; column < columns_; ++column)
            {
                if (cells_[last][column] > 0)
                {
                    entering = column;
                    break;
                }
            }
            if (entering == columns_)
                return true;

            std::size_t leaving = basis_.size();
            mpq_class best;
            for (std::size_t row = 0; row < basis_.size(); ++row)
            {
                if (cells_[row][entering] <= 0)
                    continue;
                const mpq_class ratio =
                    cells_[row][columns_] / cells_[row][entering];
                if (leaving == basis_.size() || ratio < best ||
                    (ratio == best && basis_[row] < basis_[leaving]))
                {
                    best = ratio;
                    leaving = row;
                }
            }
            if (leaving == basis_.size())
                return false;
            pivot(leaving, entering);
        }
    }

    /// Maximized objective value.
    mpq_class value() const { return -cells_[basis_.size()][columns_]; }

    /// Discard the trailing columns and every row still basic on one of them.
    ///
    /// Phase I's artificial variables must not merely be kept out of the
    /// basis for phase II: an artificial that is still basic sits at zero but
    /// grows again as soon as an entering column has a negative entry in its
    /// row, because the ratio test only bounds rows with positive entries. A
    /// phase II run over such a tableau optimizes a relaxation of the original
    /// system. Deleting the columns removes the variables outright. A row
    /// whose artificial cannot be pivoted out has no support outside them, so
    /// it is the redundant equality 0 = 0 and goes with them.
    void dropTrailing(std::size_t keep)
    {
        std::vector<std::vector<mpq_class>> cells;
        std::vector<std::size_t> basis;
        const auto compact = [&](const std::vector<mpq_class>& row)
        {
            std::vector<mpq_class> result(row.begin(), row.begin() + keep);
            result.push_back(row[columns_]);
            return result;
        };
        for (std::size_t row = 0; row < basis_.size(); ++row)
        {
            if (basis_[row] >= keep)
                continue;
            cells.push_back(compact(cells_[row]));
            basis.push_back(basis_[row]);
        }
        cells.push_back(compact(cells_.back()));
        cells_ = std::move(cells);
        basis_ = std::move(basis);
        columns_ = keep;
    }

private:
    std::size_t columns_;
    std::vector<std::vector<mpq_class>> cells_;
    std::vector<std::size_t> basis_;
};

bool feasible(std::vector<Inequality> inequalities, std::size_t dimensions)
{
    // Drop rows that carry no information, and fail fast on `0 <= negative`.
    std::vector<Inequality> rows;
    rows.reserve(inequalities.size());
    for (Inequality& inequality : inequalities)
    {
        if (falseConstant(inequality))
            return false;
        if (trueConstant(inequality))
            continue;
        rows.push_back(std::move(inequality));
    }
    if (rows.empty())
        return true;

    const bool anyStrict =
        std::any_of(rows.begin(), rows.end(),
                    [](const Inequality& row) { return row.strict; });

    // Projection zeroes a dimension's column but keeps its width, and the join
    // lift triples the width before eliminating most of it. Carrying those
    // dead columns into the tableau costs two simplex variables each for no
    // information, so map only the live ones.
    std::vector<std::size_t> live;
    for (std::size_t dimension = 0; dimension < dimensions; ++dimension)
    {
        const bool used = std::any_of(
            rows.begin(), rows.end(), [&](const Inequality& row)
            { return !row.coefficients[dimension].isZero(); });
        if (used)
            live.push_back(dimension);
    }
    dimensions = live.size();

    // Each free x_j becomes u_j - v_j with u, v >= 0. A strict row a.x < b
    // becomes a.x + epsilon <= b, and the system is strictly feasible exactly
    // when max epsilon > 0 under the extra row epsilon <= 1.
    const std::size_t constraintCount = rows.size() + (anyStrict ? 1 : 0);
    const std::size_t positivePart = 0;
    const std::size_t negativePart = dimensions;
    const std::size_t epsilonColumn = 2 * dimensions;
    const std::size_t slackBase = epsilonColumn + (anyStrict ? 1 : 0);
    const std::size_t artificialBase = slackBase + constraintCount;
    const std::size_t columns = artificialBase + constraintCount;

    Tableau tableau(constraintCount, columns);
    for (std::size_t row = 0; row < rows.size(); ++row)
    {
        for (std::size_t dimension = 0; dimension < dimensions; ++dimension)
        {
            const mpq_class& coefficient =
                rows[row].coefficients[live[dimension]].value();
            tableau.at(row, positivePart + dimension) = coefficient;
            tableau.at(row, negativePart + dimension) = -coefficient;
        }
        if (anyStrict && rows[row].strict)
            tableau.at(row, epsilonColumn) = 1;
        tableau.at(row, slackBase + row) = 1;
        tableau.rhs(row) = rows[row].bound.value();
    }
    if (anyStrict)
    {
        const std::size_t row = rows.size();
        tableau.at(row, epsilonColumn) = 1;
        tableau.at(row, slackBase + row) = 1;
        tableau.rhs(row) = 1;
    }

    // Phase I needs a non-negative right-hand side and a starting basis.
    for (std::size_t row = 0; row < constraintCount; ++row)
    {
        if (tableau.rhs(row) < 0)
        {
            for (std::size_t column = 0; column <= columns; ++column)
                tableau.at(row, column) = -tableau.at(row, column);
        }
        tableau.at(row, artificialBase + row) = 1;
        tableau.basis(row) = artificialBase + row;
    }

    // Maximize -(sum of artificials); the system is feasible iff that is 0.
    const std::size_t objective = tableau.objective();
    for (std::size_t row = 0; row < constraintCount; ++row)
    {
        for (std::size_t column = 0; column <= columns; ++column)
            tableau.at(objective, column) += tableau.at(row, column);
    }
    for (std::size_t row = 0; row < constraintCount; ++row)
        tableau.at(objective, artificialBase + row) = 0;

    tableau.maximize();
    if (tableau.value() < 0)
        return false;
    if (!anyStrict)
        return true;

    // Phase II. First pivot every artificial out of the basis where the row
    // has any other support, then delete the artificial columns entirely.
    for (std::size_t row = 0; row < constraintCount; ++row)
    {
        if (tableau.basis(row) < artificialBase)
            continue;
        for (std::size_t column = 0; column < artificialBase; ++column)
        {
            if (tableau.at(row, column) != 0)
            {
                tableau.pivot(row, column);
                break;
            }
        }
    }
    tableau.dropTrailing(artificialBase);

    // The system is strictly feasible exactly when epsilon can be made
    // positive, so maximize it over what phase I left behind.
    const std::size_t phaseTwo = tableau.objective();
    for (std::size_t column = 0; column <= tableau.columns(); ++column)
        tableau.at(phaseTwo, column) = 0;
    tableau.at(phaseTwo, epsilonColumn) = 1;
    for (std::size_t row = 0; row < tableau.rows(); ++row)
    {
        const mpq_class factor = tableau.at(phaseTwo, tableau.basis(row));
        if (factor == 0)
            continue;
        for (std::size_t column = 0; column <= tableau.columns(); ++column)
            tableau.at(phaseTwo, column) -= factor * tableau.at(row, column);
    }

    tableau.maximize();
    return tableau.value() > 0;
}

bool entails(const std::vector<Inequality>& premises, std::size_t dimensions,
             const Inequality& conclusion)
{
    std::vector<Inequality> counterexample = premises;
    counterexample.push_back(negateForCounterexample(conclusion));
    return !feasible(std::move(counterexample), dimensions);
}

/// Drop every row implied by the others.
///
/// Fourier-Motzkin elimination produces one row per (upper, lower) pair, so
/// the vast majority of what it emits is implied by the rest of the system.
/// `normalized` only merges rows whose scaled coefficient vectors are
/// identical, which leaves that redundancy in place: the join of two rational
/// points in the plane keeps 42 rows for a segment that needs three. The rows
/// are all correct, but the next operation's cost is driven by how many of
/// them survive, so the count compounds multiplicatively across joins and the
/// domain stops terminating at three dimensions.
///
/// Each candidate is tested against every row that is still in the system --
/// those already kept plus those not yet examined -- so dropping one row never
/// invalidates a later test.
std::vector<Inequality> irredundant(std::vector<Inequality> inequalities,
                                    std::size_t dimensions)
{
    if (inequalities.size() < 2)
        return inequalities;

    std::vector<Inequality> kept;
    kept.reserve(inequalities.size());
    for (std::size_t index = 0; index < inequalities.size(); ++index)
    {
        std::vector<Inequality> rest = kept;
        rest.insert(rest.end(), inequalities.begin() + index + 1,
                    inequalities.end());
        if (!entails(rest, dimensions, inequalities[index]))
            kept.push_back(std::move(inequalities[index]));
    }
    return kept;
}

} // namespace
