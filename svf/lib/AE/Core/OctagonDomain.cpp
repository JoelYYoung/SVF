//===- OctagonDomain.cpp -- Exact GMP octagon relational backend --------===//

#include "AE/Core/OctagonDomain.h"

#include <algorithm>
#include <map>
#include <vector>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <utility>

using namespace SVF::AbstractDomain;

namespace
{

std::size_t positiveNode(Dimension dimension)
{
    return 2 * dimension;
}

std::size_t negativeNode(Dimension dimension)
{
    return 2 * dimension + 1;
}

std::size_t opposite(std::size_t node)
{
    return node ^ 1U;
}

class OctagonStorage final
{
public:
    struct ScratchMatrixTag
    {
    };

    explicit OctagonStorage(const VariableEnvironment& environment, bool bottom = false)
        : OctagonStorage(extractVariableKinds(environment), bottom)
    {
    }

    explicit OctagonStorage(std::vector<NumericKind> variableKinds,
                          bool bottom = false)
        : dimensions(variableKinds.size()),
          variableKinds(std::move(variableKinds)),
          matrix(matrixSize(dimensions)),
          bottom(bottom)
    {
        const std::size_t count = nodes();
        for (std::size_t node = 0; node < count; ++node)
            at(node, node) = Bound::finite(Rational());
    }

    OctagonStorage(std::vector<NumericKind> variableKinds, ScratchMatrixTag)
        : dimensions(variableKinds.size()),
          variableKinds(std::move(variableKinds)),
          matrix(matrixSize(dimensions))
    {
    }

    std::unique_ptr<OctagonStorage> clone() const
    {
        return std::make_unique<OctagonStorage>(*this);
    }

    std::size_t nodes() const
    {
        return 2 * dimensions;
    }

    Bound& at(std::size_t row, std::size_t column)
    {
        return matrix[matrixIndex(row, column)];
    }

    const Bound& at(std::size_t row, std::size_t column) const
    {
        return matrix[matrixIndex(row, column)];
    }

    std::size_t dimensions;
    std::vector<NumericKind> variableKinds;
    std::vector<Bound> matrix;
    bool bottom = false;
    bool stronglyClosed = true;

private:
    /// APRON's hmat layout stores triangular 2x2 variable blocks. Coherent
    /// off-diagonal entries share an index. Each diagonal block is fully
    /// stored, so its two main-diagonal entries remain a distinct coherent
    /// pair that normalize() keeps equal. at(row,column) preserves the logical
    /// 2n x 2n matrix interface for all algorithms.
    static std::size_t matrixSize(std::size_t dimensions)
    {
        return 2 * dimensions * (dimensions + 1);
    }

    static std::size_t storedIndex(std::size_t row, std::size_t column)
    {
        return column + ((row + 1) * (row + 1)) / 2;
    }

    static std::size_t matrixIndex(std::size_t row, std::size_t column)
    {
        return column > row ? storedIndex(opposite(column), opposite(row))
                            : storedIndex(row, column);
    }

    static std::vector<NumericKind> extractVariableKinds(
        const VariableEnvironment& environment)
    {
        std::vector<NumericKind> result;
        result.reserve(environment.size());
        for (const VariableDeclaration& declaration : environment.variables())
            result.push_back(declaration.type.kind);
        return result;
    }
};

const OctagonStorage& asOctagon(const OctagonStorage& state)
{
    return static_cast<const OctagonStorage&>(state);
}

OctagonStorage& asOctagon(OctagonStorage& state)
{
    return static_cast<OctagonStorage&>(state);
}

bool isNegativeDiagonal(const Bound& diagonal)
{
    if (diagonal.isMinusInfinity())
        return true;
    if (!diagonal.isFinite())
        return false;
    return diagonal.value().sign() < 0 ||
           (diagonal.value().isZero() && diagonal.isStrict());
}

Rational absolute(const Rational& value)
{
    return value.sign() < 0 ? -value : value;
}

/// Greatest even integer satisfying z <= bound (or z < bound for a strict
/// bound).  Unary integer octagon entries constrain 2*x and must be even.
Bound tightenIntegerUnary(const Bound& bound)
{
    if (!bound.isFinite())
        return bound;
    Rational greatest = bound.isStrict() ? bound.value().ceil() - Rational(1)
                                         : bound.value().floor();
    Rational even = (greatest / Rational(2)).floor() * Rational(2);
    return Bound::finite(std::move(even));
}

} // namespace

class SVF::AbstractDomain::OctagonState::Impl final
{
public:
    Impl(const VariableEnvironment& environment, OctagonConfig options, bool bottom)
        : options_(std::move(options)), state_(environment, bottom)
    {
    }

    Impl(OctagonConfig options, OctagonStorage state)
        : options_(std::move(options)), state_(std::move(state))
    {
    }

    Impl(const Impl&) = default;

    const char* name() const
    {
        return "gmp-octagon";
    }

    DomainCapabilities capabilities() const
    {
        DomainCapabilities result;
        result.strictInequalities = true;
        result.integerTightening = options_.integerTightening;
        result.thresholdWidening = true;
        result.narrowing = true;
        result.parallelAssignments = true;
        result.nonlinearTreeExpressions = false;
        return result;
    }

