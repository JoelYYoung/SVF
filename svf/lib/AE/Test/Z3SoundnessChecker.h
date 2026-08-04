//===- Z3SoundnessChecker.h -- Test-only relational soundness -*- C++ -*-===//

#ifndef RELATIONAL_Z3_SOUNDNESS_CHECKER_H
#define RELATIONAL_Z3_SOUNDNESS_CHECKER_H

#include "AE/Core/RelationalDomain.h"

#include <z3++.h>

#include <map>
#include <string>

namespace relational::test
{

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
    explicit Z3SoundnessChecker(const Environment& environment);

    ProofResult implies(const AbstractState& premise,
                        const AbstractState& conclusion,
                        const std::string& obligation);
    ProofResult checkJoin(const AbstractState& lhs, const AbstractState& rhs,
                          const AbstractState& result);
    ProofResult checkMeet(const AbstractState& lhs, const AbstractState& rhs,
                          const AbstractState& result);
    ProofResult checkAssume(const AbstractState& before,
                            const LinearConstraint& assumption,
                            const AbstractState& result);
    ProofResult checkAssignment(const AbstractState& before, Variable target,
                                const LinearExpression& expression,
                                const AbstractState& result);
    ProofResult checkForget(const AbstractState& before, Variable forgotten,
                            const AbstractState& result);
    ProofResult checkWidening(const AbstractState& current,
                             const AbstractState& next,
                             const AbstractState& result);
    ProofResult checkNarrowing(const AbstractState& current,
                              const AbstractState& next,
                              const AbstractState& result);
    ProofResult checkProjection(const AbstractState& source,
                                const AbstractState& result);

private:
    z3::expr variable(Variable variable);
    z3::expr linear(const LinearExpression& expression);
    z3::expr constraint(const LinearConstraint& constraint);
    z3::expr state(const AbstractState& state);
    ProofResult prove(const z3::expr& premise, const z3::expr& conclusion,
                      const std::string& obligation);
    void requireEnvironment(const AbstractState& state) const;

    Environment environment_;
    z3::context context_;
    std::map<Variable, z3::expr> variables_;
};

} // namespace relational::test

#endif // RELATIONAL_Z3_SOUNDNESS_CHECKER_H
