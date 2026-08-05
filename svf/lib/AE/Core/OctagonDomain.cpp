//===- OctagonDomain.cpp -- Exact GMP octagon relational backend --------===//

#include "AE/Core/OctagonDomain.h"

#include <algorithm>
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

class OctagonState final : public BackendState
{
public:
    explicit OctagonState(const Environment& environment, bool bottom = false)
        : OctagonState(extractVariableKinds(environment), bottom)
    {
    }

    explicit OctagonState(std::vector<NumericKind> variableKinds,
                          bool bottom = false)
        : dimensions(variableKinds.size()),
          variableKinds(std::move(variableKinds)),
          matrix(4 * dimensions * dimensions, Bound::plusInfinity()),
          bottom(bottom)
    {
        const std::size_t count = nodes();
        for (std::size_t node = 0; node < count; ++node)
            at(node, node) = Bound::finite(Rational());
    }

    std::unique_ptr<BackendState> clone() const override
    {
        return std::make_unique<OctagonState>(*this);
    }

    std::size_t nodes() const { return 2 * dimensions; }

    Bound& at(std::size_t row, std::size_t column)
    {
        return matrix[row * nodes() + column];
    }

    const Bound& at(std::size_t row, std::size_t column) const
    {
        return matrix[row * nodes() + column];
    }

    std::size_t dimensions;
    std::vector<NumericKind> variableKinds;
    std::vector<Bound> matrix;
    bool bottom = false;
    bool stronglyClosed = true;

private:
    static std::vector<NumericKind>
    extractVariableKinds(const Environment& environment)
    {
        std::vector<NumericKind> result;
        result.reserve(environment.size());
        for (const VariableDeclaration& declaration : environment.variables())
            result.push_back(declaration.type.kind);
        return result;
    }
};

const OctagonState& asOctagon(const BackendState& state)
{
    return static_cast<const OctagonState&>(state);
}

OctagonState& asOctagon(BackendState& state)
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

class OctagonBackend final : public DomainBackend
{
public:
    explicit OctagonBackend(OctagonOptions options)
        : options_(std::move(options))
    {
    }

    const char* name() const override { return "gmp-octagon"; }

    DomainCapabilities capabilities() const override
    {
        return {/*strictInequalities=*/true,
                /*integerTightening=*/options_.integerTightening,
                /*thresholdWidening=*/true,
                /*narrowing=*/true,
                /*treeExpressions=*/false};
    }

    std::unique_ptr<BackendState>
    top(const Environment& environment) const override
    {
        return std::make_unique<OctagonState>(environment);
    }

    std::unique_ptr<BackendState>
    bottom(const Environment& environment) const override
    {
        return std::make_unique<OctagonState>(environment, true);
    }

    ApproximationKind assign(BackendState& genericState,
                             const Environment& environment, Variable target,
                             const LinearExpression& expression) const override
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
            addConstraint(state, environment,
                          LinearConstraint(std::move(equality),
                                           ConstraintKind::Equal));
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

    ApproximationKind assume(BackendState& genericState,
                             const Environment& environment,
                             const LinearConstraint& constraint) const override
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

    void forget(BackendState& genericState, const Environment& environment,
                Variable variable) const override
    {
        if (!environment.contains(variable))
            throw std::invalid_argument(
                "forgotten variable is not in relational environment");
        forget(asOctagon(genericState), environment.dimensionOf(variable));
    }

    std::unique_ptr<BackendState>
    join(const BackendState& genericLhs,
         const BackendState& genericRhs) const override
    {
        OctagonState lhs = normalized(asOctagon(genericLhs));
        OctagonState rhs = normalized(asOctagon(genericRhs));
        requireSameSize(lhs, rhs);
        if (lhs.bottom)
            return rhs.clone();
        if (rhs.bottom)
            return lhs.clone();

        OctagonState result(lhs.variableKinds);
        for (std::size_t index = 0; index < result.matrix.size(); ++index)
            result.matrix[index] = Bound::max(lhs.matrix[index],
                                              rhs.matrix[index]);
        result.stronglyClosed = false;
        normalize(result);
        return result.clone();
    }

    std::unique_ptr<BackendState>
    meet(const BackendState& genericLhs,
         const BackendState& genericRhs) const override
    {
        OctagonState lhs = normalized(asOctagon(genericLhs));
        OctagonState rhs = normalized(asOctagon(genericRhs));
        requireSameSize(lhs, rhs);
        if (lhs.bottom)
            return lhs.clone();
        if (rhs.bottom)
            return rhs.clone();

        OctagonState result(lhs.variableKinds);
        for (std::size_t index = 0; index < result.matrix.size(); ++index)
            result.matrix[index] = Bound::min(lhs.matrix[index],
                                              rhs.matrix[index]);
        result.stronglyClosed = false;
        normalize(result);
        return result.clone();
    }