    ApproximationKind assign(OctagonStorage& genericState,
                             const VariableEnvironment& environment, Variable target,
                             const LinearExpression& expression) const
    {
        OctagonStorage& state = asOctagon(genericState);
        requireVariables(environment, expression);
        if (!environment.contains(target))
            throw std::invalid_argument(
                "assignment target is not in relational environment");
        if (environment.typeOf(target).kind == NumericKind::IEEEFloat)
        {
            forget(state, environment.dimensionOf(target));
            return ApproximationKind::UnsupportedFallback;
        }
        if (state.bottom)
            return ApproximationKind::Exact;

        const Dimension targetDimension = environment.dimensionOf(target);
        if (expression.terms().empty())
        {
            forget(state, targetDimension);
            LinearExpression equality(target);
            equality -= expression;
            addConstraint(
                state, environment,
                LinearConstraint(std::move(equality), ConstraintKind::Equal));
            normalize(state, environment);
            return ApproximationKind::Exact;
        }

        if (expression.terms().size() == 1)
        {
            const auto& [source, coefficient] = *expression.terms().begin();
            if ((coefficient == Rational(1) || coefficient == Rational(-1)) &&
                environment.typeOf(source).kind != NumericKind::IEEEFloat)
            {
                const Dimension sourceDimension =
                    environment.dimensionOf(source);
                if (sourceDimension == targetDimension)
                {
                    selfAssign(state, environment, targetDimension,
                               coefficient.sign(), expression.constant());
                }
                else
                {
                    forget(state, targetDimension);
                    LinearExpression equality(target);
                    equality -= expression;
                    addConstraint(state, environment,
                                  LinearConstraint(std::move(equality),
                                                   ConstraintKind::Equal));
                    normalize(state, environment);
                }
                return ApproximationKind::Exact;
            }
        }

        // Strong-update soundness requires killing every old relation of the
        // target first. What follows is the recovery, which used to be absent:
        // the target was simply forgotten.
        //
        // The right-hand side has to be measured before the update, because
        // the update destroys what it is measured against. That matters even
        // when the recovery is the equality below: if the expression reads the
        // target, as in `x := 2*x + y`, then asserting `target = expression`
        // after the strong update is not a weaker fact, it is a false one --
        // the `target` in the expression is the old value and is already gone.
        const Interval assigned =
            evaluateInterval(state, environment, expression);
        const bool readsTarget = !expression.coefficient(target).isZero();

        forget(state, targetDimension);
        if (!readsTarget)
        {
            LinearExpression equality(target);
            equality -= expression;
            const bool exact = addConstraint(
                state, environment,
                LinearConstraint(std::move(equality), ConstraintKind::Equal));
            normalize(state, environment);
            return exact ? ApproximationKind::Exact
                         : ApproximationKind::SoundOverApproximation;
        }

        if (assigned.upper().isFinite())
        {
            LinearExpression upper(target);
            upper.setConstant(-assigned.upper().value());
            addLessEqual(state, environment, upper, assigned.upper().isStrict(),
                         false);
        }
        if (assigned.lower().isFinite())
        {
            LinearExpression lower;
            lower.setCoefficient(target, Rational(-1));
            lower.setConstant(assigned.lower().value());
            addLessEqual(state, environment, lower, assigned.lower().isStrict(),
                         false);
        }
        normalize(state, environment);
        return ApproximationKind::SoundOverApproximation;
    }

    ApproximationKind assume(OctagonStorage& genericState,
                             const VariableEnvironment& environment,
                             const LinearConstraint& constraint) const
    {
        OctagonStorage& state = asOctagon(genericState);
        requireVariables(environment, constraint.expression());
        for (const auto& [variable, coefficient] :
             constraint.expression().terms())
        {
            (void)coefficient;
            if (environment.typeOf(variable).kind == NumericKind::IEEEFloat)
                return ApproximationKind::UnsupportedFallback;
        }

        const bool exact = addConstraint(state, environment, constraint);
        normalize(state, environment);
        return exact ? ApproximationKind::Exact
                     : ApproximationKind::SoundOverApproximation;
    }

    void forget(OctagonStorage& genericState, const VariableEnvironment& environment,
                Variable variable) const
    {
        if (!environment.contains(variable))
            throw std::invalid_argument(
                "forgotten variable is not in relational environment");
        forget(asOctagon(genericState), environment.dimensionOf(variable));
    }

    std::unique_ptr<OctagonStorage> join(const OctagonStorage& genericLhs,
                                      const OctagonStorage& genericRhs) const
    {
        std::optional<OctagonStorage> lhsStorage;
        std::optional<OctagonStorage> rhsStorage;
        const OctagonStorage& lhs =
            normalized(asOctagon(genericLhs), lhsStorage);
        const OctagonStorage& rhs =
            normalized(asOctagon(genericRhs), rhsStorage);
        requireSameSize(lhs, rhs);
        if (lhs.bottom)
            return rhs.clone();
        if (rhs.bottom)
            return lhs.clone();

        auto result = std::make_unique<OctagonStorage>(
            lhs.variableKinds, OctagonStorage::ScratchMatrixTag{});
        for (std::size_t index = 0; index < result->matrix.size(); ++index)
        {
            const Bound& left = lhs.matrix[index];
            const Bound& right = rhs.matrix[index];
            result->matrix[index] = left <= right ? right : left;
        }

        // Point-wise maximum preserves coherence and every closure inequality.
        // If R[i,j] comes from operand K, then
        // K[i,j] <= K[i,h] + K[h,j] <= R[i,h] + R[h,j]; the same monotonicity
        // argument applies to the strong-closure inequality. Integer-tight
        // unary entries remain even because the maximum of two even bounds is
        // even. APRON's oct_join uses this same closed-result fast path instead
        // of scheduling another cubic close.
        result->stronglyClosed = true;
        return result;
    }

    std::unique_ptr<OctagonStorage> meet(const OctagonStorage& genericLhs,
                                      const OctagonStorage& genericRhs) const
    {
        std::optional<OctagonStorage> lhsStorage;
        std::optional<OctagonStorage> rhsStorage;
        const OctagonStorage& lhs =
            normalized(asOctagon(genericLhs), lhsStorage);
        const OctagonStorage& rhs =
            normalized(asOctagon(genericRhs), rhsStorage);
        requireSameSize(lhs, rhs);
        if (lhs.bottom)
            return lhs.clone();
        if (rhs.bottom)
            return rhs.clone();

        auto result = std::make_unique<OctagonStorage>(
            lhs.variableKinds, OctagonStorage::ScratchMatrixTag{});
        for (std::size_t index = 0; index < result->matrix.size(); ++index)
        {
            const Bound& left = lhs.matrix[index];
            const Bound& right = rhs.matrix[index];
            result->matrix[index] = left <= right ? left : right;
        }
        result->stronglyClosed = false;
        normalize(*result);
        return result;
    }

    std::unique_ptr<OctagonStorage> widen(const OctagonStorage& genericCurrent,
                                       const OctagonStorage& genericNext,
                                       const WideningPolicy& policy) const
    {
        const OctagonStorage& current = asOctagon(genericCurrent);
        std::optional<OctagonStorage> nextStorage;
        const OctagonStorage& next =
            normalized(asOctagon(genericNext), nextStorage);
        requireSameSize(current, next);
        if (current.bottom)
            return next.clone();
        if (next.bottom)
            return current.clone();

        std::vector<Rational> thresholds = policy.thresholds;
        std::sort(thresholds.begin(), thresholds.end());

        auto result = std::make_unique<OctagonStorage>(
            current.variableKinds, OctagonStorage::ScratchMatrixTag{});
        for (std::size_t row = 0; row < result->nodes(); ++row)
        {
            for (std::size_t column = 0; column < result->nodes(); ++column)
            {
                const Bound& oldBound = current.at(row, column);
                const Bound& nextBound = next.at(row, column);
                if (nextBound <= oldBound)
                {
                    result->at(row, column) = oldBound;
                    continue;
                }

                result->at(row, column) = Bound::plusInfinity();
                if (nextBound.isFinite())
                {
                    // Public thresholds use normalized octagonal constants.
                    // A unary DBM entry encodes +/-2*x, so its matrix bound
                    // needs twice the user-visible threshold.
                    const bool unary = row != column && row / 2 == column / 2;
                    for (const Rational& threshold : thresholds)
                    {
                        Bound candidate = Bound::finite(
                            unary ? threshold * Rational(2) : threshold);
                        if (nextBound <= candidate)
                        {
                            result->at(row, column) = std::move(candidate);
                            break;
                        }
                    }
                }
            }
        }
        for (std::size_t node = 0; node < result->nodes(); ++node)
            result->at(node, node) = Bound::finite(Rational());
        result->stronglyClosed = false;
        return result;
    }

