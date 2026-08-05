//===- SVFRelationalBridgeTest.cpp -- Thin integration-boundary tests ----===//

#include "AE/Core/SVFRelationalBridge.h"
#include "Z3SoundnessChecker.h"

#include <cstdlib>
#include <iostream>
#include <stdexcept>

using namespace relational;
using namespace relational::test;
using namespace SVF;

int main()
{
    try
    {
        const auto domain = makeOctagonDomain();
        SVFRelationalBridge base({{11, NumericType::integer(), "svf_11"},
                                  {22, NumericType::integer(), "svf_22"}},
                                 domain);
        base.assignConstant(11, Rational(4));
        base.assignAffine(22, {{11, Rational(1)}}, Rational(2));
        if (base.bound(22).lower().value() != Rational(6) ||
            base.bound(22).upper().value() != Rational(6))
            throw std::runtime_error("bridge lost y := x + 2");
        if (!base.projectInterval(22).equals(IntervalValue((s64_t)6)))
            throw std::runtime_error(
                "bridge did not project an exact relational singleton");

        base.assignInterval(22, IntervalValue((s64_t)-3, (s64_t)8));
        if (base.bound(22).lower().value() != Rational(-3) ||
            base.bound(22).upper().value() != Rational(8))
            throw std::runtime_error("bridge interval reduction failed");
        if (!base.projectInterval(22).equals(
                IntervalValue((s64_t)-3, (s64_t)8)))
            throw std::runtime_error(
                "bridge interval projection changed finite endpoints");
        base.changeTrackedVariables({{11, NumericType::integer(), "svf_11"}});
        if (base.tracks(22) || base.bound(11).lower().value() != Rational(4))
            throw std::runtime_error("bridge environment projection failed");

        base.changeTrackedVariables({{11, NumericType::integer(), "svf_11"},
                                     {22, NumericType::integer(), "svf_22"}});
        const AbstractState beforeAffineAssignment = base.state();
        base.assignAffine(22, {{11, Rational(1)}}, Rational(2));

        Z3SoundnessChecker checker(base.environment());
        LinearExpression xPlusTwo(Variable(11));
        xPlusTwo.setConstant(Rational(2));
        const ProofResult assignmentProof = checker.checkAssignment(
            beforeAffineAssignment, Variable(22), xPlusTwo, base.state());
        if (!assignmentProof.proved)
            throw std::runtime_error(assignmentProof.detail);

        SVFRelationalBridge assumed = base;
        const AbstractState beforeAssume = assumed.state();
        assumed.assumeAffine({{11, Rational(1)}, {22, Rational(-1)}},
                             Rational(2), ConstraintKind::LessEqual);
        LinearExpression branchExpression(Variable(11));
        branchExpression.setCoefficient(Variable(22), Rational(-1));
        branchExpression.setConstant(Rational(2));
        const LinearConstraint branchConstraint(branchExpression,
                                                ConstraintKind::LessEqual);
        const ProofResult assumeProof = checker.checkAssume(
            beforeAssume, branchConstraint, assumed.state());
        if (!assumeProof.proved)
            throw std::runtime_error(assumeProof.detail);

        SVFRelationalBridge reduced(
            {{11, NumericType::integer(), "svf_11"},
             {22, NumericType::integer(), "svf_22"}},
            domain);
        reduced.assignInterval(11,
                               IntervalValue((s64_t)0, (s64_t)10));
        reduced.assignAffine(22, {{11, Rational(1)}}, Rational(2));
        reduced.assumeAffine({{22, Rational(1)}}, Rational(-5),
                             ConstraintKind::LessEqual);
        if (!reduced.projectInterval(11).equals(
                IntervalValue((s64_t)0, (s64_t)3)) ||
            !reduced.projectInterval(22).equals(
                IntervalValue((s64_t)2, (s64_t)5)))
            throw std::runtime_error(
                "relational bounds did not reduce the AE intervals");

        SVFRelationalBridge strictInteger(
            {{11, NumericType::integer(), "svf_11"}}, domain);
        strictInteger.assumeAffine({{11, Rational(1)}}, Rational("-3/2"),
                                   ConstraintKind::LessThan);
        if (!strictInteger.projectInterval(11).equals(IntervalValue(
                IntervalValue::minus_infinity(), BoundedInt(1))))
            throw std::runtime_error(
                "strict rational bounds were not rounded for integer AE");

        SVFRelationalBridge infeasible(
            {{11, NumericType::integer(), "svf_11"}}, domain);
        infeasible.assignConstant(11, Rational(1));
        infeasible.assumeAffine({{11, Rational(1)}}, Rational(-1),
                                ConstraintKind::LessThan);
        if (!infeasible.isBottom() ||
            !infeasible.projectInterval(11).isBottom())
            throw std::runtime_error(
                "relational bottom did not project to AE bottom");

        SVFRelationalBridge outsideSigned64(
            {{11, NumericType::integer(), "svf_11"}}, domain);
        outsideSigned64.assignConstant(11,
                                       Rational("9223372036854775808"));
        if (!outsideSigned64.projectInterval(11).isTop())
            throw std::runtime_error(
                "unrepresentable GMP endpoints must project conservatively");

        SVFRelationalBridge branch = base;
        branch.assignConstant(11, Rational(5));
        branch.assignAffine(22, {{11, Rational(1)}}, Rational(2));
        SVFRelationalBridge joined = base;
        joined.joinWith(branch);

        const ProofResult proof =
            checker.checkJoin(base.state(), branch.state(), joined.state());
        if (!proof.proved)
            throw std::runtime_error(proof.detail);

        SVFRelationalBridge firstRange({{11, NumericType::integer(), "svf_11"},
                                        {22, NumericType::integer(), "svf_22"}},
                                       domain);
        firstRange.assignInterval(11, IntervalValue((s64_t)0, (s64_t)1));
        firstRange.assignAffine(22, {{11, Rational(1)}}, Rational(1));
        SVFRelationalBridge secondRange(
            {{11, NumericType::integer(), "svf_11"},
             {22, NumericType::integer(), "svf_22"}},
            domain);
        secondRange.assignInterval(11, IntervalValue((s64_t)0, (s64_t)2));
        secondRange.assignAffine(22, {{11, Rational(1)}}, Rational(1));
        SVFRelationalBridge widened = firstRange;
        widened.widenWith(secondRange);
        const ProofResult wideningProof = checker.checkWidening(
            firstRange.state(), secondRange.state(), widened.state());
        if (!wideningProof.proved)
            throw std::runtime_error(wideningProof.detail);
        SVFRelationalBridge narrowed = widened;
        narrowed.narrowWith(secondRange);
        const ProofResult narrowingProof = checker.checkNarrowing(
            widened.state(), secondRange.state(), narrowed.state());
        if (!narrowingProof.proved)
            throw std::runtime_error(narrowingProof.detail);

        bool rejected = false;
        try
        {
            joined.forget(999);
        }
        catch (const std::invalid_argument&)
        {
            rejected = true;
        }
        if (!rejected)
            throw std::runtime_error("bridge accepted an untracked NodeID");

        std::cout << "svf-relational-bridge tests: PASS\n";
        return EXIT_SUCCESS;
    }
    catch (const std::exception& error)
    {
        std::cerr << "svf-relational-bridge tests: FAIL: " << error.what()
                  << '\n';
        return EXIT_FAILURE;
    }
}
