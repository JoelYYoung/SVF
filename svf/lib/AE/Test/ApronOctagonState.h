//===- ApronOctagonState.h -- Benchmark-only APRON adapter -----*- C++ -*-===//

#ifndef SVF_AE_TEST_APRON_OCTAGON_STATE_H
#define SVF_AE_TEST_APRON_OCTAGON_STATE_H

#include "AE/Core/OctagonDomain.h"

extern "C"
{
#include "ap_abstract1.h"
}

namespace SVF::AbstractDomain
{

/// Benchmark-only NumericalState adapter for APRON octMPQ.  It lets the
/// production DenseAbstractInterpretation execute exactly the same program
/// workload with APRON without making APRON a production dependency.
class ApronOctagonState final : public NumericalState
{
public:
    using NumericalState::bound;
    using NumericalState::substitute;
    using NumericalState::substituteParallel;

    static ApronOctagonState top(const VariableEnvironment& environment);
    static ApronOctagonState bottom(const VariableEnvironment& environment);

    ApronOctagonState(const ApronOctagonState& other);
    ApronOctagonState(ApronOctagonState&& other) noexcept;
    ApronOctagonState& operator=(ApronOctagonState other) noexcept;
    ~ApronOctagonState() override;

    void swap(ApronOctagonState& other) noexcept;
    std::unique_ptr<AbstractState> clone() const override;
    const char* name() const override;
    DomainCapabilities capabilities() const override;
    const VariableEnvironment& environment() const override;
    const OctagonConfig& config() const;
    std::uint64_t hash() const;

    void assign(Variable target, const LinearExpression& expression) override;
    void assign(Variable target, const TreeExpression& expression) override;
    void assignParallel(const LinearAssignmentList& assignments) override;
    void substitute(Variable target,
                    const LinearExpression& expression) override;
    void substituteParallel(
        const LinearAssignmentList& assignments) override;
    void assume(const LinearConstraint& constraint) override;
    void assume(const TreeConstraint& constraint) override;
    void assumeAll(const LinearConstraintSet& constraints) override;
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
    void close() override;
    void canonicalize() override;

private:
    ApronOctagonState(VariableEnvironment environment, ap_abstract1_t value);

    const void* dynamicTypeToken() const noexcept override
    {
        return staticTypeToken<ApronOctagonState>();
    }
    void assignInterval(Variable target, const Interval& value) override;
    bool hasCompatibleDomain(const AbstractState& other) const override;
    void joinState(const AbstractState& other) override;
    void meetState(const AbstractState& other) override;
    void widenState(const AbstractState& next) override;
    void narrowState(const AbstractState& next) override;
    bool isBottomState() const override;
    bool isTopState() const override;
    bool leqState(const AbstractState& other) const override;
    std::string stateToString() const override;

    const ApronOctagonState& requireApron(const AbstractState& other) const;
    void replace(ap_abstract1_t value);

    VariableEnvironment environment_;
    ap_abstract1_t value_{nullptr, nullptr};
};

} // namespace SVF::AbstractDomain

#endif // SVF_AE_TEST_APRON_OCTAGON_STATE_H