    std::unique_ptr<OctagonStorage> narrow(const OctagonStorage& genericCurrent,
                                        const OctagonStorage& genericNext) const
    {
        std::optional<OctagonStorage> currentStorage;
        std::optional<OctagonStorage> nextStorage;
        const OctagonStorage& current =
            normalized(asOctagon(genericCurrent), currentStorage);
        const OctagonStorage& next =
            normalized(asOctagon(genericNext), nextStorage);
        requireSameSize(current, next);
        if (current.bottom || next.bottom)
            return next.clone();

        auto result = std::make_unique<OctagonStorage>(current);
        for (std::size_t index = 0; index < result->matrix.size(); ++index)
        {
            if (current.matrix[index].isPlusInfinity() &&
                next.matrix[index].isFinite())
                result->matrix[index] = next.matrix[index];
        }
        result->stronglyClosed = false;
        return result;
    }

    std::unique_ptr<OctagonStorage> projectLowerBounds(
        const OctagonStorage& genericState) const
    {
        std::optional<OctagonStorage> sourceStorage;
        const OctagonStorage& source =
            normalized(asOctagon(genericState), sourceStorage);
        if (source.bottom)
            return source.clone();

        auto result = std::make_unique<OctagonStorage>(source.variableKinds);
        for (Dimension dimension = 0; dimension < source.dimensions;
             ++dimension)
        {
            setCoherent(
                *result, negativeNode(dimension), positiveNode(dimension),
                source.at(negativeNode(dimension), positiveNode(dimension)));
        }
        for (Dimension lhs = 0; lhs < source.dimensions; ++lhs)
        {
            for (Dimension rhs = lhs + 1; rhs < source.dimensions; ++rhs)
            {
                // -(x+y) and -(x-y) are the selected lower orientations.
                setCoherent(*result, negativeNode(lhs), positiveNode(rhs),
                            source.at(negativeNode(lhs), positiveNode(rhs)));
                setCoherent(*result, negativeNode(lhs), negativeNode(rhs),
                            source.at(negativeNode(lhs), negativeNode(rhs)));
            }
        }
        result->stronglyClosed = false;
        normalize(*result);
        return result;
    }

    std::unique_ptr<OctagonStorage> changeEnvironment(
        const OctagonStorage& genericState, const VariableEnvironment& oldEnvironment,
        const VariableEnvironment& newEnvironment, bool initializeNewVariablesToZero) const
    {
        std::optional<OctagonStorage> sourceStorage;
        const OctagonStorage& source =
            normalized(asOctagon(genericState), sourceStorage);
        if (source.dimensions != oldEnvironment.size())
            throw std::invalid_argument(
                "old environment does not match octagon dimensions");
        if (source.bottom)
            return std::make_unique<OctagonStorage>(newEnvironment, true);

        auto result = std::make_unique<OctagonStorage>(newEnvironment);
        for (Dimension newRowDimension = 0;
             newRowDimension < newEnvironment.size(); ++newRowDimension)
        {
            const Variable rowVariable =
                newEnvironment.variableOf(newRowDimension);
            if (!oldEnvironment.contains(rowVariable))
                continue;
            if (oldEnvironment.typeOf(rowVariable) !=
                newEnvironment.typeOf(rowVariable))
                throw std::invalid_argument(
                    "environment change cannot alter a variable's type");
            const Dimension oldRowDimension =
                oldEnvironment.dimensionOf(rowVariable);

            for (Dimension newColumnDimension = 0;
                 newColumnDimension < newEnvironment.size();
                 ++newColumnDimension)
            {
                const Variable columnVariable =
                    newEnvironment.variableOf(newColumnDimension);
                if (!oldEnvironment.contains(columnVariable))
                    continue;
                if (oldEnvironment.typeOf(columnVariable) !=
                    newEnvironment.typeOf(columnVariable))
                    throw std::invalid_argument(
                        "environment change cannot alter a variable's type");
                const Dimension oldColumnDimension =
                    oldEnvironment.dimensionOf(columnVariable);

                for (std::size_t rowSign = 0; rowSign < 2; ++rowSign)
                {
                    for (std::size_t columnSign = 0; columnSign < 2;
                         ++columnSign)
                    {
                        result->at(2 * newRowDimension + rowSign,
                                   2 * newColumnDimension + columnSign) =
                            source.at(2 * oldRowDimension + rowSign,
                                      2 * oldColumnDimension + columnSign);
                    }
                }
            }
        }

        if (initializeNewVariablesToZero)
        {
            for (Dimension dimension = 0; dimension < newEnvironment.size();
                 ++dimension)
            {
                const Variable value = newEnvironment.variableOf(dimension);
                if (oldEnvironment.contains(value))
                    continue;
                setCoherent(*result, positiveNode(dimension),
                            negativeNode(dimension), Bound::finite(Rational()));
                setCoherent(*result, negativeNode(dimension),
                            positiveNode(dimension), Bound::finite(Rational()));
            }
        }
        result->stronglyClosed = false;
        normalize(*result);
        return result;
    }

    bool isBottom(const OctagonStorage& genericState) const
    {
        std::optional<OctagonStorage> storage;
        return normalized(asOctagon(genericState), storage).bottom;
    }

    bool isTop(const OctagonStorage& genericState) const
    {
        std::optional<OctagonStorage> storage;
        const OctagonStorage& state =
            normalized(asOctagon(genericState), storage);
        if (state.bottom)
            return false;
        for (std::size_t row = 0; row < state.nodes(); ++row)
        {
            for (std::size_t column = 0; column < state.nodes(); ++column)
            {
                if (row == column)
                    continue;
                if (!state.at(row, column).isPlusInfinity())
                    return false;
            }
        }
        return true;
    }

