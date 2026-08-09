//===- Z3SoundnessChecker.h -- Test-only relational soundness -*- C++ -*-===//

#ifndef RELATIONAL_Z3_SOUNDNESS_CHECKER_H
#define RELATIONAL_Z3_SOUNDNESS_CHECKER_H

#include "AE/Core/OctagonDomain.h"

#include <z3++.h>

#include <map>
#include <string>

namespace SVF::test
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

    ProofResult implies(const OctagonState& premise,
                        const OctagonState& conclusion,
                        const std::string& obligation);
    ProofResult checkJoin(const OctagonState& lhs, const OctagonState& rhs,
                          const OctagonState& result);
    ProofResult checkMeet(const OctagonState& lhs, const OctagonState& rhs,
                          const OctagonState& result);
    ProofResult checkAssume(const OctagonState& before,
                            const LinearConstraint& assumption,
                            const OctagonState& result);
    ProofResult checkAssignment(const OctagonState& before, Variable target,
                                const LinearExpression& expression,
                                const OctagonState& result);
    ProofResult checkForget(const OctagonState& before, Variable forgotten,
                            const OctagonState& result);
    ProofResult checkWidening(const OctagonState& current,
                             const OctagonState& next,
                             const OctagonState& result);
    ProofResult checkNarrowing(const OctagonState& current,
                              const OctagonState& next,
                              const OctagonState& result);
    ProofResult checkProjection(const OctagonState& source,
                                const OctagonState& result);

private:
    z3::expr variable(Variable variable);
    z3::expr linear(const LinearExpression& expression);
    z3::expr constraint(const LinearConstraint& constraint);
    z3::expr state(const OctagonState& state);
    ProofResult prove(const z3::expr& premise, const z3::expr& conclusion,
                      const std::string& obligation);
    void requireEnvironment(const OctagonState& state) const;

    Environment environment_;
    z3::context context_;
    std::map<Variable, z3::expr> variables_;
};

} // namespace SVF::test

#endif // RELATIONAL_Z3_SOUNDNESS_CHECKER_H
