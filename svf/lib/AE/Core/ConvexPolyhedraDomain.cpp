//===- ConvexPolyhedraDomain.cpp -- Exact rational polyhedra ------------===//

#include "AE/Core/ConvexPolyhedraDomain.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <limits>
#include <numeric>
#include <optional>
#include <set>
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

/// A homogeneous generator `(t,x)`. A positive `t` denotes the point
/// `x/t`; `t == 0` denotes a recession ray. Lines are represented by the
/// corresponding pair of opposite rays. This keeps the public domain
/// interface independent of the H/V implementation detail.
struct Generator
{
    std::vector<Rational> coordinates;
    /// Bit i is set when this generator saturates processed homogeneous
    /// constraint i.  The cache is rebuilt whenever its constraint basis
    /// changes; it is not part of generator identity.
    std::vector<std::uint64_t> saturation;
};

std::vector<Inequality> project(
    std::vector<Inequality> inequalities,
    const std::vector<std::size_t>& dimensions);
bool feasible(std::vector<Inequality> inequalities, std::size_t dimensions);
bool entails(const std::vector<Inequality>& premises, std::size_t dimensions,
             const Inequality& conclusion);
std::vector<Inequality> irredundant(std::vector<Inequality> inequalities,
                                    std::size_t dimensions);
std::vector<Generator> generatorsFromConstraints(
    const std::vector<Inequality>& inequalities, std::size_t dimensions);
std::vector<Inequality> constraintsFromGenerators(
    const std::vector<Generator>& generators, std::size_t dimensions);
std::vector<Generator> irredundantGenerators(
    std::vector<Generator> generators);
std::vector<Generator> uniqueGenerators(std::vector<Generator> generators);
std::vector<Generator> mergeGeneratorSets(
    const std::vector<Generator>& lhs, const std::vector<Generator>& rhs);
bool generatorsEntail(const std::vector<Generator>& generators,
                      const Inequality& inequality);
Interval boundFromGenerators(const std::vector<Generator>& generators,
                             const std::vector<Rational>& objective,
                             const Rational& constant);
std::vector<Generator> intersectGeneratorsWithConstraints(
    std::vector<Generator> generators,
    const std::vector<Inequality>& existing,
    const std::vector<Inequality>& added, std::size_t dimensions);

bool hasStrictConstraint(const std::vector<Inequality>& inequalities)
{
    return std::any_of(inequalities.begin(), inequalities.end(),
                       [](const Inequality& inequality)
                       { return inequality.strict; });
}

bool sameInequality(const Inequality& lhs, const Inequality& rhs)
{
    return lhs.coefficients == rhs.coefficients && lhs.bound == rhs.bound &&
           lhs.strict == rhs.strict;
}

bool hasIntegerVariable(const VariableEnvironment& environment)
{
    return std::any_of(
        environment.variables().begin(), environment.variables().end(),
        [](const VariableDeclaration& declaration)
        { return declaration.type.kind == NumericKind::Integer; });
}

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

bool oppositeRows(const Inequality& lhs, const Inequality& rhs)
{
    if (lhs.strict || rhs.strict || lhs.bound != -rhs.bound ||
        lhs.coefficients.size() != rhs.coefficients.size())
        return false;
    for (std::size_t dimension = 0; dimension < lhs.coefficients.size();
         ++dimension)
    {
        if (lhs.coefficients[dimension] != -rhs.coefficients[dimension])
            return false;
    }
    return true;
}

/// Put explicit equality pairs in reduced row-echelon form and reduce every
/// inequality modulo them.  A facet of a lower-dimensional polyhedron is only
/// unique modulo its affine hull; without this step equivalent H/V histories
/// can choose different-looking rows and consequently different hashes.
std::vector<Inequality> canonicalizeAffineHull(
    std::vector<Inequality> inequalities, bool& bottom)
{
    if (inequalities.size() < 2)
        return inequalities;

    std::vector<bool> equalityMember(inequalities.size(), false);
    std::vector<Inequality> equations;
    for (std::size_t lhs = 0; lhs < inequalities.size(); ++lhs)
    {
        if (equalityMember[lhs] || inequalities[lhs].strict)
            continue;
        for (std::size_t rhs = lhs + 1; rhs < inequalities.size(); ++rhs)
        {
            if (equalityMember[rhs] ||
                !oppositeRows(inequalities[lhs], inequalities[rhs]))
                continue;
            equalityMember[lhs] = true;
            equalityMember[rhs] = true;
            const auto first = std::find_if(
                inequalities[lhs].coefficients.begin(),
                inequalities[lhs].coefficients.end(),
                [](const Rational& coefficient)
                { return !coefficient.isZero(); });
            equations.push_back(first->sign() > 0 ? inequalities[lhs]
                                                   : inequalities[rhs]);
            break;
        }
    }
    if (equations.empty())
        return inequalities;

    std::vector<std::size_t> pivots;
    std::size_t row = 0;
    const std::size_t dimensions = equations.front().coefficients.size();
    for (std::size_t dimension = 0;
         dimension < dimensions && row < equations.size(); ++dimension)
    {
        auto pivot = std::find_if(
            equations.begin() + row, equations.end(),
            [&](const Inequality& equation)
            { return !equation.coefficients[dimension].isZero(); });
        if (pivot == equations.end())
            continue;
        std::iter_swap(equations.begin() + row, pivot);
        const Rational divisor = equations[row].coefficients[dimension];
        for (Rational& coefficient : equations[row].coefficients)
            coefficient /= divisor;
        equations[row].bound /= divisor;
        for (std::size_t other = 0; other < equations.size(); ++other)
        {
            if (other == row ||
                equations[other].coefficients[dimension].isZero())
                continue;
            const Rational factor =
                equations[other].coefficients[dimension];
            for (std::size_t column = 0; column < dimensions; ++column)
            {
                equations[other].coefficients[column] -=
                    factor * equations[row].coefficients[column];
            }
            equations[other].bound -= factor * equations[row].bound;
        }
        pivots.push_back(dimension);
        ++row;
    }
    for (std::size_t dependent = row; dependent < equations.size();
         ++dependent)
    {
        const bool zero = std::all_of(
            equations[dependent].coefficients.begin(),
            equations[dependent].coefficients.end(),
            [](const Rational& coefficient) { return coefficient.isZero(); });
        if (zero && !equations[dependent].bound.isZero())
        {
            bottom = true;
            return {};
        }
    }
    equations.resize(row);

    std::vector<Inequality> result;
    result.reserve(2 * equations.size() + inequalities.size());
    for (const Inequality& equation : equations)
    {
        result.push_back(equation);
        Inequality opposite = equation;
        for (Rational& coefficient : opposite.coefficients)
            coefficient = -coefficient;
        opposite.bound = -opposite.bound;
        result.push_back(std::move(opposite));
    }
    for (std::size_t index = 0; index < inequalities.size(); ++index)
    {
        if (equalityMember[index])
            continue;
        Inequality reduced = std::move(inequalities[index]);
        for (std::size_t equation = 0; equation < equations.size(); ++equation)
        {
            const Rational factor = reduced.coefficients[pivots[equation]];
            if (factor.isZero())
                continue;
            for (std::size_t dimension = 0; dimension < dimensions; ++dimension)
            {
                reduced.coefficients[dimension] -=
                    factor * equations[equation].coefficients[dimension];
            }
            reduced.bound -= factor * equations[equation].bound;
        }
        result.push_back(std::move(reduced));
    }
    return normalized(std::move(result), bottom);
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

void tightenIntegerRow(Inequality& inequality,
                       const VariableEnvironment& environment)
{
    mpz_class scale = 1;
    bool hasVariable = false;
    for (Dimension dimension = 0; dimension < environment.size(); ++dimension)
    {
        if (inequality.coefficients[dimension].isZero())
            continue;
        hasVariable = true;
        if (environment.typeOf(environment.variableOf(dimension)).kind !=
            NumericKind::Integer)
            return;
        const mpz_class denominator =
            inequality.coefficients[dimension].value().get_den();
        mpz_lcm(scale.get_mpz_t(), scale.get_mpz_t(),
                denominator.get_mpz_t());
    }
    if (!hasVariable)
        return;
    const mpz_class boundDenominator = inequality.bound.value().get_den();
    mpz_lcm(scale.get_mpz_t(), scale.get_mpz_t(),
            boundDenominator.get_mpz_t());

    mpz_class divisor = 0;
    for (const Rational& coefficient : inequality.coefficients)
    {
        if (coefficient.isZero())
            continue;
        const mpq_class scaledCoefficient = coefficient.value() * scale;
        const mpz_class integerCoefficient = scaledCoefficient.get_num();
        mpz_gcd(divisor.get_mpz_t(), divisor.get_mpz_t(),
                integerCoefficient.get_mpz_t());
    }
    if (divisor == 0)
        return;
    if (divisor < 0)
        divisor = -divisor;

    const mpq_class scaledBound = inequality.bound.value() * scale;
    const mpz_class integerBound = scaledBound.get_num();
    mpz_class quotient;
    mpz_fdiv_q(quotient.get_mpz_t(), integerBound.get_mpz_t(),
               divisor.get_mpz_t());
    mpz_class tightened = quotient * divisor;
    if (inequality.strict && tightened == integerBound)
        tightened -= divisor;

    inequality.bound = Rational::fromRaw(mpq_class(tightened, scale));
    inequality.strict = false;
}

} // namespace