    bool leq(const OctagonStorage& genericLhs, const OctagonStorage& genericRhs) const
    {
        std::optional<OctagonStorage> lhsStorage;
        std::optional<OctagonStorage> rhsStorage;
        const OctagonStorage& lhs =
            normalized(asOctagon(genericLhs), lhsStorage);
        const OctagonStorage& rhs =
            normalized(asOctagon(genericRhs), rhsStorage);
        requireSameSize(lhs, rhs);
        if (lhs.bottom)
            return true;
        if (rhs.bottom)
            return false;
        for (std::size_t index = 0; index < lhs.matrix.size(); ++index)
        {
            if (!(lhs.matrix[index] <= rhs.matrix[index]))
                return false;
        }
        return true;
    }

    Interval bound(const OctagonStorage& genericState,
                   const VariableEnvironment& environment, Variable variable) const
    {
        if (!environment.contains(variable))
            throw std::invalid_argument(
                "bounded variable is not in relational environment");
        std::optional<OctagonStorage> storage;
        const OctagonStorage& state =
            normalized(asOctagon(genericState), storage);
        if (state.bottom)
            return Interval(Bound::plusInfinity(), Bound::minusInfinity());

        const Dimension dimension = environment.dimensionOf(variable);
        const Bound& doubledUpper =
            state.at(positiveNode(dimension), negativeNode(dimension));
        const Bound& doubledNegativeLower =
            state.at(negativeNode(dimension), positiveNode(dimension));

        Bound lower = Bound::minusInfinity();
        Bound upper = Bound::plusInfinity();
        if (doubledUpper.isFinite())
            upper = Bound::divideByPositive(doubledUpper, Rational(2));
        if (doubledNegativeLower.isFinite())
        {
            lower = Bound::finite(-(doubledNegativeLower.value() / Rational(2)),
                                  doubledNegativeLower.isStrict());
        }
        return Interval(std::move(lower), std::move(upper));
    }

    LinearConstraintSet constraints(const OctagonStorage& genericState,
                                    const VariableEnvironment& environment) const
    {
        std::optional<OctagonStorage> storage;
        const OctagonStorage& state =
            normalized(asOctagon(genericState), storage);
        LinearConstraintSet result;
        if (state.bottom)
            return result;

        for (std::size_t row = 0; row < state.nodes(); ++row)
        {
            for (std::size_t column = 0; column < state.nodes(); ++column)
            {
                if (row == column || !state.at(row, column).isFinite())
                    continue;

                // Coherent entries describe the same octagonal constraint.
                const std::size_t coherentRow = opposite(column);
                const std::size_t coherentColumn = opposite(row);
                if (std::pair<std::size_t, std::size_t>(coherentRow,
                                                        coherentColumn) <
                    std::pair<std::size_t, std::size_t>(row, column))
                    continue;

                LinearExpression expression;
                addNodeTerm(expression, environment, row, Rational(1));
                addNodeTerm(expression, environment, column, Rational(-1));
                expression.setConstant(-state.at(row, column).value());
                result.emplace_back(std::move(expression),
                                    state.at(row, column).isStrict()
                                        ? ConstraintKind::LessThan
                                        : ConstraintKind::LessEqual);
            }
        }
        return result;
    }

    std::string toString(const OctagonStorage& genericState,
                         const VariableEnvironment& environment) const
    {
        if (isBottom(genericState))
            return "bottom";
        const LinearConstraintSet exported =
            constraints(genericState, environment);
        if (exported.empty())
            return "top";
        std::ostringstream output;
        for (std::size_t index = 0; index < exported.size(); ++index)
        {
            if (index != 0)
                output << " && ";
            output << exported[index].toString(&environment);
        }
        return output.str();
    }

    const OctagonConfig& config() const
    {
        return options_;
    }

    void reconfigure(OctagonConfig options)
    {
        options_ = std::move(options);
        // A matrix marked closed under a weaker policy must be normalized
        // again when stronger integer/strong closure is enabled.
        if (!state_.bottom)
        {
            state_.stronglyClosed = false;
            normalize(state_);
        }
    }

    ApproximationKind assignCurrent(const VariableEnvironment& environment,
                                    Variable target,
                                    const LinearExpression& expression)
    {
        return assign(state_, environment, target, expression);
    }

    ApproximationKind assumeCurrent(const VariableEnvironment& environment,
                                    const LinearConstraint& constraint)
    {
        return assume(state_, environment, constraint);
    }

    void forgetCurrent(const VariableEnvironment& environment, Variable variable)
    {
        forget(state_, environment, variable);
    }

    void joinCurrent(const Impl& other)
    {
        state_ = std::move(*join(state_, other.state_));
    }

    std::unique_ptr<Impl> joined(const Impl& other) const
    {
        return std::make_unique<Impl>(
            options_, std::move(*join(state_, other.state_)));
    }

    void meetCurrent(const Impl& other)
    {
        state_ = std::move(*meet(state_, other.state_));
    }

    std::unique_ptr<Impl> met(const Impl& other) const
    {
        return std::make_unique<Impl>(
            options_, std::move(*meet(state_, other.state_)));
    }

    void widenCurrent(const Impl& other, const WideningPolicy& policy)
    {
        state_ = std::move(*widen(state_, other.state_, policy));
    }

    std::unique_ptr<Impl> widened(
        const Impl& other, const WideningPolicy& policy) const
    {
        return std::make_unique<Impl>(
            options_, std::move(*widen(state_, other.state_, policy)));
    }

    void narrowCurrent(const Impl& other)
    {
        state_ = std::move(*narrow(state_, other.state_));
    }

    std::unique_ptr<Impl> narrowed(const Impl& other) const
    {
        return std::make_unique<Impl>(
            options_, std::move(*narrow(state_, other.state_)));
    }

    void projectLowerBoundsCurrent()
    {
        state_ = std::move(*projectLowerBounds(state_));
    }

    std::unique_ptr<Impl> projectedLowerBounds() const
    {
        return std::make_unique<Impl>(
            options_, std::move(*projectLowerBounds(state_)));
    }

    void changeEnvironmentCurrent(const VariableEnvironment& oldEnvironment,
                                  const VariableEnvironment& newEnvironment,
                                  bool initializeNewVariablesToZero)
    {
        state_ = std::move(*changeEnvironment(
            state_, oldEnvironment, newEnvironment, initializeNewVariablesToZero));
    }

    std::unique_ptr<Impl> changedEnvironment(
        const VariableEnvironment& oldEnvironment, const VariableEnvironment& newEnvironment,
        bool initializeNewVariablesToZero) const
    {
        return std::make_unique<Impl>(
            options_, std::move(*changeEnvironment(
                          state_, oldEnvironment, newEnvironment,
                          initializeNewVariablesToZero)));
    }

    bool isBottomCurrent() const
    {
        return isBottom(state_);
    }

    bool isTopCurrent() const
    {
        return isTop(state_);
    }

    bool leqCurrent(const Impl& other) const
    {
        return leq(state_, other.state_);
    }

