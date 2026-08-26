//===- ConvexPolyhedraDomain.h -- Exact rational polyhedra ----*- C++ -*-===//

#ifndef SVF_AE_CONVEX_POLYHEDRA_DOMAIN_H
#define SVF_AE_CONVEX_POLYHEDRA_DOMAIN_H

#include "AE/Core/AbstractState.h"
#include "AE/Core/NumericalDomain.h"

#include <memory>
#include <vector>

namespace SVF::AbstractDomain
{

struct ConvexPolyhedraConfig
{
    std::shared_ptr<DiagnosticSink> diagnostics;
    bool integerTightening = true;

    bool operationCompatible(const ConvexPolyhedraConfig& other) const
    {
        return integerTightening == other.integerTightening;
    }
};

enum class PolyhedraGeneratorKind
{
    Point,
    ClosurePoint,
    Ray,
    Line
};

/// Public, environment-ordered V-representation element. Point coordinates
/// are affine values; Ray/Line coordinates are directions. A closure point is
/// available only in an NNC generator system. An empty system denotes bottom.
struct PolyhedraGenerator
{
    PolyhedraGeneratorKind kind = PolyhedraGeneratorKind::Point;
    std::vector<Rational> coordinates;
};

using PolyhedraGeneratorSet = std::vector<PolyhedraGenerator>;

/// Closed/non-closed convex polyhedra over exact GMP rationals. The internal
/// representation lazily caches normalized constraints (H) and homogeneous
/// points/rays/lines (V), converting with an exact double-description kernel.
/// Operations select the cheaper valid side without exposing H/V state to the
/// abstract interpreter; exact Fourier-Motzkin remains the NNC projection
/// fallback.
class ConvexPolyhedraState final : public NumericalState
{
public:
    using NumericalState::assignParallel;
    using NumericalState::bound;
    using NumericalState::substitute;
    using NumericalState::substituteParallel;

    static ConvexPolyhedraState top(
        const VariableEnvironment& environment,
        const ConvexPolyhedraConfig& config = {});
    static ConvexPolyhedraState bottom(
        const VariableEnvironment& environment,
        const ConvexPolyhedraConfig& config = {});
    static ConvexPolyhedraState fromBox(
        const VariableEnvironment& environment, const IntervalBox& box,
        const ConvexPolyhedraConfig& config = {});
    static ConvexPolyhedraState fromConstraints(
        const VariableEnvironment& environment,
        const LinearConstraintSet& constraints,
        const ConvexPolyhedraConfig& config = {});
    static ConvexPolyhedraState fromGenerators(
        const VariableEnvironment& environment,
        const PolyhedraGeneratorSet& generators,
        const ConvexPolyhedraConfig& config = {});

    ConvexPolyhedraState(const ConvexPolyhedraState& other);
    ConvexPolyhedraState(ConvexPolyhedraState&& other) noexcept;
    ConvexPolyhedraState& operator=(const ConvexPolyhedraState& other);
    ConvexPolyhedraState& operator=(ConvexPolyhedraState&& other) noexcept;
    ~ConvexPolyhedraState() override;

    std::unique_ptr<AbstractState> clone() const override;
    const char* name() const override;
    DomainCapabilities capabilities() const override;

    const VariableEnvironment& environment() const override
    {
        return environment_;
    }
    const ConvexPolyhedraConfig& config() const
    {
        return config_;
    }

    void assign(Variable target,
                const LinearExpression& expression) override;
    void assign(Variable target, const TreeExpression& expression) override;
    void assignParallel(const LinearAssignmentList& assignments) override;
    void substitute(Variable target,
                    const LinearExpression& expression) override;
    void substituteParallel(
        const LinearAssignmentList& assignments) override;
    void assume(const LinearConstraint& constraint) override;
    void assumeAll(const LinearConstraintSet& constraints) override;
    void assume(const TreeConstraint& constraint) override;
    void forget(Variable variable) override;
    void changeEnvironment(const VariableEnvironment& environment,
                           bool initializeNewVariablesToZero = false) override;
    void expand(Variable source,
                const std::vector<VariableDeclaration>& copies) override;
    void fold(Variable target,
              const std::vector<Variable>& folded) override;

    CheckResult entails(const LinearConstraint& constraint) const override;
    Interval bound(Variable variable) const override;
    Interval bound(const LinearExpression& expression) const override;
    IntervalBox toBox() const override;
    LinearConstraintSet toConstraints() const override;
    PolyhedraGeneratorSet toGenerators() const;
    void close() override;
    void canonicalize() override;

    ConvexPolyhedraState join(
        const ConvexPolyhedraState& other) const;
    ConvexPolyhedraState meet(
        const ConvexPolyhedraState& other) const;
    ConvexPolyhedraState widen(
        const ConvexPolyhedraState& next,
        const WideningPolicy& policy = {}) const;
    ConvexPolyhedraState narrow(
        const ConvexPolyhedraState& next) const;

private:
    class Impl;
    ConvexPolyhedraState(VariableEnvironment environment,
                         ConvexPolyhedraConfig config, bool bottom);

    const void* dynamicTypeToken() const noexcept override
    {
        return staticTypeToken<ConvexPolyhedraState>();
    }
    bool hasCompatibleDomain(const AbstractState& other) const override;
    void joinState(const AbstractState& other) override;
    void meetState(const AbstractState& other) override;
    void widenState(const AbstractState& next) override;
    void narrowState(const AbstractState& next) override;
    bool isBottomState() const override;
    bool isTopState() const override;
    bool leqState(const AbstractState& other) const override;
    std::string stateToString() const override;

    const ConvexPolyhedraState& requirePolyhedron(
        const AbstractState& other) const;
    void ensureConstraints() const;
    void ensureGenerators() const;
    void invalidateConstraints();
    void invalidateGenerators();
    void normalize();
    void report(OperationKind operation, ApproximationKind approximation,
                std::string reason, bool best = true) const;

    VariableEnvironment environment_;
    ConvexPolyhedraConfig config_;
    std::unique_ptr<Impl> impl_;
};

} // namespace SVF::AbstractDomain

#endif // SVF_AE_CONVEX_POLYHEDRA_DOMAIN_H
