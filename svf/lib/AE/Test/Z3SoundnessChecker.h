//===- Z3SoundnessChecker.h -- Test-only relational soundness -*- C++ -*-===//

#ifndef RELATIONAL_Z3_SOUNDNESS_CHECKER_H
#define RELATIONAL_Z3_SOUNDNESS_CHECKER_H

#include "AE/Core/NumericalDomain.h"

#include <z3++.h>

#include <map>
#include <string>

namespace SVF::test
{

using namespace AbstractDomain;

struct ProofResult
{
    bool proved = false;
    std::string detail;
};

/// Independent, test-only oracle.  It translates exported abstract-state
/// constraints to Z3 and asks whether a concrete counterexample exists.
class Z3SoundnessChecker
{
public:
    explicit Z3SoundnessChecker(const VariableEnvironment& environment);

    ProofResult implies(const NumericalState& premise,
                        const NumericalState& conclusion,
                        const std::string& obligation);
    ProofResult checkJoin(const NumericalState& lhs,
                          const NumericalState& rhs,
                          const NumericalState& result);
    ProofResult checkMeet(const NumericalState& lhs,
                          const NumericalState& rhs,
                          const NumericalState& result);
    ProofResult checkAssume(const NumericalState& before,
                            const LinearConstraint& assumption,
                            const NumericalState& result);
    ProofResult checkAssignment(const NumericalState& before, Variable target,
                                const LinearExpression& expression,
                                const NumericalState& result);
    ProofResult checkForget(const NumericalState& before, Variable forgotten,
                            const NumericalState& result);
    ProofResult checkWidening(const NumericalState& current,
                             const NumericalState& next,
                             const NumericalState& result);
    ProofResult checkNarrowing(const NumericalState& current,
                              const NumericalState& next,
                              const NumericalState& result);
    ProofResult checkProjection(const NumericalState& source,
                                const NumericalState& result);

private:
    z3::expr variable(Variable variable);
    z3::expr linear(const LinearExpression& expression);
    z3::expr constraint(const LinearConstraint& constraint);
    z3::expr state(const NumericalState& state);
    ProofResult prove(const z3::expr& premise, const z3::expr& conclusion,
                      const std::string& obligation);
    void requireEnvironment(const NumericalState& state) const;

    VariableEnvironment environment_;
    z3::context context_;
    std::map<Variable, z3::expr> variables_;
};

} // namespace SVF::test

#endif // RELATIONAL_Z3_SOUNDNESS_CHECKER_H