    Interval boundCurrent(const VariableEnvironment& environment,
                          Variable variable) const
    {
        return bound(state_, environment, variable);
    }

    LinearConstraintSet constraintsCurrent(
        const VariableEnvironment& environment) const
    {
        return constraints(state_, environment);
    }

    std::string toStringCurrent(const VariableEnvironment& environment) const
    {
        return toString(state_, environment);
    }

private:
    void requireSameSize(const OctagonStorage& lhs, const OctagonStorage& rhs) const
    {
        if (lhs.dimensions != rhs.dimensions ||
            lhs.variableKinds != rhs.variableKinds)
            throw std::invalid_argument("octagon state shapes do not match");
    }

    void requireVariables(const VariableEnvironment& environment,
                          const LinearExpression& expression) const
    {
        for (const auto& [variable, coefficient] : expression.terms())
        {
            (void)coefficient;
            if (!environment.contains(variable))
                throw std::invalid_argument(
                    "expression variable is not in relational environment");
        }
    }

    void setCoherent(OctagonStorage& state, std::size_t row, std::size_t column,
                     const Bound& bound) const
    {
        state.at(row, column) = Bound::min(state.at(row, column), bound);
        const std::size_t coherentRow = opposite(column);
        const std::size_t coherentColumn = opposite(row);
        state.at(coherentRow, coherentColumn) =
            Bound::min(state.at(coherentRow, coherentColumn), bound);
        state.stronglyClosed = false;
    }

    void forget(OctagonStorage& state, Dimension dimension) const
    {
        if (dimension >= state.dimensions)
            throw std::out_of_range("invalid forgotten octagon dimension");
        if (state.bottom)
            return;
        normalize(state);
        const std::size_t first = positiveNode(dimension);
        const std::size_t second = negativeNode(dimension);
        for (std::size_t node = 0; node < state.nodes(); ++node)
        {
            state.at(first, node) = Bound::plusInfinity();
            state.at(second, node) = Bound::plusInfinity();
            state.at(node, first) = Bound::plusInfinity();
            state.at(node, second) = Bound::plusInfinity();
        }
        state.at(first, first) = Bound::finite(Rational());
        state.at(second, second) = Bound::finite(Rational());
        state.stronglyClosed = true;
    }

    bool addConstraint(OctagonStorage& state, const VariableEnvironment& environment,
                       const LinearConstraint& constraint) const
    {
        if (state.bottom)
            return true;

        switch (constraint.kind())
        {
        case ConstraintKind::Equal: {
            const bool forward = addLessEqual(state, environment,
                                              constraint.expression(), false);
            const bool backward = addLessEqual(state, environment,
                                               -constraint.expression(), false);
            return forward && backward;
        }
        case ConstraintKind::NotEqual:
            return handleConstantNotEqual(state, constraint.expression());
        case ConstraintKind::LessThan:
            return addLessEqual(state, environment, constraint.expression(),
                                true);
        case ConstraintKind::LessEqual:
            return addLessEqual(state, environment, constraint.expression(),
                                false);
        case ConstraintKind::GreaterThan:
            return addLessEqual(state, environment, -constraint.expression(),
                                true);
        case ConstraintKind::GreaterEqual:
            return addLessEqual(state, environment, -constraint.expression(),
                                false);
        }
        return false;
    }

    bool handleConstantNotEqual(OctagonStorage& state,
                                const LinearExpression& expression) const
    {
        if (!expression.terms().empty())
            return false;
        if (expression.constant().isZero())
            state.bottom = true;
        return true;
    }

    bool addLessEqual(OctagonStorage& state, const VariableEnvironment& environment,
                      const LinearExpression& expression, bool strict,
                      bool allowLinearization = true) const
    {
        const auto& terms = expression.terms();
        if (terms.empty())
        {
            const int sign = expression.constant().sign();
            if (sign > 0 || (sign == 0 && strict))
                state.bottom = true;
            return true;
        }

        if (terms.size() == 1)
        {
            const auto& [variable, coefficient] = *terms.begin();
            if (coefficient.isZero())
                return false;
            const Dimension dimension = environment.dimensionOf(variable);
            const Rational magnitude = absolute(coefficient);
            const Rational value = -expression.constant() / magnitude;
            const std::size_t row = coefficient.sign() > 0
                                        ? positiveNode(dimension)
                                        : negativeNode(dimension);
            const std::size_t column = opposite(row);
            setCoherent(state, row, column,
                        Bound::finite(value * Rational(2), strict));
            return true;
        }

        if (terms.size() == 2)
        {
            auto it = terms.begin();
            const Variable lhsVariable = it->first;
            const Rational lhsCoefficient = it->second;
            ++it;
            const Variable rhsVariable = it->first;
            const Rational rhsCoefficient = it->second;
            const Rational lhsMagnitude = absolute(lhsCoefficient);
            if (lhsMagnitude != absolute(rhsCoefficient) ||
                lhsMagnitude.isZero())
                return false;

            const Dimension lhsDimension = environment.dimensionOf(lhsVariable);
            const Dimension rhsDimension = environment.dimensionOf(rhsVariable);
            const std::size_t lhsNode = lhsCoefficient.sign() > 0
                                            ? positiveNode(lhsDimension)
                                            : negativeNode(lhsDimension);
            const std::size_t negativeRhsNode =
                rhsCoefficient.sign() > 0 ? negativeNode(rhsDimension)
                                          : positiveNode(rhsDimension);
            const Rational value = -expression.constant() / lhsMagnitude;
            setCoherent(state, lhsNode, negativeRhsNode,
                        Bound::finite(value, strict));
            return true;
        }

        if (allowLinearization)
            addLinearized(state, environment, expression, strict);
        return false;
    }

    /// Interval of a linear expression under the current per-variable bounds.
    Interval evaluateInterval(const OctagonStorage& state,
                              const VariableEnvironment& environment,
                              const LinearExpression& expression) const
    {
        Rational low = expression.constant();
        Rational high = expression.constant();
        bool lowFinite = true;
        bool highFinite = true;
        bool lowStrict = false;
        bool highStrict = false;
        for (const auto& [variable, coefficient] : expression.terms())
        {
            if (coefficient.isZero())
                continue;
            const Interval interval = bound(state, environment, variable);
            const Bound& least =
                coefficient.sign() > 0 ? interval.lower() : interval.upper();
            const Bound& greatest =
                coefficient.sign() > 0 ? interval.upper() : interval.lower();
            if (lowFinite && least.isFinite())
            {
                low += least.value() * coefficient;
                lowStrict = lowStrict || least.isStrict();
            }
            else
            {
                lowFinite = false;
            }
            if (highFinite && greatest.isFinite())
            {
                high += greatest.value() * coefficient;
                highStrict = highStrict || greatest.isStrict();
            }
            else
            {
                highFinite = false;
            }
        }
        return Interval(lowFinite ? Bound::finite(low, lowStrict)
                                  : Bound::minusInfinity(),
                        highFinite ? Bound::finite(high, highStrict)
                                   : Bound::plusInfinity());
    }