    std::unique_ptr<BackendState>
    widen(const BackendState& genericCurrent,
          const BackendState& genericNext,
          const WideningPolicy& policy) const override
    {
        const OctagonState& current = asOctagon(genericCurrent);
        OctagonState next = normalized(asOctagon(genericNext));
        requireSameSize(current, next);
        if (current.bottom)
            return next.clone();
        if (next.bottom)
            return current.clone();

        std::vector<Rational> thresholds = policy.thresholds;
        std::sort(thresholds.begin(), thresholds.end());

        OctagonState result(current.variableKinds);
        for (std::size_t row = 0; row < result.nodes(); ++row)
        {
            for (std::size_t column = 0; column < result.nodes(); ++column)
            {
                const Bound& oldBound = current.at(row, column);
                const Bound& nextBound = next.at(row, column);
                if (nextBound <= oldBound)
                {
                    result.at(row, column) = oldBound;
                    continue;
                }

                result.at(row, column) = Bound::plusInfinity();
                if (nextBound.isFinite())
                {
                    // Public thresholds use normalized octagonal constants.
                    // A unary DBM entry encodes +/-2*x, so its matrix bound
                    // needs twice the user-visible threshold.
                    const bool unary = row != column &&
                                       row / 2 == column / 2;
                    for (const Rational& threshold : thresholds)
                    {
                        Bound candidate = Bound::finite(
                            unary ? threshold * Rational(2) : threshold);
                        if (nextBound <= candidate)
                        {
                            result.at(row, column) = std::move(candidate);
                            break;
                        }
                    }
                }
            }
        }
        for (std::size_t node = 0; node < result.nodes(); ++node)
            result.at(node, node) = Bound::finite(Rational());
        result.stronglyClosed = false;
        return result.clone();
    }

    std::unique_ptr<BackendState>
    narrow(const BackendState& genericCurrent,
           const BackendState& genericNext) const override
    {
        const OctagonState& current = asOctagon(genericCurrent);
        OctagonState next = normalized(asOctagon(genericNext));
        requireSameSize(current, next);
        if (current.bottom || next.bottom)
            return next.clone();

        OctagonState result = current;
        for (std::size_t index = 0; index < result.matrix.size(); ++index)
        {
            if (current.matrix[index].isPlusInfinity() &&
                    next.matrix[index].isFinite())
                result.matrix[index] = next.matrix[index];
        }
        result.stronglyClosed = false;
        return result.clone();
    }

    std::unique_ptr<BackendState>
    projectLowerBounds(const BackendState& genericState) const override
    {
        OctagonState source = normalized(asOctagon(genericState));
        if (source.bottom)
            return source.clone();

        OctagonState result(source.variableKinds);
        for (Dimension dimension = 0; dimension < source.dimensions;
             ++dimension)
        {
            setCoherent(result, negativeNode(dimension),
                        positiveNode(dimension),
                        source.at(negativeNode(dimension),
                                  positiveNode(dimension)));
        }
        for (Dimension lhs = 0; lhs < source.dimensions; ++lhs)
        {
            for (Dimension rhs = lhs + 1; rhs < source.dimensions; ++rhs)
            {
                // -(x+y) and -(x-y) are the selected lower orientations.
                setCoherent(result, negativeNode(lhs), positiveNode(rhs),
                            source.at(negativeNode(lhs), positiveNode(rhs)));
                setCoherent(result, negativeNode(lhs), negativeNode(rhs),
                            source.at(negativeNode(lhs), negativeNode(rhs)));
            }
        }
        result.stronglyClosed = false;
        normalize(result);
        return result.clone();
    }

