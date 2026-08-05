//===- RelationalDomainTest.cpp -- Native relational-domain tests --------===//

#include "AE/Core/OctagonDomain.h"
#include "Z3SoundnessChecker.h"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <type_traits>

using namespace relational;
using namespace relational::test;

namespace
{

void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

void requireProof(const ProofResult& proof)
{
    require(proof.proved, proof.detail);
}

LinearConstraint atMost(Variable variable, const Rational& value)
{
    LinearExpression expression(variable);
    expression.setConstant(-value);
    return LinearConstraint(std::move(expression), ConstraintKind::LessEqual);
}

LinearConstraint below(Variable variable, const Rational& value)
{
    LinearExpression expression(variable);
    expression.setConstant(-value);
    return LinearConstraint(std::move(expression), ConstraintKind::LessThan);
}

LinearConstraint atLeast(Variable variable, const Rational& value)
{
    LinearExpression expression(variable);
    expression.setConstant(-value);
    return LinearConstraint(std::move(expression),
                            ConstraintKind::GreaterEqual);
}

LinearConstraint equalsConstant(Variable variable, const Rational& value)
{
    LinearExpression expression(variable);
    expression.setConstant(-value);
    return LinearConstraint(std::move(expression), ConstraintKind::Equal);
}

LinearConstraint differenceEquals(Variable lhs, Variable rhs,
                                  const Rational& value)
{
    LinearExpression expression(lhs);
    expression.setCoefficient(rhs, Rational(-1));
    expression.setConstant(-value);
    return LinearConstraint(std::move(expression), ConstraintKind::Equal);
}

void testExactNumericLayer()
{
    const Rational oneThird("1/3");
    require(oneThird + oneThird + oneThird == Rational(1),
            "GMP rational arithmetic must be exact");

    const Rational halfFloatUlp("1/16777216");
    require(FloatSemantics::add(Rational(1), halfFloatUlp, 24,
                                RoundingMode::NearestTiesToEven) == Rational(1),
            "binary32 tie must round to even");
    require(FloatSemantics::add(Rational(1), halfFloatUlp, 53,
                                RoundingMode::NearestTiesToEven) ==
                Rational("16777217/16777216"),
            "the same increment must remain exact at binary64 precision");
}

void testEnvironment()
{
    const Variable x(7);
    const Variable y(2);
    const Environment environment(
        {{x, NumericType::integer(), "x"}, {y, NumericType::real(), "y"}});
    require(environment.variableOf(0) == y && environment.variableOf(1) == x,
            "environment dimensions must be deterministic by variable id");
    require(environment.typeOf(y).kind == NumericKind::Real,
            "environment must preserve numeric types");
}

void testPublicDomainArchitecture()
{
    static_assert(std::is_base_of_v<AbstractDomain, OctagonDomain>);

    const Variable x(1);
    const Variable y(2);
    const Environment environment(
        {{x, NumericType::real(), "x"}, {y, NumericType::real(), "y"}});
    std::shared_ptr<AbstractDomain> domain = makeOctagonDomain();
    require(std::string(domain->name()) == "gmp-octagon",
            "OctagonDomain must be usable through AbstractDomain");

    LinearExpression lhs(x);
    lhs.setCoefficient(y, Rational(-1));
    const LinearConstraint constraint =
        lessEqual(lhs, LinearExpression(Rational(3)));
    require(constraint.kind() == ConstraintKind::LessEqual &&
                constraint.expression().coefficient(x) == Rational(1) &&
                constraint.expression().coefficient(y) == Rational(-1) &&
                constraint.expression().constant() == Rational(-3),
            "structured constraint construction must normalize lhs <= rhs "
            "to lhs - rhs <= 0");

    AbstractState state =
        domain->fromConstraints(environment, LinearConstraintSet{constraint});
    require(state.entails(constraint) == CheckResult::True,
            "AbstractDomain must consume structured linear constraints");
}

void testAssumeClosureAndAssignment()
{
    const Variable x(1);
    const Variable y(2);
    const Environment environment(
        {{x, NumericType::integer(), "x"}, {y, NumericType::integer(), "y"}});
    const auto domain = makeOctagonDomain();
    Z3SoundnessChecker checker(environment);

    AbstractState state = domain->top(environment);
    const LinearConstraint nonnegative = atLeast(x, Rational(0));
    AbstractState beforeAssume = state;
    state.assume(nonnegative);
    requireProof(checker.checkAssume(beforeAssume, nonnegative, state));
    state.assume(atMost(x, Rational(10)));
    state.assume(differenceEquals(x, y, Rational(0)));
    require(state.bound(y).lower().value() == Rational(0) &&
                state.bound(y).upper().value() == Rational(10),
            "strong closure must propagate x bounds through x == y");
    require(state.entails(differenceEquals(x, y, Rational(0))) ==
                CheckResult::True,
            "the state must entail an explicitly assumed equality");

    AbstractState assigned = domain->top(environment);
    assigned.assume(equalsConstant(x, Rational(2)));
    LinearExpression xPlusThree(x);
    xPlusThree.setConstant(Rational(3));
    AbstractState beforeAssignment = assigned;
    assigned.assign(y, xPlusThree);
    requireProof(
        checker.checkAssignment(beforeAssignment, y, xPlusThree, assigned));
    require(assigned.bound(y).lower().value() == Rational(5) &&
                assigned.bound(y).upper().value() == Rational(5),
            "y := x + 3 must preserve an exact octagonal relation");

    LinearExpression selfIncrement(x);
    selfIncrement.setConstant(Rational(1));
    assigned.assign(x, selfIncrement);
    require(assigned.bound(x).lower().value() == Rational(3) &&
                assigned.bound(x).upper().value() == Rational(3),
            "x := x + 1 must transform old x relations exactly");

    LinearExpression selfNegate(x);
    selfNegate *= Rational(-1);
    selfNegate.setConstant(Rational(2));
    beforeAssignment = assigned;
    assigned.assign(x, selfNegate);
    requireProof(
        checker.checkAssignment(beforeAssignment, x, selfNegate, assigned));
    require(assigned.bound(x).lower().value() == Rational(-1) &&
                assigned.bound(x).upper().value() == Rational(-1),
            "x := -x + 2 must transform signed target nodes exactly");

    LinearExpression unsupported(x);
    unsupported *= Rational(2);
    beforeAssignment = assigned;
    assigned.assign(y, unsupported);
    requireProof(
        checker.checkAssignment(beforeAssignment, y, unsupported, assigned));
    requireProof(checker.checkForget(beforeAssignment, y, assigned));
    require(assigned.bound(y).isTop(),
            "unsupported y := 2*x must soundly forget y");
    require(assigned.bound(x).lower().value() == Rational(-1),
            "forgetting y must retain independent x information");
}

void testStrictIntegerAndRealBounds()
{
    const Variable integer(1);
    const Environment integers(
        {{integer, NumericType::integer(), "integer_value"}});
    AbstractState integerState = makeOctagonDomain()->top(integers);
    integerState.assume(below(integer, Rational("3/2")));
    const Interval integerBound = integerState.bound(integer);
    require(integerBound.upper().value() == Rational(1) &&
                !integerBound.upper().isStrict(),
            "integer tightening must turn x < 3/2 into x <= 1");

    const Variable real(2);
    const Environment reals({{real, NumericType::real(), "real_value"}});
    AbstractState realState = makeOctagonDomain()->top(reals);
    realState.assume(below(real, Rational("3/2")));
    const Interval realBound = realState.bound(real);
    require(realBound.upper().value() == Rational("3/2") &&
                realBound.upper().isStrict(),
            "real strict bounds must remain strict exact rationals");
    realState.assume(atLeast(real, Rational("3/2")));
    require(realState.isBottom(), "x < 3/2 and x >= 3/2 must close to bottom");
}

void testEnvironmentChanges()
{
    const Variable x(1);
    const Variable y(2);
    const Variable z(3);
    const Environment original(
        {{x, NumericType::integer(), "x"}, {y, NumericType::integer(), "y"}});
    const Environment changed(
        {{x, NumericType::integer(), "x"}, {z, NumericType::integer(), "z"}});
    const auto domain = makeOctagonDomain();
    AbstractState state = domain->top(original);
    state.assume(equalsConstant(x, Rational(7)));
    state.assume(equalsConstant(y, Rational(9)));

    AbstractState unconstrained = state.changedEnvironment(changed);
    require(unconstrained.bound(x).lower().value() == Rational(7) &&
                unconstrained.bound(z).isTop(),
            "environment change must project removed variables and add top "
            "dimensions");

    AbstractState projected = state.changedEnvironment(changed, true);
    require(projected.bound(z).lower().value() == Rational(0) &&
                projected.bound(z).upper().value() == Rational(0),
            "projected new environment dimensions must be initialized to zero");
}

void testLatticeAndZ3Soundness()
{
    const Variable x(1);
    const Variable y(2);
    const Environment environment(
        {{x, NumericType::integer(), "x"}, {y, NumericType::integer(), "y"}});
    const auto domain = makeOctagonDomain();
    Z3SoundnessChecker checker(environment);

    AbstractState zero = domain->top(environment);
    zero.assume(equalsConstant(x, Rational(0)));
    zero.assume(equalsConstant(y, Rational(0)));
    AbstractState one = domain->top(environment);
    one.assume(equalsConstant(x, Rational(1)));
    one.assume(equalsConstant(y, Rational(1)));

    AbstractState joined = zero.joined(one);
    requireProof(checker.checkJoin(zero, one, joined));
    require(joined.entails(differenceEquals(x, y, Rational(0))) ==
                CheckResult::True,
            "octagon join must preserve the common x == y relation");

    AbstractState xRange = domain->top(environment);
    xRange.assume(atLeast(x, Rational(0)));
    xRange.assume(atMost(x, Rational(2)));
    AbstractState equality = domain->top(environment);
    equality.assume(differenceEquals(x, y, Rational(0)));
    AbstractState met = xRange.met(equality);
    requireProof(checker.checkMeet(xRange, equality, met));

    AbstractState current = domain->top(environment);
    current.assume(atLeast(x, Rational(0)));
    current.assume(atMost(x, Rational(1)));
    AbstractState next = domain->top(environment);
    next.assume(atLeast(x, Rational(0)));
    next.assume(atMost(x, Rational(2)));
    AbstractState widened = current.widened(next);
    requireProof(checker.checkWidening(current, next, widened));
    require(widened.bound(x).lower().value() == Rational(0) &&
                widened.bound(x).upper().isPlusInfinity(),
            "widening must retain the stable lower bound and drop a growing "
            "upper bound");

    AbstractState thresholdWidened =
        current.widened(next, WideningPolicy{{Rational(10)}});
    require(thresholdWidened.bound(x).upper().value() == Rational(10),
            "threshold widening must use user-visible unary constants");
    requireProof(checker.checkWidening(current, next, thresholdWidened));

    AbstractState narrowed = widened.narrowed(next);
    requireProof(checker.checkNarrowing(widened, next, narrowed));
    require(narrowed.bound(x).upper().value() == Rational(2),
            "narrowing must recover a finite bound lost by widening");

    AbstractState projected = next.projectLowerBounds();
    requireProof(checker.checkProjection(next, projected));
    require(projected.bound(x).lower().value() == Rational(0) &&
                projected.bound(x).upper().isPlusInfinity(),
            "lower projection must retain lower and discard upper bounds");
}

} // namespace

int main()
{
    try
    {
        testExactNumericLayer();
        testEnvironment();
        testPublicDomainArchitecture();
        testAssumeClosureAndAssignment();
        testStrictIntegerAndRealBounds();
        testEnvironmentChanges();
        testLatticeAndZ3Soundness();
        std::cout << "relational-domain tests: PASS\n";
        return EXIT_SUCCESS;
    }
    catch (const std::exception& error)
    {
        std::cerr << "relational-domain tests: FAIL: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