    /// Strongest octagonal consequences of a constraint that is not itself
    /// octagonal.
    ///
    /// A DBM cannot store `sum a_i x_i + k <= 0` once it has three terms, or
    /// two terms of different magnitude. Dropping it is sound and needlessly
    /// weak: replacing the terms that cannot be stored by their current bounds
    /// leaves a constraint over one or two variables that can be. That is
    /// Mine's interval linearization, restricted to the sub-expressions this
    /// domain represents exactly, and it is what makes a guard such as
    /// `2*i + 3*j <= 10` narrow anything at all here.
    void addLinearized(OctagonStorage& state,
                       const VariableEnvironment& environment,
                       const LinearExpression& expression, bool strict) const
    {
        normalize(state, environment);
        if (state.bottom)
            return;

        std::vector<Variable> variables;
        for (const auto& [variable, coefficient] : expression.terms())
        {
            if (!coefficient.isZero())
                variables.push_back(variable);
        }
        if (variables.size() < 2)
            return;

        // Derive every bound from the same pre-constraint snapshot, so the
        // result does not depend on the order the sub-constraints are applied.
        std::map<Variable, Interval> intervals;
        for (Variable variable : variables)
            intervals.emplace(variable, bound(state, environment, variable));

        // Least value the terms outside `kept` can take.
        const auto restMinimum = [&](const std::vector<Variable>& kept,
                                     Rational& total, bool& totalStrict)
        {
            total = Rational();
            totalStrict = false;
            for (const auto& [variable, coefficient] : expression.terms())
            {
                if (coefficient.isZero())
                    continue;
                if (std::find(kept.begin(), kept.end(), variable) != kept.end())
                    continue;
                const Interval& interval = intervals.at(variable);
                const Bound& endpoint = coefficient.sign() > 0
                                            ? interval.lower()
                                            : interval.upper();
                if (!endpoint.isFinite())
                    return false;
                total += endpoint.value() * coefficient;
                totalStrict = totalStrict || endpoint.isStrict();
            }
            return true;
        };

        const auto apply = [&](const std::vector<Variable>& kept)
        {
            Rational rest;
            bool restStrict = false;
            if (!restMinimum(kept, rest, restStrict))
                return;
            LinearExpression reduced(expression.constant() + rest);
            for (Variable variable : kept)
                reduced.setCoefficient(variable,
                                       expression.coefficient(variable));
            addLessEqual(state, environment, reduced, strict || restStrict,
                         false);
        };

        for (std::size_t first = 0; first < variables.size(); ++first)
        {
            apply({variables[first]});
            const Rational firstMagnitude =
                absolute(expression.coefficient(variables[first]));
            for (std::size_t second = first + 1; second < variables.size();
                 ++second)
            {
                if (firstMagnitude ==
                    absolute(expression.coefficient(variables[second])))
                    apply({variables[first], variables[second]});
            }
        }
    }

    void selfAssign(OctagonStorage& state, const VariableEnvironment& environment,
                    Dimension target, int sign, const Rational& constant) const
    {
        normalize(state, environment);
        if (state.bottom)
            return;
        OctagonStorage old = std::move(state);
        OctagonStorage result(old.variableKinds, OctagonStorage::ScratchMatrixTag{});

        auto oldNode = [target, sign](std::size_t newNode) {
            if (newNode == positiveNode(target))
                return sign > 0 ? positiveNode(target) : negativeNode(target);
            if (newNode == negativeNode(target))
                return sign > 0 ? negativeNode(target) : positiveNode(target);
            return newNode;
        };
        auto delta = [target, &constant](std::size_t newNode) {
            if (newNode == positiveNode(target))
                return constant;
            if (newNode == negativeNode(target))
                return -constant;
            return Rational();
        };

        for (std::size_t row = 0; row < result.nodes(); ++row)
        {
            for (std::size_t column = 0; column < result.nodes(); ++column)
            {
                const Bound& oldBound = old.at(oldNode(row), oldNode(column));
                if (!oldBound.isFinite())
                    result.at(row, column) = oldBound;
                else
                    result.at(row, column) = Bound::finite(
                        oldBound.value() + delta(row) - delta(column),
                        oldBound.isStrict());
            }
        }
        result.stronglyClosed = old.stronglyClosed;
        state = std::move(result);
    }

    void enforceCoherence(OctagonStorage& state) const
    {
        for (std::size_t row = 0; row < state.nodes(); ++row)
        {
            for (std::size_t column = 0; column < state.nodes(); ++column)
            {
                const Bound coherent =
                    state.at(opposite(column), opposite(row));
                const Bound minimum =
                    Bound::min(state.at(row, column), coherent);
                state.at(row, column) = minimum;
                state.at(opposite(column), opposite(row)) = minimum;
            }
        }
    }

    void shortestPathClosure(OctagonStorage& state) const
    {
        for (std::size_t middle = 0; middle < state.nodes(); ++middle)
        {
            for (std::size_t row = 0; row < state.nodes(); ++row)
            {
                if (state.at(row, middle).isPlusInfinity())
                    continue;
                for (std::size_t column = 0; column < state.nodes(); ++column)
                {
                    if (state.at(middle, column).isPlusInfinity())
                        continue;
                    const Bound candidate = Bound::add(
                        state.at(row, middle), state.at(middle, column));
                    state.at(row, column) =
                        Bound::min(state.at(row, column), candidate);
                }
            }
        }
    }

    void integerTighten(OctagonStorage& state) const
    {
        if (!options_.integerTightening)
            return;
        for (Dimension dimension = 0; dimension < state.dimensions; ++dimension)
        {
            if (state.variableKinds[dimension] != NumericKind::Integer)
                continue;
            const std::size_t positive = positiveNode(dimension);
            const std::size_t negative = negativeNode(dimension);
            state.at(positive, negative) =
                tightenIntegerUnary(state.at(positive, negative));
            state.at(negative, positive) =
                tightenIntegerUnary(state.at(negative, positive));
        }
    }

    void strongClosure(OctagonStorage& state) const
    {
        if (!options_.strongClosure)
            return;
        for (std::size_t row = 0; row < state.nodes(); ++row)
        {
            for (std::size_t column = 0; column < state.nodes(); ++column)
            {
                const Bound& lhs = state.at(row, opposite(row));
                const Bound& rhs = state.at(opposite(column), column);
                if (lhs.isPlusInfinity() || rhs.isPlusInfinity())
                    continue;
                const Bound candidate =
                    Bound::divideByPositive(Bound::add(lhs, rhs), Rational(2));
                state.at(row, column) =
                    Bound::min(state.at(row, column), candidate);
            }
        }
    }

