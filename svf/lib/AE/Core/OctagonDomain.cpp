//===- OctagonDomain.cpp -- Exact GMP octagon relational backend --------===//

#include "AE/Core/OctagonDomain.h"

#include <algorithm>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <utility>

using namespace relational;

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

class OctagonState final : public DomainState
{
public:
    struct ScratchMatrixTag
    {
    };

    explicit OctagonState(const Environment& environment, bool bottom = false)
        : OctagonState(extractVariableKinds(environment), bottom)
    {
    }

    explicit OctagonState(std::vector<NumericKind> variableKinds,
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

    OctagonState(std::vector<NumericKind> variableKinds, ScratchMatrixTag)
        : dimensions(variableKinds.size()),
          variableKinds(std::move(variableKinds)),
          matrix(matrixSize(dimensions))
    {
    }

    std::unique_ptr<DomainState> clone() const override
    {
        return std::make_unique<OctagonState>(*this);
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
    /// A coherent Octagon DBM satisfies M[i,j] = M[j^1,i^1], so only one
    /// representative of each pair is stored. This is APRON's hmat layout;
    /// at(row,column) retains the logical 2n x 2n matrix interface.
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
        const Environment& environment)
    {
        std::vector<NumericKind> result;
        result.reserve(environment.size());
        for (const VariableDeclaration& declaration : environment.variables())
            result.push_back(declaration.type.kind);
        return result;
    }
};

const OctagonState& asOctagon(const DomainState& state)
{
    return static_cast<const OctagonState&>(state);
}

OctagonState& asOctagon(DomainState& state)
{
    return static_cast<OctagonState&>(state);
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

class relational::OctagonDomain::Impl final
{
public:
    explicit Impl(OctagonOptions options) : options_(std::move(options)) {}

    const char* name() const
    {
        return "gmp-octagon";
    }

    DomainCapabilities capabilities() const
    {
        return {/*strictInequalities=*/true,
                /*integerTightening=*/options_.integerTightening,
                /*thresholdWidening=*/true,
                /*narrowing=*/true,
                /*treeExpressions=*/false};
    }

    std::unique_ptr<DomainState> top(const Environment& environment) const
    {
        return std::make_unique<OctagonState>(environment);
    }

    std::unique_ptr<DomainState> bottom(const Environment& environment) const
    {
        return std::make_unique<OctagonState>(environment, true);
    }

    ApproximationKind assign(DomainState& genericState,
                             const Environment& environment, Variable target,
                             const LinearExpression& expression) const
    {
        OctagonState& state = asOctagon(genericState);
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
        // target before falling back.  Unary recovery can be added later.
        forget(state, targetDimension);
        return ApproximationKind::UnsupportedFallback;
    }

    ApproximationKind assume(DomainState& genericState,
                             const Environment& environment,
                             const LinearConstraint& constraint) const
    {
        OctagonState& state = asOctagon(genericState);
        requireVariables(environment, constraint.expression());
        for (const auto& [variable, coefficient] :
             constraint.expression().terms())
        {
            (void)coefficient;
            if (environment.typeOf(variable).kind == NumericKind::IEEEFloat)
                return ApproximationKind::UnsupportedFallback;
        }

        const bool supported = addConstraint(state, environment, constraint);
        if (supported)
            normalize(state, environment);
        return supported ? ApproximationKind::Exact
                         : ApproximationKind::UnsupportedFallback;
    }

    void forget(DomainState& genericState, const Environment& environment,
                Variable variable) const
    {
        if (!environment.contains(variable))
            throw std::invalid_argument(
                "forgotten variable is not in relational environment");
        forget(asOctagon(genericState), environment.dimensionOf(variable));
    }

    std::unique_ptr<DomainState> join(const DomainState& genericLhs,
                                      const DomainState& genericRhs) const
    {
        std::optional<OctagonState> lhsStorage;
        std::optional<OctagonState> rhsStorage;
        const OctagonState& lhs =
            normalized(asOctagon(genericLhs), lhsStorage);
        const OctagonState& rhs =
            normalized(asOctagon(genericRhs), rhsStorage);
        requireSameSize(lhs, rhs);
        if (lhs.bottom)
            return rhs.clone();
        if (rhs.bottom)
            return lhs.clone();

        auto result = std::make_unique<OctagonState>(
            lhs.variableKinds, OctagonState::ScratchMatrixTag{});
        for (std::size_t index = 0; index < result->matrix.size(); ++index)
        {
            const Bound& left = lhs.matrix[index];
            const Bound& right = rhs.matrix[index];
            result->matrix[index] = left <= right ? right : left;
        }

        // Point-wise maximum preserves coherence, shortest-path closure and
        // strong closure: every right-hand side in a closure inequality can
        // only grow. Integer-tight unary entries remain even because the
        // maximum of two even bounds is even. APRON's oct_join uses this same
        // closed-result fast path instead of scheduling another cubic close.
        result->stronglyClosed = true;
        return result;
    }

    std::unique_ptr<DomainState> meet(const DomainState& genericLhs,
                                      const DomainState& genericRhs) const
    {
        std::optional<OctagonState> lhsStorage;
        std::optional<OctagonState> rhsStorage;
        const OctagonState& lhs =
            normalized(asOctagon(genericLhs), lhsStorage);
        const OctagonState& rhs =
            normalized(asOctagon(genericRhs), rhsStorage);
        requireSameSize(lhs, rhs);
        if (lhs.bottom)
            return lhs.clone();
        if (rhs.bottom)
            return rhs.clone();

        auto result = std::make_unique<OctagonState>(
            lhs.variableKinds, OctagonState::ScratchMatrixTag{});
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

    std::unique_ptr<DomainState> widen(const DomainState& genericCurrent,
                                       const DomainState& genericNext,
                                       const WideningPolicy& policy) const
    {
        const OctagonState& current = asOctagon(genericCurrent);
        std::optional<OctagonState> nextStorage;
        const OctagonState& next =
            normalized(asOctagon(genericNext), nextStorage);
        requireSameSize(current, next);
        if (current.bottom)
            return next.clone();
        if (next.bottom)
            return current.clone();

        std::vector<Rational> thresholds = policy.thresholds;
        std::sort(thresholds.begin(), thresholds.end());

        auto result = std::make_unique<OctagonState>(
            current.variableKinds, OctagonState::ScratchMatrixTag{});
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

    std::unique_ptr<DomainState> narrow(const DomainState& genericCurrent,
                                        const DomainState& genericNext) const
    {
        std::optional<OctagonState> currentStorage;
        std::optional<OctagonState> nextStorage;
        const OctagonState& current =
            normalized(asOctagon(genericCurrent), currentStorage);
        const OctagonState& next =
            normalized(asOctagon(genericNext), nextStorage);
        requireSameSize(current, next);
        if (current.bottom || next.bottom)
            return next.clone();

        auto result = std::make_unique<OctagonState>(current);
        for (std::size_t index = 0; index < result->matrix.size(); ++index)
        {
            if (current.matrix[index].isPlusInfinity() &&
                next.matrix[index].isFinite())
                result->matrix[index] = next.matrix[index];
        }
        result->stronglyClosed = false;
        return result;
    }

    std::unique_ptr<DomainState> projectLowerBounds(
        const DomainState& genericState) const
    {
        std::optional<OctagonState> sourceStorage;
        const OctagonState& source =
            normalized(asOctagon(genericState), sourceStorage);
        if (source.bottom)
            return source.clone();

        auto result = std::make_unique<OctagonState>(source.variableKinds);
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

    std::unique_ptr<DomainState> changeEnvironment(
        const DomainState& genericState, const Environment& oldEnvironment,
        const Environment& newEnvironment, bool projectNewVariables) const
    {
        std::optional<OctagonState> sourceStorage;
        const OctagonState& source =
            normalized(asOctagon(genericState), sourceStorage);
        if (source.dimensions != oldEnvironment.size())
            throw std::invalid_argument(
                "old environment does not match octagon dimensions");
        if (source.bottom)
            return std::make_unique<OctagonState>(newEnvironment, true);

        auto result = std::make_unique<OctagonState>(newEnvironment);
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

        if (projectNewVariables)
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

    bool isBottom(const DomainState& genericState) const
    {
        std::optional<OctagonState> storage;
        return normalized(asOctagon(genericState), storage).bottom;
    }

    bool isTop(const DomainState& genericState) const
    {
        std::optional<OctagonState> storage;
        const OctagonState& state =
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

    bool leq(const DomainState& genericLhs, const DomainState& genericRhs) const
    {
        std::optional<OctagonState> lhsStorage;
        std::optional<OctagonState> rhsStorage;
        const OctagonState& lhs =
            normalized(asOctagon(genericLhs), lhsStorage);
        const OctagonState& rhs =
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

    Interval bound(const DomainState& genericState,
                   const Environment& environment, Variable variable) const
    {
        if (!environment.contains(variable))
            throw std::invalid_argument(
                "bounded variable is not in relational environment");
        std::optional<OctagonState> storage;
        const OctagonState& state =
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

    LinearConstraintSet constraints(const DomainState& genericState,
                                    const Environment& environment) const
    {
        std::optional<OctagonState> storage;
        const OctagonState& state =
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

    std::string toString(const DomainState& genericState,
                         const Environment& environment) const
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

private:
    void requireSameSize(const OctagonState& lhs, const OctagonState& rhs) const
    {
        if (lhs.dimensions != rhs.dimensions)
            throw std::invalid_argument("octagon dimensions do not match");
    }

    void requireVariables(const Environment& environment,
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

    void setCoherent(OctagonState& state, std::size_t row, std::size_t column,
                     const Bound& bound) const
    {
        state.at(row, column) = Bound::min(state.at(row, column), bound);
        const std::size_t coherentRow = opposite(column);
        const std::size_t coherentColumn = opposite(row);
        state.at(coherentRow, coherentColumn) =
            Bound::min(state.at(coherentRow, coherentColumn), bound);
        state.stronglyClosed = false;
    }

    void forget(OctagonState& state, Dimension dimension) const
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

    bool addConstraint(OctagonState& state, const Environment& environment,
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

    bool handleConstantNotEqual(OctagonState& state,
                                const LinearExpression& expression) const
    {
        if (!expression.terms().empty())
            return false;
        if (expression.constant().isZero())
            state.bottom = true;
        return true;
    }

    bool addLessEqual(OctagonState& state, const Environment& environment,
                      const LinearExpression& expression, bool strict) const
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
        return false;
    }

    void selfAssign(OctagonState& state, const Environment& environment,
                    Dimension target, int sign, const Rational& constant) const
    {
        normalize(state, environment);
        if (state.bottom)
            return;
        OctagonState old = std::move(state);
        OctagonState result(old.variableKinds, OctagonState::ScratchMatrixTag{});

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

    void enforceCoherence(OctagonState& state) const
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

    void shortestPathClosure(OctagonState& state) const
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

    void integerTighten(OctagonState& state) const
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

    void strongClosure(OctagonState& state) const
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

    bool detectBottom(OctagonState& state) const
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

    void normalize(OctagonState& state) const
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

    void normalize(OctagonState& state, const Environment& environment) const
    {
        (void)environment;
        normalize(state);
    }

    const OctagonState& normalized(
        const OctagonState& state,
        std::optional<OctagonState>& normalizedStorage) const
    {
        if (state.bottom || state.stronglyClosed)
            return state;
        normalizedStorage.emplace(state);
        normalize(*normalizedStorage);
        return *normalizedStorage;
    }

    void addNodeTerm(LinearExpression& expression,
                     const Environment& environment, std::size_t node,
                     const Rational& multiplier) const
    {
        const Dimension dimension = node / 2;
        const Rational sign = node % 2 == 0 ? Rational(1) : Rational(-1);
        const Variable variable = environment.variableOf(dimension);
        expression.setCoefficient(variable, expression.coefficient(variable) +
                                                sign * multiplier);
    }

    OctagonOptions options_;
};

namespace relational
{

OctagonDomain::OctagonDomain(const OctagonOptions& options)
    : AbstractDomain(options.diagnostics),
      impl_(std::make_unique<Impl>(options))
{
}

OctagonDomain::~OctagonDomain() = default;

const char* OctagonDomain::name() const
{
    return impl_->name();
}

DomainCapabilities OctagonDomain::capabilities() const
{
    return impl_->capabilities();
}

std::unique_ptr<DomainState> OctagonDomain::makeTop(
    const Environment& environment) const
{
    return impl_->top(environment);
}

std::unique_ptr<DomainState> OctagonDomain::makeBottom(
    const Environment& environment) const
{
    return impl_->bottom(environment);
}

ApproximationKind OctagonDomain::assignState(
    DomainState& state, const Environment& environment, Variable target,
    const LinearExpression& expression) const
{
    return impl_->assign(state, environment, target, expression);
}

ApproximationKind OctagonDomain::assumeState(
    DomainState& state, const Environment& environment,
    const LinearConstraint& constraint) const
{
    return impl_->assume(state, environment, constraint);
}

void OctagonDomain::forgetState(DomainState& state,
                                const Environment& environment,
                                Variable variable) const
{
    impl_->forget(state, environment, variable);
}

std::unique_ptr<DomainState> OctagonDomain::joinStates(
    const DomainState& lhs, const DomainState& rhs) const
{
    return impl_->join(lhs, rhs);
}

std::unique_ptr<DomainState> OctagonDomain::meetStates(
    const DomainState& lhs, const DomainState& rhs) const
{
    return impl_->meet(lhs, rhs);
}

std::unique_ptr<DomainState> OctagonDomain::widenStates(
    const DomainState& current, const DomainState& next,
    const WideningPolicy& policy) const
{
    return impl_->widen(current, next, policy);
}

std::unique_ptr<DomainState> OctagonDomain::narrowStates(
    const DomainState& current, const DomainState& next) const
{
    return impl_->narrow(current, next);
}

std::unique_ptr<DomainState> OctagonDomain::projectLowerBoundsState(
    const DomainState& state) const
{
    return impl_->projectLowerBounds(state);
}

std::unique_ptr<DomainState> OctagonDomain::changeEnvironmentState(
    const DomainState& state, const Environment& oldEnvironment,
    const Environment& newEnvironment, bool projectNewVariables) const
{
    return impl_->changeEnvironment(state, oldEnvironment, newEnvironment,
                                    projectNewVariables);
}

bool OctagonDomain::isBottomState(const DomainState& state) const
{
    return impl_->isBottom(state);
}

bool OctagonDomain::isTopState(const DomainState& state) const
{
    return impl_->isTop(state);
}

bool OctagonDomain::leqStates(const DomainState& lhs,
                              const DomainState& rhs) const
{
    return impl_->leq(lhs, rhs);
}

Interval OctagonDomain::boundState(const DomainState& state,
                                   const Environment& environment,
                                   Variable variable) const
{
    return impl_->bound(state, environment, variable);
}

LinearConstraintSet OctagonDomain::constraintsState(
    const DomainState& state, const Environment& environment) const
{
    return impl_->constraints(state, environment);
}

std::string OctagonDomain::stateToString(const DomainState& state,
                                         const Environment& environment) const
{
    return impl_->toString(state, environment);
}

std::shared_ptr<OctagonDomain> makeOctagonDomain(const OctagonOptions& options)
{
    return std::shared_ptr<OctagonDomain>(new OctagonDomain(options));
}

} // namespace relational