class ConvexPolyhedraState::Impl
{
public:
    mutable std::vector<Inequality> inequalities;
    mutable std::vector<Generator> generators;
    mutable bool constraintsValid = true;
    mutable bool generatorsValid = false;
    /// NNC constraints are converted to generators for their closure. Such a
    /// cache is useful for join, whose documented result is closed, but must
    /// not be used by exact assignment/projection/query operations.
    mutable bool generatorsExact = false;
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
    result.assumeAll(constraints);
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

void ConvexPolyhedraState::assign(
    Variable target, const LinearExpression& expression)
{
    assignParallel({{target, expression}});
}

void ConvexPolyhedraState::assignParallel(
    const LinearAssignmentList& assignments)
{
    std::map<Variable, const LinearExpression*> expressions;
    for (const LinearAssignment& assignment : assignments)
    {
        if (!environment_.contains(assignment.target))
            throw std::invalid_argument(
                "parallel assignment target is not in environment");
        if (!expressions.emplace(assignment.target, &assignment.expression)
                 .second)
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
    if (assignments.empty() || impl_->bottom)
        return;

    ensureGenerators();
    if (impl_->generatorsExact)
    {
        for (Generator& generator : impl_->generators)
        {
            const std::vector<Rational> old = generator.coordinates;
            for (const auto& [target, expression] : expressions)
            {
                Rational value = expression->constant() * old.front();
                for (const auto& [variable, coefficient] : expression->terms())
                {
                    value += coefficient *
                        old[environment_.dimensionOf(variable) + 1];
                }
                generator.coordinates[environment_.dimensionOf(target) + 1] =
                    std::move(value);
            }
        }
        // An affine image of a generating set stays exact even when some
        // generators become redundant. Normalize/deduplicate cheaply and let
        // the next H conversion remove semantic redundancy in one batch.
        impl_->generators = uniqueGenerators(std::move(impl_->generators));
        invalidateConstraints();
        // Real-only states can remain V-only until a constraint operation
        // requests H. Integer states materialize H now so the existing per-row
        // tightening remains part of assignment semantics.
        if (config_.integerTightening && hasIntegerVariable(environment_))
            ensureConstraints();
        return;
    }

    ensureConstraints();

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

    for (Dimension dimension = 0; dimension < dimensions; ++dimension)
    {
        Inequality equality;
        equality.coefficients.resize(2 * dimensions);
        equality.coefficients[dimension] = Rational(1);
        const Variable target = environment_.variableOf(dimension);
        const auto assigned = expressions.find(target);
        if (assigned != expressions.end())
        {
            for (const auto& [variable, coefficient] :
                 assigned->second->terms())
            {
                equality.coefficients[dimensions +
                    environment_.dimensionOf(variable)] -= coefficient;
            }
            equality.bound = assigned->second->constant();
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

void ConvexPolyhedraState::substitute(
    Variable target, const LinearExpression& expression)
{
    substituteParallel({{target, expression}});
}

void ConvexPolyhedraState::substituteParallel(
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
    if (assignments.empty() || impl_->bottom)
        return;

    LinearConstraintSet preimage;
    for (const LinearConstraint& constraint : toConstraints())
        preimage.emplace_back(
            constraint.expression().substituted(replacements),
            constraint.kind());
    *this = fromConstraints(environment_, preimage, config_);
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

    std::vector<Inequality> rows = constraintRows(environment_, constraint);
    if (!rows.empty() && impl_->generatorsValid && impl_->generatorsExact &&
        !hasStrictConstraint(rows))
    {
        const std::vector<Inequality> noConstraints;
        impl_->generators = intersectGeneratorsWithConstraints(
            std::move(impl_->generators),
            impl_->constraintsValid ? impl_->inequalities : noConstraints,
            rows, environment_.size());
        if (impl_->generators.empty())
        {
            impl_->bottom = true;
            impl_->inequalities.clear();
            impl_->constraintsValid = true;
            impl_->generatorsValid = false;
            impl_->generatorsExact = false;
            return;
        }
        impl_->generatorsValid = true;
        impl_->generatorsExact = true;
        invalidateConstraints();
        if (config_.integerTightening && hasIntegerVariable(environment_))
            ensureConstraints();
        return;
    }

    ensureConstraints();
    impl_->inequalities.insert(impl_->inequalities.end(), rows.begin(),
                               rows.end());
    normalize();
}

void ConvexPolyhedraState::assumeAll(
    const LinearConstraintSet& constraints)
{
    if (impl_->bottom || constraints.empty())
        return;

    LinearConstraintSet disequalities;
    std::vector<Inequality> rows;
    for (const LinearConstraint& constraint : constraints)
    {
        if (constraint.kind() == ConstraintKind::NotEqual)
        {
            disequalities.push_back(constraint);
            continue;
        }
        std::vector<Inequality> next =
            constraintRows(environment_, constraint);
        rows.insert(rows.end(), std::make_move_iterator(next.begin()),
                    std::make_move_iterator(next.end()));
    }

    if (!rows.empty() && impl_->generatorsValid && impl_->generatorsExact &&
        !hasStrictConstraint(rows))
    {
        const std::vector<Inequality> noConstraints;
        impl_->generators = intersectGeneratorsWithConstraints(
            std::move(impl_->generators),
            impl_->constraintsValid ? impl_->inequalities : noConstraints,
            rows, environment_.size());
        if (impl_->generators.empty())
        {
            impl_->bottom = true;
            impl_->inequalities.clear();
            impl_->constraintsValid = true;
            impl_->generatorsValid = false;
            impl_->generatorsExact = false;
        }
        else
        {
            impl_->generatorsValid = true;
            impl_->generatorsExact = true;
            invalidateConstraints();
            if (config_.integerTightening &&
                hasIntegerVariable(environment_))
                ensureConstraints();
        }
    }
    else if (!rows.empty())
    {
        ensureConstraints();
        impl_->inequalities.insert(impl_->inequalities.end(), rows.begin(),
                                   rows.end());
        // A convex conjunction needs one normalization pass. Disequalities
        // are checked afterwards so x != 0 and x = 0 are order independent.
        normalize();
    }
    for (const LinearConstraint& disequality : disequalities)
        assume(disequality);
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

    ensureGenerators();
    if (impl_->generatorsExact)
    {
        const Dimension forgotten = environment_.dimensionOf(variable) + 1;
        for (Generator& generator : impl_->generators)
            generator.coordinates[forgotten] = Rational();
        Generator direction;
        direction.coordinates.resize(environment_.size() + 1);
        direction.coordinates[forgotten] = Rational(1);
        impl_->generators.push_back(direction);
        direction.coordinates[forgotten] = Rational(-1);
        impl_->generators.push_back(std::move(direction));
        impl_->generators = uniqueGenerators(std::move(impl_->generators));
        invalidateConstraints();
        if (config_.integerTightening && hasIntegerVariable(environment_))
            ensureConstraints();
        return;
    }

    ensureConstraints();
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
        impl_->generators.clear();
        impl_->constraintsValid = true;
        impl_->generatorsValid = false;
        impl_->generatorsExact = false;
        return;
    }

    ensureGenerators();
    if (impl_->generatorsExact)
    {
        std::vector<Generator> remapped;
        remapped.reserve(impl_->generators.size() + 2 * environment.size());
        for (const Generator& generator : impl_->generators)
        {
            Generator next;
            next.coordinates.resize(environment.size() + 1);
            next.coordinates.front() = generator.coordinates.front();
            for (Dimension old = 0; old < environment_.size(); ++old)
            {
                const Variable variable = environment_.variableOf(old);
                if (environment.contains(variable))
                {
                    next.coordinates[environment.dimensionOf(variable) + 1] =
                        generator.coordinates[old + 1];
                }
            }
            remapped.push_back(std::move(next));
        }
        if (!initializeNewVariablesToZero)
        {
            for (const VariableDeclaration& declaration :
                 environment.variables())
            {
                if (environment_.contains(declaration.variable))
                    continue;
                Generator direction;
                direction.coordinates.resize(environment.size() + 1);
                direction.coordinates[
                    environment.dimensionOf(declaration.variable) + 1] =
                    Rational(1);
                remapped.push_back(direction);
                direction.coordinates[
                    environment.dimensionOf(declaration.variable) + 1] =
                    Rational(-1);
                remapped.push_back(std::move(direction));
            }
        }
        environment_ = environment;
        impl_->generators = uniqueGenerators(std::move(remapped));
        impl_->generatorsValid = true;
        impl_->generatorsExact = true;
        invalidateConstraints();
        if (config_.integerTightening && hasIntegerVariable(environment_))
            ensureConstraints();
        return;
    }

    ensureConstraints();

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
    if (impl_->generatorsValid && impl_->generatorsExact)
        return generatorsEntail(impl_->generators, rows.front())
                   ? CheckResult::True
                   : CheckResult::Unknown;
    ensureConstraints();
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

    if (impl_->generatorsValid && impl_->generatorsExact)
    {
        std::vector<Rational> objective(environment_.size());
        objective[environment_.dimensionOf(variable)] = Rational(1);
        return boundFromGenerators(impl_->generators, objective, Rational());
    }
    ensureConstraints();

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

Interval ConvexPolyhedraState::bound(
    const LinearExpression& expression) const
{
    for (const auto& [variable, coefficient] : expression.terms())
    {
        (void)coefficient;
        if (!environment_.contains(variable))
            throw std::invalid_argument(
                "bounded expression uses an unknown variable");
    }
    if (impl_->bottom)
        return Interval(Bound::plusInfinity(), Bound::minusInfinity());
    if (expression.terms().empty())
        return Interval::singleton(expression.constant());

    if (impl_->generatorsValid && impl_->generatorsExact)
    {
        std::vector<Rational> objective(environment_.size());
        for (const auto& [variable, coefficient] : expression.terms())
            objective[environment_.dimensionOf(variable)] = coefficient;
        return boundFromGenerators(impl_->generators, objective,
                                   expression.constant());
    }
    ensureConstraints();

    const std::size_t dimensions = environment_.size();
    const std::size_t valueDimension = dimensions;
    std::vector<Inequality> extended = impl_->inequalities;
    for (Inequality& inequality : extended)
        inequality.coefficients.resize(dimensions + 1);

    Inequality equality;
    equality.coefficients.resize(dimensions + 1);
    equality.coefficients[valueDimension] = Rational(1);
    for (const auto& [variable, coefficient] : expression.terms())
        equality.coefficients[environment_.dimensionOf(variable)] =
            -coefficient;
    equality.bound = expression.constant();
    extended.push_back(equality);
    for (Rational& coefficient : equality.coefficients)
        coefficient = -coefficient;
    equality.bound = -equality.bound;
    extended.push_back(std::move(equality));

    std::vector<std::size_t> removed(dimensions);
    std::iota(removed.begin(), removed.end(), 0);
    const std::vector<Inequality> projected =
        project(std::move(extended), removed);
    Bound lower = Bound::minusInfinity();
    Bound upper = Bound::plusInfinity();
    for (const Inequality& inequality : projected)
    {
        const Rational coefficient =
            inequality.coefficients[valueDimension];
        if (coefficient.sign() > 0)
        {
            upper = Bound::min(
                upper, Bound::finite(inequality.bound / coefficient,
                                     inequality.strict));
        }
        else if (coefficient.sign() < 0)
        {
            const Bound candidate = Bound::finite(
                inequality.bound / coefficient, inequality.strict);
            if (lower.isMinusInfinity() || lower.value() < candidate.value() ||
                (lower.value() == candidate.value() && candidate.isStrict() &&
                 !lower.isStrict()))
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
    ensureConstraints();
    result.reserve(impl_->inequalities.size());
    for (const Inequality& inequality : impl_->inequalities)
        result.push_back(rowConstraint(environment_, inequality));
    return result;
}

void ConvexPolyhedraState::close()
{
    if (impl_->bottom)
        return;
    ensureConstraints();
    for (Inequality& inequality : impl_->inequalities)
        inequality.strict = false;
    normalize();
}

void ConvexPolyhedraState::canonicalize()
{
    normalize();
}

ConvexPolyhedraState ConvexPolyhedraState::join(
    const ConvexPolyhedraState& other) const
{
    requirePolyhedron(other);
    if (impl_->bottom)
        return other;
    if (other.impl_->bottom)
        return *this;

    // In homogeneous V form the closed convex hull is simply the cone
    // generated by the union.  For NNC operands ensureGenerators returns the
    // closure, matching the domain's pre-existing join contract.
    ensureGenerators();
    other.ensureGenerators();
    if (other.impl_->constraintsValid &&
        !hasStrictConstraint(other.impl_->inequalities) &&
        std::all_of(other.impl_->inequalities.begin(),
                    other.impl_->inequalities.end(),
                    [&](const Inequality& inequality)
                    { return generatorsEntail(impl_->generators, inequality); }))
        return other;
    if (impl_->constraintsValid &&
        !hasStrictConstraint(impl_->inequalities) &&
        std::all_of(impl_->inequalities.begin(), impl_->inequalities.end(),
                    [&](const Inequality& inequality)
                    {
                        return generatorsEntail(other.impl_->generators,
                                                inequality);
                    }))
        return *this;

    ConvexPolyhedraState result = top(environment_, config_);
    // The union already generates the exact hull. Eagerly solving one LP per
    // generator at every join makes a hull chain repeatedly minimize all old
    // vertices. Primitive normalization and exact deduplication are enough;
    // the polar H conversion performs one saturation-based minimization when
    // constraints are actually requested.
    result.impl_->generators =
        mergeGeneratorSets(impl_->generators, other.impl_->generators);
    result.impl_->constraintsValid = false;
    result.impl_->generatorsValid = true;
    result.impl_->generatorsExact = true;
    if (config_.integerTightening && hasIntegerVariable(environment_))
        result.ensureConstraints();
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
    const ConvexPolyhedraState& next, const WideningPolicy& policy) const
{
    requirePolyhedron(next);
    if (impl_->bottom)
        return next;
    if (next.impl_->bottom)
        return *this;
    ensureConstraints();
    next.ensureConstraints();
    ConvexPolyhedraState result = top(environment_, config_);
    for (const Inequality& inequality : impl_->inequalities)
    {
        if (::entails(next.impl_->inequalities, environment_.size(),
                      inequality))
            result.impl_->inequalities.push_back(inequality);
    }
    result.normalize();
    const auto retainApplicableThreshold =
        [&](const LinearConstraint& threshold)
        {
            if (entails(threshold) == CheckResult::True &&
                    next.entails(threshold) == CheckResult::True)
                result.assume(threshold);
        };
    for (const Rational& threshold : policy.thresholds)
    {
        for (const VariableDeclaration& declaration :
             environment_.variables())
        {
            retainApplicableThreshold(
                lessEqual(LinearExpression(declaration.variable),
                          LinearExpression(threshold)));
            retainApplicableThreshold(
                greaterEqual(LinearExpression(declaration.variable),
                             LinearExpression(threshold)));
        }
    }
    for (const LinearConstraint& threshold : policy.linearThresholds)
        retainApplicableThreshold(threshold);
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
        impl_->generators.clear();
        impl_->constraintsValid = true;
        impl_->generatorsValid = false;
        impl_->generatorsExact = false;
        return;
    }
    polyhedron.ensureConstraints();
    if (impl_->generatorsValid && impl_->generatorsExact &&
        !hasStrictConstraint(polyhedron.impl_->inequalities))
    {
        const std::vector<Inequality> noConstraints;
        impl_->generators = intersectGeneratorsWithConstraints(
            std::move(impl_->generators),
            impl_->constraintsValid ? impl_->inequalities : noConstraints,
            polyhedron.impl_->inequalities, environment_.size());
        if (impl_->generators.empty())
        {
            impl_->bottom = true;
            impl_->inequalities.clear();
            impl_->constraintsValid = true;
            impl_->generatorsValid = false;
            impl_->generatorsExact = false;
            return;
        }
        impl_->generatorsValid = true;
        impl_->generatorsExact = true;
        invalidateConstraints();
        if (config_.integerTightening && hasIntegerVariable(environment_))
            ensureConstraints();
        return;
    }

    ensureConstraints();
    if (polyhedron.impl_->generatorsValid &&
        polyhedron.impl_->generatorsExact &&
        !hasStrictConstraint(impl_->inequalities))
    {
        impl_->generators = intersectGeneratorsWithConstraints(
            polyhedron.impl_->generators, polyhedron.impl_->inequalities,
            impl_->inequalities, environment_.size());
        if (impl_->generators.empty())
        {
            impl_->bottom = true;
            impl_->inequalities.clear();
            impl_->constraintsValid = true;
            impl_->generatorsValid = false;
            impl_->generatorsExact = false;
            return;
        }
        impl_->generatorsValid = true;
        impl_->generatorsExact = true;
        invalidateConstraints();
        if (config_.integerTightening && hasIntegerVariable(environment_))
            ensureConstraints();
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
    ensureConstraints();
    return !impl_->bottom && impl_->inequalities.empty();
}

bool ConvexPolyhedraState::leqState(const AbstractState& other) const
{
    const ConvexPolyhedraState& polyhedron = requirePolyhedron(other);
    if (impl_->bottom)
        return true;
    if (polyhedron.impl_->bottom)
        return false;
    polyhedron.ensureConstraints();
    if (impl_->generatorsValid && impl_->generatorsExact)
    {
        return std::all_of(
            polyhedron.impl_->inequalities.begin(),
            polyhedron.impl_->inequalities.end(),
            [&](const Inequality& inequality)
            { return generatorsEntail(impl_->generators, inequality); });
    }
    ensureConstraints();
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
    ensureConstraints();
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

void ConvexPolyhedraState::ensureConstraints() const
{
    if (impl_->bottom || impl_->constraintsValid)
        return;
    if (!impl_->generatorsValid || !impl_->generatorsExact)
        throw std::logic_error(
            "polyhedron has no exact representation for H conversion");

    std::vector<Inequality> inequalities = constraintsFromGenerators(
        impl_->generators, environment_.size());
    bool bottom = false;
    inequalities = normalized(std::move(inequalities), bottom);
    bool tightened = false;
    if (!bottom && config_.integerTightening)
    {
        for (Inequality& inequality : inequalities)
        {
            const Inequality before = inequality;
            tightenIntegerRow(inequality, environment_);
            tightened = tightened || !sameInequality(before, inequality);
        }
        inequalities = normalized(std::move(inequalities), bottom);
    }
    if (!bottom)
        inequalities =
            canonicalizeAffineHull(std::move(inequalities), bottom);
    // A real-valued V cache contains a positive-homogeneous point by
    // construction, and the saturation DD conversion above already returns
    // its irredundant facets. Integer row tightening is the only step here
    // that can change that set or make it infeasible, so reserve the global
    // LP validation/minimization for that case.
    if (!bottom && tightened)
    {
        if (!feasible(inequalities, environment_.size()))
            bottom = true;
        else
            inequalities =
                irredundant(std::move(inequalities), environment_.size());
    }

    if (bottom)
    {
        // This should only be reachable for a malformed internal V cache.  Be
        // fail-closed instead of publishing an inconsistent pair of caches.
        auto* self = const_cast<ConvexPolyhedraState*>(this);
        self->impl_->bottom = true;
        self->impl_->inequalities.clear();
        self->impl_->generators.clear();
        self->impl_->constraintsValid = true;
        self->impl_->generatorsValid = false;
        self->impl_->generatorsExact = false;
        return;
    }
    impl_->inequalities = std::move(inequalities);
    impl_->constraintsValid = true;
    if (tightened)
    {
        impl_->generators.clear();
        impl_->generatorsValid = false;
        impl_->generatorsExact = false;
    }
}

void ConvexPolyhedraState::ensureGenerators() const
{
    if (impl_->bottom || impl_->generatorsValid)
        return;
    ensureConstraints();
    impl_->generators = generatorsFromConstraints(
        impl_->inequalities, environment_.size());
    if (impl_->generators.empty())
    {
        auto* self = const_cast<ConvexPolyhedraState*>(this);
        self->impl_->bottom = true;
        self->impl_->inequalities.clear();
        self->impl_->constraintsValid = true;
        self->impl_->generatorsValid = false;
        self->impl_->generatorsExact = false;
        return;
    }
    impl_->generatorsValid = true;
    impl_->generatorsExact = !hasStrictConstraint(impl_->inequalities);
}

void ConvexPolyhedraState::invalidateConstraints()
{
    if (!impl_->generatorsValid || !impl_->generatorsExact)
        throw std::logic_error(
            "cannot invalidate constraints without an exact V representation");
    impl_->inequalities.clear();
    impl_->constraintsValid = false;
}

void ConvexPolyhedraState::invalidateGenerators()
{
    impl_->generators.clear();
    impl_->generatorsValid = false;
    impl_->generatorsExact = false;
}

void ConvexPolyhedraState::normalize()
{
    ensureConstraints();
    invalidateGenerators();
    if (impl_->bottom)
    {
        impl_->inequalities.clear();
        impl_->constraintsValid = true;
        return;
    }
    impl_->inequalities =
        normalized(std::move(impl_->inequalities), impl_->bottom);
    if (!impl_->bottom && config_.integerTightening)
    {
        for (Inequality& inequality : impl_->inequalities)
            tightenIntegerRow(inequality, environment_);
        impl_->inequalities =
            normalized(std::move(impl_->inequalities), impl_->bottom);
    }
    if (!impl_->bottom)
        impl_->inequalities = canonicalizeAffineHull(
            std::move(impl_->inequalities), impl_->bottom);
    if (!impl_->bottom && !feasible(impl_->inequalities, environment_.size()))
    {
        impl_->bottom = true;
        impl_->inequalities.clear();
        impl_->constraintsValid = true;
        return;
    }
    if (!impl_->bottom)
        impl_->inequalities =
            irredundant(std::move(impl_->inequalities), environment_.size());
    impl_->constraintsValid = true;
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

Rational dot(const std::vector<Rational>& lhs,
             const std::vector<Rational>& rhs)
{
    Rational result;
    for (std::size_t index = 0; index < lhs.size(); ++index)
        result += lhs[index] * rhs[index];
    return result;
}

bool generatorsEntail(const std::vector<Generator>& generators,
                      const Inequality& inequality)
{
    for (const Generator& generator : generators)
    {
        Rational value = -inequality.bound * generator.coordinates.front();
        for (std::size_t dimension = 0;
             dimension < inequality.coefficients.size(); ++dimension)
        {
            value += inequality.coefficients[dimension] *
                     generator.coordinates[dimension + 1];
        }
        if (generator.coordinates.front().isZero())
        {
            if (value.sign() > 0)
                return false;
        }
        else if (value.sign() > 0 ||
                 (value.isZero() && inequality.strict))
            return false;
    }
    return true;
}

Interval boundFromGenerators(const std::vector<Generator>& generators,
                             const std::vector<Rational>& objective,
                             const Rational& constant)
{
    std::optional<Rational> minimum;
    std::optional<Rational> maximum;
    bool lowerUnbounded = false;
    bool upperUnbounded = false;
    for (const Generator& generator : generators)
    {
        Rational value = constant * generator.coordinates.front();
        for (std::size_t dimension = 0; dimension < objective.size();
             ++dimension)
        {
            value += objective[dimension] *
                     generator.coordinates[dimension + 1];
        }
        if (generator.coordinates.front().isZero())
        {
            lowerUnbounded = lowerUnbounded || value.sign() < 0;
            upperUnbounded = upperUnbounded || value.sign() > 0;
            continue;
        }
        value /= generator.coordinates.front();
        if (!minimum || value < *minimum)
            minimum = value;
        if (!maximum || *maximum < value)
            maximum = value;
    }
    if (!minimum || !maximum)
        return Interval(Bound::plusInfinity(), Bound::minusInfinity());
    return Interval(lowerUnbounded ? Bound::minusInfinity()
                                   : Bound::finite(*minimum),
                    upperUnbounded ? Bound::plusInfinity()
                                   : Bound::finite(*maximum));
}

bool normalizeGenerator(Generator& generator)
{
    if (std::all_of(generator.coordinates.begin(),
                    generator.coordinates.end(),
                    [](const Rational& coordinate)
                    { return coordinate.isZero(); }))
        return false;

    // NewPolka keeps primitive integer rows. Clearing all denominators and the
    // common numerator GCD here prevents repeated H/V conversion and affine
    // images from accumulating large rational numerators and denominators.
    mpz_class scale = 1;
    for (const Rational& coordinate : generator.coordinates)
    {
        if (coordinate.isZero())
            continue;
        const mpz_class denominator = coordinate.value().get_den();
        mpz_lcm(scale.get_mpz_t(), scale.get_mpz_t(),
                denominator.get_mpz_t());
    }
    mpz_class divisor = 0;
    for (const Rational& coordinate : generator.coordinates)
    {
        const mpq_class scaled = coordinate.value() * scale;
        const mpz_class integer = scaled.get_num();
        mpz_gcd(divisor.get_mpz_t(), divisor.get_mpz_t(),
                integer.get_mpz_t());
    }
    if (divisor < 0)
        divisor = -divisor;
    for (Rational& coordinate : generator.coordinates)
    {
        const mpq_class scaled = coordinate.value() * scale;
        const mpz_class integer = scaled.get_num() / divisor;
        coordinate = Rational::fromRaw(mpq_class(integer));
    }
    return true;
}

struct GeneratorLess
{
    bool operator()(const Generator& lhs, const Generator& rhs) const
    {
        return std::lexicographical_compare(
            lhs.coordinates.begin(), lhs.coordinates.end(),
            rhs.coordinates.begin(), rhs.coordinates.end());
    }
};

std::vector<Generator> uniqueGenerators(std::vector<Generator> generators)
{
    std::set<Generator, GeneratorLess> unique;
    for (Generator& generator : generators)
    {
        if (normalizeGenerator(generator))
            unique.insert(std::move(generator));
    }
    return std::vector<Generator>(unique.begin(), unique.end());
}

std::vector<Generator> mergeGeneratorSets(
    const std::vector<Generator>& lhs, const std::vector<Generator>& rhs)
{
    // Every authoritative V cache is primitive-normalized and sorted when it
    // is created. A hull can therefore union two caches linearly without
    // renormalizing every old GMP row at every step.
    std::vector<Generator> result;
    result.reserve(lhs.size() + rhs.size());
    std::set_union(lhs.begin(), lhs.end(), rhs.begin(), rhs.end(),
                   std::back_inserter(result), GeneratorLess{});
    return result;
}

bool generatedByCone(const Generator& target,
                     const std::vector<Generator>& generators)
{
    if (generators.empty())
        return false;

    const std::size_t variables = generators.size();
    std::vector<Inequality> system;
    system.reserve(variables + 2 * target.coordinates.size());
    for (std::size_t variable = 0; variable < variables; ++variable)
    {
        Inequality nonnegative;
        nonnegative.coefficients.resize(variables);
        nonnegative.coefficients[variable] = Rational(-1);
        system.push_back(std::move(nonnegative));
    }
    for (std::size_t coordinate = 0;
         coordinate < target.coordinates.size(); ++coordinate)
    {
        Inequality upper;
        upper.coefficients.resize(variables);
        for (std::size_t variable = 0; variable < variables; ++variable)
            upper.coefficients[variable] =
                generators[variable].coordinates[coordinate];
        upper.bound = target.coordinates[coordinate];
        system.push_back(upper);
        for (Rational& coefficient : upper.coefficients)
            coefficient = -coefficient;
        upper.bound = -upper.bound;
        system.push_back(std::move(upper));
    }
    return feasible(std::move(system), variables);
}

std::vector<Generator> irredundantGenerators(
    std::vector<Generator> generators)
{
    generators = uniqueGenerators(std::move(generators));
    if (generators.size() < 2)
        return generators;

    std::vector<Generator> kept;
    kept.reserve(generators.size());
    for (std::size_t index = 0; index < generators.size(); ++index)
    {
        std::vector<Generator> rest = kept;
        rest.insert(rest.end(), generators.begin() + index + 1,
                    generators.end());
        if (!generatedByCone(generators[index], rest))
            kept.push_back(std::move(generators[index]));
    }
    return kept;
}

constexpr std::size_t saturationWordBits = 64;

std::size_t saturationWords(std::size_t constraints)
{
    return (constraints + saturationWordBits - 1) / saturationWordBits;
}

void clearSaturation(Generator& generator, std::size_t constraints)
{
    generator.saturation.assign(saturationWords(constraints), 0);
}

void setSaturation(Generator& generator, std::size_t constraint)
{
    const std::size_t word = constraint / saturationWordBits;
    if (generator.saturation.size() <= word)
        generator.saturation.resize(word + 1);
    generator.saturation[word] |=
        std::uint64_t(1) << (constraint % saturationWordBits);
}

bool saturated(const Generator& generator, std::size_t constraint)
{
    const std::size_t word = constraint / saturationWordBits;
    return word < generator.saturation.size() &&
           (generator.saturation[word] &
            (std::uint64_t(1) << (constraint % saturationWordBits))) != 0;
}

std::size_t saturationCount(const Generator& generator,
                            std::size_t constraints)
{
    std::size_t result = 0;
    for (std::size_t constraint = 0; constraint < constraints; ++constraint)
        result += saturated(generator, constraint) ? 1 : 0;
    return result;
}

std::vector<std::uint64_t> commonSaturation(const Generator& lhs,
                                            const Generator& rhs)
{
    const std::size_t words =
        std::min(lhs.saturation.size(), rhs.saturation.size());
    std::vector<std::uint64_t> result(words);
    for (std::size_t word = 0; word < words; ++word)
        result[word] = lhs.saturation[word] & rhs.saturation[word];
    return result;
}

std::size_t saturationCount(const std::vector<std::uint64_t>& saturation)
{
    std::size_t result = 0;
    for (std::uint64_t word : saturation)
    {
#if defined(__clang__) || defined(__GNUC__)
        result += static_cast<std::size_t>(__builtin_popcountll(word));
#else
        while (word != 0)
        {
            word &= word - 1;
            ++result;
        }
#endif
    }
    return result;
}

bool saturationSubset(const std::vector<std::uint64_t>& subset,
                      const Generator& superset)
{
    for (std::size_t word = 0; word < subset.size(); ++word)
    {
        const std::uint64_t candidate =
            word < superset.saturation.size() ? superset.saturation[word] : 0;
        if ((subset[word] & ~candidate) != 0)
            return false;
    }
    return true;
}

void rebuildSaturation(
    std::vector<Generator>& generators,
    const std::vector<std::vector<Rational>>& processedConstraints)
{
    for (Generator& generator : generators)
    {
        clearSaturation(generator, processedConstraints.size());
        for (std::size_t constraint = 0;
             constraint < processedConstraints.size(); ++constraint)
        {
            if (dot(processedConstraints[constraint], generator.coordinates)
                    .isZero())
                setSaturation(generator, constraint);
        }
    }
}

std::optional<std::pair<std::size_t, std::size_t>> linePivot(
    const std::vector<Generator>& generators,
    const std::vector<Rational>* constraint, std::size_t processedConstraints)
{
    for (std::size_t first = 0; first < generators.size(); ++first)
    {
        if (saturationCount(generators[first], processedConstraints) !=
                processedConstraints ||
            (constraint != nullptr &&
             dot(*constraint, generators[first].coordinates).isZero()))
            continue;
        for (std::size_t second = first + 1; second < generators.size();
             ++second)
        {
            if (saturationCount(generators[second], processedConstraints) !=
                processedConstraints)
                continue;
            bool opposite = true;
            for (std::size_t coordinate = 0;
                 coordinate < generators[first].coordinates.size();
                 ++coordinate)
            {
                if (generators[first].coordinates[coordinate] !=
                    -generators[second].coordinates[coordinate])
                {
                    opposite = false;
                    break;
                }
            }
            if (opposite)
                return std::make_pair(first, second);
        }
    }
    return std::nullopt;
}

bool coneHasLineality(const std::vector<Generator>& generators,
                      std::size_t processedConstraints)
{
    return linePivot(generators, nullptr, processedConstraints).has_value();
}

bool adjacentGenerators(const std::vector<Generator>& generators,
                        std::size_t lhs, std::size_t rhs,
                        std::size_t processedConstraints,
                        std::size_t ambientDimensions)
{
    const std::vector<std::uint64_t> common =
        commonSaturation(generators[lhs], generators[rhs]);
    const std::size_t required = ambientDimensions > 2
                                     ? ambientDimensions - 2
                                     : 0;
    // The rank of common active constraints cannot exceed their count.  Fewer
    // than D-2 therefore cannot define a two-dimensional face.
    if (saturationCount(common) < required)
        return false;

    for (std::size_t other = 0; other < generators.size(); ++other)
    {
        if (other == lhs || other == rhs)
            continue;
        // Explicit opposite rays encode lineality. NewPolka excludes lines
        // from this extremal-ray dominance test.
        if (saturationCount(generators[other], processedConstraints) ==
            processedConstraints)
            continue;
        if (saturationSubset(common, generators[other]))
            return false;
    }
    return true;
}

struct ConstraintSplit
{
    std::size_t inside = 0;
    std::size_t outside = 0;
    std::size_t boundary = 0;

    std::size_t estimatedSize() const
    {
        if (outside == 0)
            return inside + boundary;
        if (inside == 0)
            return boundary;
        if (inside > std::numeric_limits<std::size_t>::max() / outside)
            return std::numeric_limits<std::size_t>::max();
        const std::size_t pairs = inside * outside;
        if (pairs > std::numeric_limits<std::size_t>::max() -
                        inside - boundary)
            return std::numeric_limits<std::size_t>::max();
        return inside + boundary + pairs;
    }
};

ConstraintSplit splitBy(const std::vector<Generator>& generators,
                        const std::vector<Rational>& constraint)
{
    ConstraintSplit result;
    for (const Generator& generator : generators)
    {
        const int sign = dot(constraint, generator.coordinates).sign();
        if (sign < 0)
            ++result.inside;
        else if (sign > 0)
            ++result.outside;
        else
            ++result.boundary;
    }
    return result;
}

/// Incremental Chernikova/double-description conversion for a homogeneous
/// cone.  Saturation sets make the expensive crossing-pair step adjacency
/// aware once lineality has disappeared.  Before that point an exact conic
/// redundancy pass retains the general non-pointed behavior.
std::vector<Generator> intersectCone(
    std::vector<Generator> generators,
    std::vector<std::vector<Rational>> processedConstraints,
    std::vector<std::vector<Rational>> constraints,
    bool inputIsMinimal = false)
{
    generators = inputIsMinimal
                     ? uniqueGenerators(std::move(generators))
                     : irredundantGenerators(std::move(generators));
    rebuildSaturation(generators, processedConstraints);

    while (!constraints.empty() && !generators.empty())
    {
        // Constraint order changes intermediate size dramatically. Select the
        // row with the smallest predicted next generator set.
        std::size_t selected = 0;
        bool selectedCutsLine =
            linePivot(generators, &constraints[0],
                      processedConstraints.size())
                .has_value();
        ConstraintSplit selectedSplit = splitBy(generators, constraints[0]);
        for (std::size_t candidate = 1; candidate < constraints.size();
             ++candidate)
        {
            const bool cutsLine =
                linePivot(generators, &constraints[candidate],
                          processedConstraints.size())
                    .has_value();
            const ConstraintSplit split =
                splitBy(generators, constraints[candidate]);
            if ((cutsLine && !selectedCutsLine) ||
                (cutsLine == selectedCutsLine &&
                 split.estimatedSize() < selectedSplit.estimatedSize()))
            {
                selected = candidate;
                selectedCutsLine = cutsLine;
                selectedSplit = split;
            }
        }
        std::vector<Rational> constraint =
            std::move(constraints[selected]);
        constraints.erase(constraints.begin() + selected);

        // Every generator satisfies the row, so it is redundant for the cone
        // and need not consume a saturation column.
        if (selectedSplit.outside == 0)
            continue;

        // A row cutting a lineality direction can be handled by a single
        // Chernikova line pivot. Move every other generator along that free
        // direction onto the new boundary, and retain only the feasible
        // orientation of the pivot as a ray. This removes one line dimension
        // without materializing all crossing pairs or solving redundancy LPs.
        const auto pivotPair = linePivot(
            generators, &constraint, processedConstraints.size());
        if (pivotPair)
        {
            std::size_t pivotIndex = pivotPair->first;
            std::size_t oppositeIndex = pivotPair->second;
            Rational pivotValue =
                dot(constraint, generators[pivotIndex].coordinates);
            if (pivotValue.sign() < 0)
            {
                std::swap(pivotIndex, oppositeIndex);
                pivotValue = -pivotValue;
            }
            const Generator& pivot = generators[pivotIndex];
            std::vector<Generator> next;
            next.reserve(generators.size() - 1);
            next.push_back(generators[oppositeIndex]);
            for (std::size_t index = 0; index < generators.size(); ++index)
            {
                if (index == pivotIndex || index == oppositeIndex)
                    continue;
                const Rational value =
                    dot(constraint, generators[index].coordinates);
                Generator boundary;
                boundary.coordinates.resize(
                    generators[index].coordinates.size());
                for (std::size_t coordinate = 0;
                     coordinate < boundary.coordinates.size(); ++coordinate)
                {
                    boundary.coordinates[coordinate] =
                        pivotValue *
                            generators[index].coordinates[coordinate] -
                        value * pivot.coordinates[coordinate];
                }
                next.push_back(std::move(boundary));
            }
            processedConstraints.push_back(std::move(constraint));
            generators = uniqueGenerators(std::move(next));
            rebuildSaturation(generators, processedConstraints);
            continue;
        }

        const std::size_t newConstraint = processedConstraints.size();
        const bool pointed =
            !coneHasLineality(generators, processedConstraints.size());
        std::vector<std::pair<std::size_t, Rational>> inside;
        std::vector<std::pair<std::size_t, Rational>> outside;
        std::vector<std::size_t> boundary;
        for (std::size_t index = 0; index < generators.size(); ++index)
        {
            Rational value = dot(constraint, generators[index].coordinates);
            if (value.sign() < 0)
                inside.emplace_back(index, std::move(value));
            else if (value.sign() > 0)
                outside.emplace_back(index, std::move(value));
            else
                boundary.push_back(index);
        }

        std::vector<Generator> next;
        const std::size_t estimated = selectedSplit.estimatedSize();
        if (estimated != std::numeric_limits<std::size_t>::max())
            next.reserve(estimated);
        for (std::size_t index : boundary)
        {
            Generator generator = generators[index];
            setSaturation(generator, newConstraint);
            next.push_back(std::move(generator));
        }
        for (const auto& [index, value] : inside)
        {
            (void)value;
            next.push_back(generators[index]);
        }
        for (const auto& [inIndex, inValue] : inside)
        {
            for (const auto& [outIndex, outValue] : outside)
            {
                if (pointed &&
                    !adjacentGenerators(generators, inIndex, outIndex,
                                        processedConstraints.size(),
                                        generators[inIndex].coordinates.size()))
                    continue;
                Generator intersection;
                intersection.coordinates.resize(
                    generators[inIndex].coordinates.size());
                for (std::size_t coordinate = 0;
                     coordinate < intersection.coordinates.size(); ++coordinate)
                {
                    intersection.coordinates[coordinate] =
                        outValue *
                            generators[inIndex].coordinates[coordinate] -
                        inValue *
                            generators[outIndex].coordinates[coordinate];
                }
                intersection.saturation = commonSaturation(
                    generators[inIndex], generators[outIndex]);
                setSaturation(intersection, newConstraint);
                next.push_back(std::move(intersection));
            }
        }
        processedConstraints.push_back(std::move(constraint));
        if (pointed)
            generators = uniqueGenerators(std::move(next));
        else
            generators = irredundantGenerators(std::move(next));
    }
    return generators;
}

std::vector<Generator> fullSpaceCone(std::size_t dimensions)
{
    std::vector<Generator> result;
    result.reserve(2 * dimensions);
    for (std::size_t dimension = 0; dimension < dimensions; ++dimension)
    {
        Generator positive;
        positive.coordinates.resize(dimensions);
        positive.coordinates[dimension] = Rational(1);
        result.push_back(positive);
        positive.coordinates[dimension] = Rational(-1);
        result.push_back(std::move(positive));
    }
    return result;
}

std::vector<Rational> homogeneousConstraint(const Inequality& inequality,
                                            std::size_t dimensions)
{
    std::vector<Rational> row(dimensions + 1);
    row[0] = -inequality.bound;
    std::copy(inequality.coefficients.begin(),
              inequality.coefficients.end(), row.begin() + 1);
    return row;
}

std::vector<std::vector<Rational>> homogeneousConstraints(
    const std::vector<Inequality>& inequalities, std::size_t dimensions)
{
    std::vector<std::vector<Rational>> result;
    result.reserve(inequalities.size());
    for (const Inequality& inequality : inequalities)
        result.push_back(homogeneousConstraint(inequality, dimensions));
    return result;
}

std::vector<Generator> intersectGeneratorsWithConstraints(
    std::vector<Generator> generators,
    const std::vector<Inequality>& existing,
    const std::vector<Inequality>& added, std::size_t dimensions)
{
    std::vector<std::vector<Rational>> processed;
    processed.reserve(existing.size() + 1);
    std::vector<Rational> nonnegativeHomogeneous(dimensions + 1);
    nonnegativeHomogeneous[0] = Rational(-1);
    processed.push_back(std::move(nonnegativeHomogeneous));
    std::vector<std::vector<Rational>> oldRows =
        homogeneousConstraints(existing, dimensions);
    processed.insert(processed.end(),
                     std::make_move_iterator(oldRows.begin()),
                     std::make_move_iterator(oldRows.end()));
    return intersectCone(std::move(generators), std::move(processed),
                         homogeneousConstraints(added, dimensions));
}

std::vector<Generator> generatorsFromConstraints(
    const std::vector<Inequality>& inequalities, std::size_t dimensions)
{
    const std::size_t homogeneousDimensions = dimensions + 1;
    // The homogenized top polyhedron is cone((1,0), (0,+/-e_i)).
    std::vector<Generator> initial;
    Generator origin;
    origin.coordinates.resize(homogeneousDimensions);
    origin.coordinates[0] = Rational(1);
    initial.push_back(std::move(origin));
    for (std::size_t dimension = 0; dimension < dimensions; ++dimension)
    {
        Generator ray;
        ray.coordinates.resize(homogeneousDimensions);
        ray.coordinates[dimension + 1] = Rational(1);
        initial.push_back(ray);
        ray.coordinates[dimension + 1] = Rational(-1);
        initial.push_back(std::move(ray));
    }

    std::vector<std::vector<Rational>> rows =
        homogeneousConstraints(inequalities, dimensions);
    std::vector<Rational> nonnegativeHomogeneous(homogeneousDimensions);
    nonnegativeHomogeneous[0] = Rational(-1);
    std::vector<Generator> result = intersectCone(
        std::move(initial), {std::move(nonnegativeHomogeneous)},
        std::move(rows), true);

    // A non-empty polyhedron must have a generator with positive homogeneous
    // coordinate.  A cone containing only recession directions is the
    // homogenization of the empty affine slice.
    const bool hasPoint = std::any_of(
        result.begin(), result.end(), [](const Generator& generator)
        { return generator.coordinates.front().sign() > 0; });
    if (!hasPoint)
        return {};
    return result;
}

std::vector<Inequality> constraintsFromGenerators(
    const std::vector<Generator>& generators, std::size_t dimensions)
{
    const std::size_t homogeneousDimensions = dimensions + 1;
    std::vector<std::vector<Rational>> generatorHalfspaces;
    generatorHalfspaces.reserve(generators.size());
    for (const Generator& generator : generators)
        generatorHalfspaces.push_back(generator.coordinates);

    // The valid homogeneous linear forms are the polar cone of the generator
    // cone.  Applying the same double-description kernel to that polar gives
    // every facet/equality row of the original polyhedron.
    std::vector<Generator> polar = intersectCone(
        fullSpaceCone(homogeneousDimensions), {},
        std::move(generatorHalfspaces), true);
    std::vector<Inequality> result;
    result.reserve(polar.size());
    for (const Generator& form : polar)
    {
        Inequality inequality;
        inequality.coefficients.assign(form.coordinates.begin() + 1,
                                       form.coordinates.end());
        inequality.bound = -form.coordinates.front();
        result.push_back(std::move(inequality));
    }
    bool bottom = false;
    result = normalized(std::move(result), bottom);
    if (bottom)
        return {};
    // The pointed phase of the DD kernel emits only adjacent extreme forms;
    // after normalization these are already the facets/equality directions.
    // Running one LP per row here repeats the minimization just performed by
    // the saturation matrix and dominates V -> H conversion in dimensions
    // where a hull has many facets.
    return result;
}

} // namespace