    bool detectBottom(OctagonStorage& state) const
    {
        for (std::size_t node = 0; node < state.nodes(); ++node)
        {
            if (isNegativeDiagonal(state.at(node, node)))
            {
                state.bottom = true;
                return true;
            }
        }
        return state.bottom;
    }

    void normalize(OctagonStorage& state) const
    {
        if (state.bottom || state.stronglyClosed)
            return;
        enforceCoherence(state);
        shortestPathClosure(state);
        if (detectBottom(state))
            return;
        integerTighten(state);
        shortestPathClosure(state);
        if (detectBottom(state))
            return;
        strongClosure(state);
        enforceCoherence(state);
        if (detectBottom(state))
            return;
        state.stronglyClosed = true;
    }

    void normalize(OctagonStorage& state, const VariableEnvironment& environment) const
    {
        (void)environment;
        normalize(state);
    }

    const OctagonStorage& normalized(
        const OctagonStorage& state,
        std::optional<OctagonStorage>& normalizedStorage) const
    {
        if (state.bottom || state.stronglyClosed)
            return state;
        normalizedStorage.emplace(state);
        normalize(*normalizedStorage);
        return *normalizedStorage;
    }

    void addNodeTerm(LinearExpression& expression,
                     const VariableEnvironment& environment, std::size_t node,
                     const Rational& multiplier) const
    {
        const Dimension dimension = node / 2;
        const Rational sign = node % 2 == 0 ? Rational(1) : Rational(-1);
        const Variable variable = environment.variableOf(dimension);
        expression.setCoefficient(variable, expression.coefficient(variable) +
                                                sign * multiplier);
    }

    OctagonConfig options_;
    OctagonStorage state_;
};