    std::unique_ptr<BackendState>
    changeEnvironment(const BackendState& genericState,
                      const Environment& oldEnvironment,
                      const Environment& newEnvironment,
                      bool projectNewVariables) const override
    {
        OctagonState source = normalized(asOctagon(genericState));
        if (source.dimensions != oldEnvironment.size())
            throw std::invalid_argument(
                "old environment does not match octagon dimensions");
        if (source.bottom)
            return std::make_unique<OctagonState>(newEnvironment, true);

        OctagonState result(newEnvironment);
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
                        result.at(2 * newRowDimension + rowSign,
                                  2 * newColumnDimension + columnSign) =
                            source.at(2 * oldRowDimension + rowSign,
                                      2 * oldColumnDimension + columnSign);
                    }
                }
            }
        }

        if (projectNewVariables)
        {
            for (Dimension dimension = 0;
                 dimension < newEnvironment.size(); ++dimension)
            {
                const Variable value = newEnvironment.variableOf(dimension);
                if (oldEnvironment.contains(value))
                    continue;
                setCoherent(result, positiveNode(dimension),
                            negativeNode(dimension),
                            Bound::finite(Rational()));
                setCoherent(result, negativeNode(dimension),
                            positiveNode(dimension),
                            Bound::finite(Rational()));
            }
        }
        result.stronglyClosed = false;
        normalize(result);
        return result.clone();
    }

    bool isBottom(const BackendState& genericState) const override
    {
        return normalized(asOctagon(genericState)).bottom;
    }

    bool isTop(const BackendState& genericState) const override
    {
        OctagonState state = normalized(asOctagon(genericState));
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

    bool leq(const BackendState& genericLhs,
             const BackendState& genericRhs) const override
    {
        OctagonState lhs = normalized(asOctagon(genericLhs));
        OctagonState rhs = normalized(asOctagon(genericRhs));
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

    Interval bound(const BackendState& genericState,
                   const Environment& environment,
                   Variable variable) const override
    {
        if (!environment.contains(variable))
            throw std::invalid_argument(
                "bounded variable is not in relational environment");
        OctagonState state = normalized(asOctagon(genericState));
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
            lower = Bound::finite(
                -(doubledNegativeLower.value() / Rational(2)),
                doubledNegativeLower.isStrict());
        }
        return Interval(std::move(lower), std::move(upper));
    }

    ConstraintSet constraints(const BackendState& genericState,
                              const Environment& environment) const override
    {
        OctagonState state = normalized(asOctagon(genericState));
        ConstraintSet result;
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
                result.emplace_back(
                    std::move(expression),
                    state.at(row, column).isStrict()
                        ? ConstraintKind::LessThan
                        : ConstraintKind::LessEqual);
            }
        }
        return result;
    }

    std::string toString(const BackendState& genericState,
                         const Environment& environment) const override
    {
        if (isBottom(genericState))
            return "bottom";
        const ConstraintSet exported = constraints(genericState, environment);
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
    void requireSameSize(const OctagonState& lhs,
                         const OctagonState& rhs) const
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

    void setCoherent(OctagonState& state, std::size_t row,
                     std::size_t column, const Bound& bound) const
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
        case ConstraintKind::Equal:
        {
            const bool forward = addLessEqual(
                state, environment, constraint.expression(), false);
            const bool backward = addLessEqual(
                state, environment, -constraint.expression(), false);
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

            const Dimension lhsDimension =
                environment.dimensionOf(lhsVariable);
            const Dimension rhsDimension =
                environment.dimensionOf(rhsVariable);
            const std::size_t lhsNode = lhsCoefficient.sign() > 0
                                            ? positiveNode(lhsDimension)
                                            : negativeNode(lhsDimension);
            const std::size_t negativeRhsNode = rhsCoefficient.sign() > 0
                                                    ? negativeNode(rhsDimension)
                                                    : positiveNode(rhsDimension);
            const Rational value = -expression.constant() / lhsMagnitude;
            setCoherent(state, lhsNode, negativeRhsNode,
                        Bound::finite(value, strict));
            return true;
        }
        return false;
    }

    void selfAssign(OctagonState& state, const Environment& environment,
                    Dimension target, int sign,
                    const Rational& constant) const
    {
        normalize(state, environment);
        if (state.bottom)
            return;
        OctagonState old = state;
        OctagonState result(state.variableKinds);

        auto oldNode = [target, sign](std::size_t newNode)
        {
            if (newNode == positiveNode(target))
                return sign > 0 ? positiveNode(target) : negativeNode(target);
            if (newNode == negativeNode(target))
                return sign > 0 ? negativeNode(target) : positiveNode(target);
            return newNode;
        };
        auto delta = [target, &constant](std::size_t newNode)
        {
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
                    const Bound candidate =
                        Bound::add(state.at(row, middle),
                                   state.at(middle, column));
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
        for (Dimension dimension = 0; dimension < state.dimensions;
             ++dimension)
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
                const Bound candidate = Bound::divideByPositive(
                    Bound::add(lhs, rhs), Rational(2));
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

    void normalize(OctagonState& state,
                   const Environment& environment) const
    {
        (void)environment;
        normalize(state);
    }

    OctagonState normalized(const OctagonState& state) const
    {
        OctagonState result = state;
        normalize(result);
        return result;
    }

    void addNodeTerm(LinearExpression& expression,
                     const Environment& environment, std::size_t node,
                     const Rational& multiplier) const
    {
        const Dimension dimension = node / 2;
        const Rational sign = node % 2 == 0 ? Rational(1) : Rational(-1);
        const Variable variable = environment.variableOf(dimension);
        expression.setCoefficient(
            variable, expression.coefficient(variable) + sign * multiplier);
    }

    OctagonOptions options_;
};

} // namespace

std::shared_ptr<DomainBackend>
relational::makeOctagonBackend(const OctagonOptions& options)
{
    return std::make_shared<OctagonBackend>(options);
}