namespace SVF::AbstractDomain
{

OctagonState::OctagonState(VariableEnvironment environment, OctagonConfig config,
                           bool bottom)
    : environment_(std::move(environment)),
      impl_(std::make_unique<Impl>(environment_, std::move(config), bottom))
{
}

OctagonState::OctagonState(VariableEnvironment environment, std::unique_ptr<Impl> impl)
    : environment_(std::move(environment)), impl_(std::move(impl))
{
}

OctagonState OctagonState::top(const VariableEnvironment& environment,
                               const OctagonConfig& config)
{
    return OctagonState(environment, config, false);
}

OctagonState OctagonState::bottom(const VariableEnvironment& environment,
                                  const OctagonConfig& config)
{
    return OctagonState(environment, config, true);
}

OctagonState OctagonState::fromBox(const VariableEnvironment& environment,
                                   const IntervalBox& box,
                                   const OctagonConfig& config)
{
    OctagonState state = top(environment, config);
    for (const auto& [variable, interval] : box.bounds)
    {
        if (!environment.contains(variable))
            throw std::invalid_argument("box contains an unknown variable");
        if (interval.isBottom())
            return bottom(environment, config);
        if (interval.lower().isFinite())
        {
            LinearExpression expression(variable);
            expression.setConstant(-interval.lower().value());
            state.assume(LinearConstraint(
                std::move(expression),
                interval.lower().isStrict() ? ConstraintKind::GreaterThan
                                            : ConstraintKind::GreaterEqual));
        }
        if (interval.upper().isFinite())
        {
            LinearExpression expression(variable);
            expression.setConstant(-interval.upper().value());
            state.assume(LinearConstraint(
                std::move(expression),
                interval.upper().isStrict() ? ConstraintKind::LessThan
                                            : ConstraintKind::LessEqual));
        }
    }
    return state;
}

OctagonState OctagonState::fromConstraints(
    const VariableEnvironment& environment,
    const LinearConstraintSet& constraints,
    const OctagonConfig& config)
{
    OctagonState state = top(environment, config);
    for (const LinearConstraint& constraint : constraints)
        state.assume(constraint);
    return state;
}

OctagonState::OctagonState(const OctagonState& other)
    : NumericalState(other), environment_(other.environment_),
      impl_(std::make_unique<Impl>(*other.impl_))
{
}

OctagonState::OctagonState(OctagonState&& other) noexcept = default;

OctagonState& OctagonState::operator=(const OctagonState& other)
{
    if (this == &other)
        return *this;
    NumericalState::operator=(other);
    environment_ = other.environment_;
    impl_ = std::make_unique<Impl>(*other.impl_);
    return *this;
}

OctagonState& OctagonState::operator=(OctagonState&& other) noexcept = default;

OctagonState::~OctagonState() = default;

std::unique_ptr<AbstractState> OctagonState::clone() const
{
    return std::make_unique<OctagonState>(*this);
}

const char* OctagonState::name() const
{
    return impl_->name();
}

DomainCapabilities OctagonState::capabilities() const
{
    return impl_->capabilities();
}

void OctagonState::report(OperationKind operation,
                          ApproximationKind approximation,
                          std::string reason) const
{
    DiagnosticSink* sink = diagnosticSink();
    if (sink && approximation != ApproximationKind::Exact)
        sink->report({operation, approximation, std::move(reason)});
}

void OctagonState::assign(Variable target,
                          const LinearExpression& expression)
{
    const ApproximationKind approximation = assignState(target, expression);
    report(OperationKind::Assignment, approximation,
           approximation == ApproximationKind::UnsupportedFallback
               ? std::string(name()) +
                     " forgot a target assigned an unsupported linear expression"
               : std::string(name()) +
                     " approximated a linear assignment");
}

void OctagonState::assign(Variable target, const TreeExpression& expression)
{
    if (const auto linear = expression.asLinear())
    {
        assign(target, *linear);
        return;
    }
    forget(target);
    report(OperationKind::Assignment,
           ApproximationKind::UnsupportedFallback,
           std::string(name()) +
               " forgot a target assigned a nonlinear or floating tree expression");
}

void OctagonState::assume(const LinearConstraint& constraint)
{
    const ApproximationKind approximation = assumeState(constraint);
    report(OperationKind::Assumption, approximation,
           std::string(name()) +
               " ignored or approximated an unsupported constraint");
}

void OctagonState::assume(const TreeConstraint& constraint)
{
    if (const auto linear = constraint.expression().asLinear())
    {
        assume(LinearConstraint(*linear, constraint.kind()));
        return;
    }
    report(OperationKind::Assumption,
           ApproximationKind::UnsupportedFallback,
           std::string(name()) +
               " ignored a nonlinear or floating assumption");
}

void OctagonState::forget(Variable variable)
{
    forgetState(variable);
}

void OctagonState::projectLowerBounds()
{
    projectLowerBoundsState();
}

void OctagonState::changeEnvironment(const VariableEnvironment& environment,
                                     bool initializeNewVariablesToZero)
{
    const VariableEnvironment oldEnvironment = environment_;
    changeEnvironmentState(oldEnvironment, environment, initializeNewVariablesToZero);
    environment_ = environment;
}

CheckResult OctagonState::entails(const LinearConstraint& constraint) const
{
    if (constraint.kind() == ConstraintKind::Equal)
    {
        const CheckResult le = entails(LinearConstraint(
            constraint.expression(), ConstraintKind::LessEqual));
        const CheckResult ge = entails(LinearConstraint(
            constraint.expression(), ConstraintKind::GreaterEqual));
        if (le == CheckResult::True && ge == CheckResult::True)
            return CheckResult::True;
        if (le == CheckResult::False || ge == CheckResult::False)
            return CheckResult::False;
        return CheckResult::Unknown;
    }

    if (constraint.kind() == ConstraintKind::NotEqual)
    {
        OctagonState witness(*this);
        witness.assume(LinearConstraint(constraint.expression(),
                                        ConstraintKind::Equal));
        return witness.isBottom() ? CheckResult::True : CheckResult::False;
    }

    ConstraintKind negated;
    switch (constraint.kind())
    {
    case ConstraintKind::LessThan:
        negated = ConstraintKind::GreaterEqual;
        break;
    case ConstraintKind::LessEqual:
        negated = ConstraintKind::GreaterThan;
        break;
    case ConstraintKind::GreaterThan:
        negated = ConstraintKind::LessEqual;
        break;
    case ConstraintKind::GreaterEqual:
        negated = ConstraintKind::LessThan;
        break;
    case ConstraintKind::Equal:
    case ConstraintKind::NotEqual:
        throw std::logic_error("equality entailment was not normalized");
    }
    OctagonState counterexample(*this);
    counterexample.assume(
        LinearConstraint(constraint.expression(), negated));
    return counterexample.isBottom() ? CheckResult::True : CheckResult::False;
}

Interval OctagonState::bound(Variable variable) const
{
    return boundState(variable);
}

IntervalBox OctagonState::toBox() const
{
    IntervalBox box;
    for (const VariableDeclaration& declaration : environment_.variables())
        box.bounds.emplace(declaration.variable, bound(declaration.variable));
    return box;
}

LinearConstraintSet OctagonState::toConstraints() const
{
    return constraintsState();
}

const OctagonConfig& OctagonState::config() const
{
    return impl_->config();
}

OctagonState OctagonState::reconfigured(const OctagonConfig& config) const
{
    OctagonState result(*this);
    result.impl_->reconfigure(config);
    return result;
}

OctagonState OctagonState::join(const OctagonState& other) const
{
    requireCompatible(other);
    return OctagonState(environment(), impl_->joined(*other.impl_));
}

OctagonState OctagonState::meet(const OctagonState& other) const
{
    requireCompatible(other);
    return OctagonState(environment(), impl_->met(*other.impl_));
}

OctagonState OctagonState::widen(
    const OctagonState& next, const WideningPolicy& policy) const
{
    requireCompatible(next);
    return OctagonState(environment(), impl_->widened(*next.impl_, policy));
}

OctagonState OctagonState::narrow(const OctagonState& next) const
{
    requireCompatible(next);
    if (!next.leqState(*this))
        throw std::invalid_argument(
            "narrowing requires next to be included in current");
    return OctagonState(environment(), impl_->narrowed(*next.impl_));
}

OctagonState OctagonState::projectedLowerBounds() const
{
    return OctagonState(environment(), impl_->projectedLowerBounds());
}

OctagonState OctagonState::withEnvironment(
    const VariableEnvironment& environment, bool initializeNewVariablesToZero) const
{
    return OctagonState(
        environment,
        impl_->changedEnvironment(this->environment(), environment,
                                  initializeNewVariablesToZero));
}

DiagnosticSink* OctagonState::diagnosticSink() const
{
    return impl_->config().diagnostics.get();
}

const OctagonState& OctagonState::requireOctagon(
    const AbstractState& other) const
{
    const auto* octagon = dynamic_cast<const OctagonState*>(&other);
    if (!octagon)
        throw std::invalid_argument("relational state is not an OctagonState");
    return *octagon;
}

bool OctagonState::hasCompatibleDomain(const AbstractState& other) const
{
    const auto* octagon = dynamic_cast<const OctagonState*>(&other);
    return octagon && environment_ == octagon->environment_ &&
           config().operationCompatible(octagon->config());
}

ApproximationKind OctagonState::assignState(
    Variable target, const LinearExpression& expression)
{
    return impl_->assignCurrent(environment(), target, expression);
}

ApproximationKind OctagonState::assumeState(
    const LinearConstraint& constraint)
{
    return impl_->assumeCurrent(environment(), constraint);
}

void OctagonState::forgetState(Variable variable)
{
    impl_->forgetCurrent(environment(), variable);
}

void OctagonState::joinState(const AbstractState& other)
{
    impl_->joinCurrent(*requireOctagon(other).impl_);
}

void OctagonState::meetState(const AbstractState& other)
{
    impl_->meetCurrent(*requireOctagon(other).impl_);
}

void OctagonState::widenState(const AbstractState& next)
{
    impl_->widenCurrent(*requireOctagon(next).impl_, {});
}

void OctagonState::narrowState(const AbstractState& next)
{
    impl_->narrowCurrent(*requireOctagon(next).impl_);
}

void OctagonState::projectLowerBoundsState()
{
    impl_->projectLowerBoundsCurrent();
}

void OctagonState::changeEnvironmentState(
    const VariableEnvironment& oldEnvironment, const VariableEnvironment& newEnvironment,
    bool initializeNewVariablesToZero)
{
    impl_->changeEnvironmentCurrent(oldEnvironment, newEnvironment,
                                    initializeNewVariablesToZero);
}

bool OctagonState::isBottomState() const
{
    return impl_->isBottomCurrent();
}

bool OctagonState::isTopState() const
{
    return impl_->isTopCurrent();
}

bool OctagonState::leqState(const AbstractState& other) const
{
    return impl_->leqCurrent(*requireOctagon(other).impl_);
}

Interval OctagonState::boundState(Variable variable) const
{
    return impl_->boundCurrent(environment(), variable);
}

LinearConstraintSet OctagonState::constraintsState() const
{
    return impl_->constraintsCurrent(environment());
}

std::string OctagonState::stateToString() const
{
    return impl_->toStringCurrent(environment());
}

} // namespace SVF::AbstractDomain
