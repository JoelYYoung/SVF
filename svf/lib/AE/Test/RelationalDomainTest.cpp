//===- RelationalDomainTest.cpp -- Native relational-domain tests --------===//

#include "AE/Core/OctagonDomain.h"
#include "Z3SoundnessChecker.h"

#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

using namespace SVF;
using namespace SVF::AbstractDomain;
using namespace SVF::test;

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

template <typename Action>
void requireThrows(Action&& action, const std::string& message)
{
    try
    {
        std::forward<Action>(action)();
    }
    catch (const std::exception&)
    {
        return;
    }
    throw std::runtime_error(message);
}

class RecordingDiagnosticSink final : public DiagnosticSink
{
public:
    void report(const Diagnostic& diagnostic) override
    {
        diagnostics.push_back(diagnostic);
    }

    std::vector<Diagnostic> diagnostics;
};

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

LinearConstraint signedSumAtMost(Variable lhs, int lhsSign, Variable rhs,
                                 int rhsSign, const Rational& value)
{
    LinearExpression expression;
    expression.setCoefficient(lhs, Rational(lhsSign));
    expression.setCoefficient(rhs, Rational(rhsSign));
    expression.setConstant(-value);
    return LinearConstraint(std::move(expression), ConstraintKind::LessEqual);
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

void testPolymorphicStateContract()
{
    const Variable x(1);
    const Variable y(2);
    const VariableEnvironment environment({{x, NumericType::integer(), "x"}});
    const VariableEnvironment extended(
        {{x, NumericType::integer(), "x"},
         {y, NumericType::integer(), "y"}});

    OctagonState current = OctagonState::top(environment);
    current.assume(atLeast(x, Rational(0)));
    current.assume(atMost(x, Rational(1)));
    OctagonState next = OctagonState::top(environment);
    next.assume(atLeast(x, Rational(0)));
    next.assume(atMost(x, Rational(2)));

    const AbstractState& genericCurrent = current;
    const AbstractState& genericNext = next;
    std::unique_ptr<AbstractState> cloned = genericCurrent.clone();
    require(cloned->isEquivalentTo(genericCurrent) == CheckResult::True,
            "polymorphic clone must preserve the state");

    std::unique_ptr<AbstractState> joined = genericCurrent.clone();
    joined->joinWith(genericNext);
    require(joined->isEquivalentTo(genericNext) == CheckResult::True,
            "polymorphic join must dispatch to Octagon");
    std::unique_ptr<AbstractState> met = genericCurrent.clone();
    met->meetWith(genericNext);
    require(met->isEquivalentTo(genericCurrent) == CheckResult::True,
            "polymorphic meet must dispatch to Octagon");

    std::unique_ptr<AbstractState> widened = genericCurrent.clone();
    widened->widenWith(genericNext);
    const auto& widen =
        dynamic_cast<const OctagonState&>(*widened);
    require(widen.bound(x).lower().value() == Rational(0) &&
                widen.bound(x).upper().isPlusInfinity(),
            "polymorphic widening must dispatch to Octagon");
    std::unique_ptr<AbstractState> narrowed = widened->clone();
    narrowed->narrowWith(genericNext);
    require(narrowed->isEquivalentTo(genericNext) == CheckResult::True,
            "polymorphic narrowing must dispatch to Octagon");

    OctagonState projected = next.projectedLowerBounds();
    require(projected.bound(x).lower().value() == Rational(0) &&
                projected.bound(x).upper().isPlusInfinity(),
            "Octagon projection must discard upper bounds");
    OctagonState changed = current.withEnvironment(extended);
    require(changed.environment() == extended && changed.bound(y).isTop(),
            "Octagon environment change must introduce y unconstrained");

    OctagonState treeState = OctagonState::top(environment);
    TreeExpression xTree =
        TreeExpression::variable(x, NumericType::integer());
    TreeExpression three =
        TreeExpression::constant(Rational(3), NumericType::integer());
    treeState.assume(TreeConstraint(
        TreeExpression::binary(BinaryOperator::Subtract, xTree, three,
                               NumericType::integer()),
        ConstraintKind::LessEqual));
    require(treeState.bound(x).upper().value() == Rational(3),
            "linear tree assumptions must use the common fallback");

    OctagonState singleton = OctagonState::top(environment);
    singleton.assume(equalsConstant(x, Rational(1)));
    require(singleton.entails(LinearConstraint(
                LinearExpression(x) - LinearExpression(Rational(2)),
                ConstraintKind::NotEqual)) == CheckResult::True,
            "not-equal entailment must use an equality counterexample");
    require(std::string(toString(CheckResult::False)) == "false" &&
                std::string(toString(CheckResult::True)) == "true" &&
                std::string(toString(CheckResult::Unknown)) == "unknown",
            "CheckResult formatting must cover every result");
}

void testEnvironment()
{
    const Variable x(7);
    const Variable y(2);
    const VariableEnvironment environment(
        {{x, NumericType::integer(), "x"}, {y, NumericType::real(), "y"}});
    require(environment.variableOf(0) == y && environment.variableOf(1) == x,
            "environment dimensions must be deterministic by variable id");
    require(environment.typeOf(y).kind == NumericKind::Real,
            "environment must preserve numeric types");
}

void testPublicDomainArchitecture()
{
    static_assert(std::is_base_of_v<AbstractState, OctagonState>);

    const Variable x(1);
    const Variable y(2);
    const VariableEnvironment environment(
        {{x, NumericType::real(), "x"}, {y, NumericType::real(), "y"}});
    OctagonState state = OctagonState::top(environment);
    const AbstractState& abstractState = state;
    require(std::string(abstractState.name()) == "gmp-octagon",
            "OctagonState must be usable through AbstractState");

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

    state = OctagonState::fromConstraints(
        environment, LinearConstraintSet{constraint});
    require(state.entails(constraint) == CheckResult::True,
            "OctagonState must consume structured linear constraints");
}

void testAssumeClosureAndAssignment()
{
    const Variable x(1);
    const Variable y(2);
    const VariableEnvironment environment(
        {{x, NumericType::integer(), "x"}, {y, NumericType::integer(), "y"}});
    Z3SoundnessChecker checker(environment);

    OctagonState state = OctagonState::top(environment);
    const LinearConstraint nonnegative = atLeast(x, Rational(0));
    OctagonState beforeAssume = state;
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

    OctagonState assigned = OctagonState::top(environment);
    assigned.assume(equalsConstant(x, Rational(2)));
    LinearExpression xPlusThree(x);
    xPlusThree.setConstant(Rational(3));
    OctagonState beforeAssignment = assigned;
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

    LinearExpression scaled(x);
    scaled *= Rational(2);
    beforeAssignment = assigned;
    assigned.assign(y, scaled);
    requireProof(
        checker.checkAssignment(beforeAssignment, y, scaled, assigned));
    require(assigned.bound(y).lower().value() == Rational(-2) &&
                assigned.bound(y).upper().value() == Rational(-2),
            "general y := 2*x must recover the strongest interval image");
    require(assigned.bound(x).lower().value() == Rational(-1),
            "a general affine image must retain independent x information");
}

void testStrictIntegerAndRealBounds()
{
    const Variable integer(1);
    const VariableEnvironment integers(
        {{integer, NumericType::integer(), "integer_value"}});
    OctagonState integerState = OctagonState::top(integers);
    integerState.assume(below(integer, Rational("3/2")));
    const Interval integerBound = integerState.bound(integer);
    require(integerBound.upper().value() == Rational(1) &&
                !integerBound.upper().isStrict(),
            "integer tightening must turn x < 3/2 into x <= 1");

    const Variable real(2);
    const VariableEnvironment reals({{real, NumericType::real(), "real_value"}});
    OctagonState realState = OctagonState::top(reals);
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
    const VariableEnvironment original(
        {{x, NumericType::integer(), "x"}, {y, NumericType::integer(), "y"}});
    const VariableEnvironment changed(
        {{x, NumericType::integer(), "x"}, {z, NumericType::integer(), "z"}});
    OctagonState state = OctagonState::top(original);
    state.assume(equalsConstant(x, Rational(7)));
    state.assume(equalsConstant(y, Rational(9)));

    OctagonState unconstrained = state.withEnvironment(changed);
    require(unconstrained.bound(x).lower().value() == Rational(7) &&
                unconstrained.bound(z).isTop(),
            "environment change must project removed variables and add top "
            "dimensions");

    OctagonState projected = state.withEnvironment(changed, true);
    require(projected.bound(z).lower().value() == Rational(0) &&
                projected.bound(z).upper().value() == Rational(0),
            "projected new environment dimensions must be initialized to zero");
}

void testLatticeAndZ3Soundness()
{
    const Variable x(1);
    const Variable y(2);
    const VariableEnvironment environment(
        {{x, NumericType::integer(), "x"}, {y, NumericType::integer(), "y"}});
    Z3SoundnessChecker checker(environment);

    OctagonState zero = OctagonState::top(environment);
    zero.assume(equalsConstant(x, Rational(0)));
    zero.assume(equalsConstant(y, Rational(0)));
    OctagonState one = OctagonState::top(environment);
    one.assume(equalsConstant(x, Rational(1)));
    one.assume(equalsConstant(y, Rational(1)));

    OctagonState joined = zero.join(one);
    requireProof(checker.checkJoin(zero, one, joined));
    require(joined.entails(differenceEquals(x, y, Rational(0))) ==
                CheckResult::True,
            "octagon join must preserve the common x == y relation");

    OctagonState xRange = OctagonState::top(environment);
    xRange.assume(atLeast(x, Rational(0)));
    xRange.assume(atMost(x, Rational(2)));
    OctagonState equality = OctagonState::top(environment);
    equality.assume(differenceEquals(x, y, Rational(0)));
    OctagonState met = xRange.meet(equality);
    requireProof(checker.checkMeet(xRange, equality, met));

    OctagonState current = OctagonState::top(environment);
    current.assume(atLeast(x, Rational(0)));
    current.assume(atMost(x, Rational(1)));
    OctagonState next = OctagonState::top(environment);
    next.assume(atLeast(x, Rational(0)));
    next.assume(atMost(x, Rational(2)));
    OctagonState widened = current.widen(next);
    requireProof(checker.checkWidening(current, next, widened));
    require(widened.bound(x).lower().value() == Rational(0) &&
                widened.bound(x).upper().isPlusInfinity(),
            "widening must retain the stable lower bound and drop a growing "
            "upper bound");

    OctagonState thresholdWidened =
        current.widen(next, WideningPolicy{{Rational(10)}});
    require(thresholdWidened.bound(x).upper().value() == Rational(10),
            "threshold widening must use user-visible unary constants");
    requireProof(checker.checkWidening(current, next, thresholdWidened));

    OctagonState narrowed = widened.narrow(next);
    requireProof(checker.checkNarrowing(widened, next, narrowed));
    require(narrowed.bound(x).upper().value() == Rational(2),
            "narrowing must recover a finite bound lost by widening");

    OctagonState projected = next.projectedLowerBounds();
    requireProof(checker.checkProjection(next, projected));
    require(projected.bound(x).lower().value() == Rational(0) &&
                projected.bound(x).upper().isPlusInfinity(),
            "lower projection must retain lower and discard upper bounds");
}

void testTopBottomIdentitiesAndQueries()
{
    const Variable x(1);
    const Variable y(2);
    const Variable unknown(99);
    const VariableEnvironment environment(
        {{x, NumericType::integer(), "x"}, {y, NumericType::real(), "y"}});
    OctagonState top = OctagonState::top(environment);
    const DomainCapabilities capabilities = top.capabilities();
    require(capabilities.strictInequalities &&
                capabilities.integerTightening &&
                capabilities.thresholdWidening && capabilities.narrowing &&
                capabilities.parallelAssignments &&
                capabilities.expressionBounds &&
                capabilities.backwardAssignments &&
                capabilities.topologicalClosure &&
                capabilities.canonicalization &&
                !capabilities.nonlinearTreeExpressions,
            "Octagon capabilities must describe the enabled implementation");

    OctagonState bottom = OctagonState::bottom(environment);
    require(top.isTop() && !top.isBottom() && top.toString() == "top",
            "top queries must agree");
    require(bottom.isBottom() && !bottom.isTop() &&
                bottom.toString() == "bottom" &&
                bottom.bound(x).isBottom() && bottom.toConstraints().empty(),
            "bottom queries must agree");
    require(top.toBox().bounds.size() == 2 && top.bound(x).isTop(),
            "box export must include every top dimension");

    OctagonState constrained = top;
    constrained.assume(atLeast(x, Rational(0)));
    constrained.assume(atMost(x, Rational(4)));
    constrained.assume(signedSumAtMost(x, 1, y, -1, Rational(2)));
    require(!constrained.isTop() && !constrained.toConstraints().empty() &&
                constrained.toString().find("&&") != std::string::npos,
            "non-top states must export their relational constraints");

    require(bottom.join(constrained).isEquivalentTo(constrained) ==
                CheckResult::True &&
                constrained.join(bottom).isEquivalentTo(constrained) ==
                    CheckResult::True,
            "bottom must be the join identity");
    require(bottom.meet(constrained).isBottom() &&
                constrained.meet(bottom).isBottom(),
            "bottom must absorb meet");
    require(bottom.isSubsetOf(top) == CheckResult::True &&
                top.isSubsetOf(bottom) == CheckResult::False &&
                constrained.isSubsetOf(top) == CheckResult::True &&
                top.isSubsetOf(constrained) == CheckResult::False,
            "subset checks must handle top and bottom");

    OctagonState mutated = bottom;
    mutated.joinWith(constrained);
    require(mutated.isEquivalentTo(constrained) == CheckResult::True,
            "in-place join must match value join");
    mutated.meetWith(top);
    require(mutated.isEquivalentTo(constrained) == CheckResult::True,
            "in-place meet with top must be an identity");
    mutated.forget(x);
    require(mutated.bound(x).isTop(), "forget must remove one dimension");
    bottom.forget(x);
    require(bottom.isBottom(), "forgetting from bottom must retain bottom");

    requireThrows([&] { (void)top.bound(unknown); },
                  "bounding an unknown variable must throw");
    requireThrows([&] { top.forget(unknown); },
                  "forgetting an unknown variable must throw");
    requireThrows([&] { top.assign(unknown, LinearExpression(Rational(0))); },
                  "assigning an unknown target must throw");
    requireThrows([&] { top.assume(atMost(unknown, Rational(0))); },
                  "assuming an unknown variable must throw");

    OctagonConfig relaxedConfig;
    relaxedConfig.integerTightening = false;
    OctagonState otherConfiguration =
        OctagonState::top(environment, relaxedConfig);
    requireThrows([&] { (void)top.join(otherConfiguration); },
                  "states with distinct configurations must not mix");
    const VariableEnvironment smaller({{x, NumericType::integer(), "x"}});
    OctagonState otherEnvironment = OctagonState::top(smaller);
    requireThrows([&] { (void)top.join(otherEnvironment); },
                  "states with distinct environments must not mix");
}

void testFallbacksConstantsAndOptions()
{
    const Variable x(1);
    const Variable y(2);
    const Variable z(3);
    const VariableEnvironment environment(
        {{x, NumericType::integer(), "x"},
         {y, NumericType::integer(), "y"},
         {z, NumericType::integer(), "z"}});
    auto diagnostics = std::make_shared<RecordingDiagnosticSink>();
    OctagonConfig options;
    options.diagnostics = diagnostics;
    OctagonState assigned = OctagonState::top(environment, options);
    assigned.assign(x, LinearExpression(Rational(4)));
    require(assigned.bound(x).lower().value() == Rational(4) &&
                assigned.bound(x).upper().value() == Rational(4),
            "constant assignment must create a singleton");

    LinearExpression nonlinearLinearForm;
    nonlinearLinearForm.setCoefficient(x, Rational(1));
    nonlinearLinearForm.setCoefficient(y, Rational(1));
    nonlinearLinearForm.setCoefficient(z, Rational(1));
    assigned.assign(y, nonlinearLinearForm);
    require(assigned.bound(y).isTop(),
            "unsupported multi-variable assignment must forget its target");

    OctagonState assumptions = OctagonState::top(environment, options);
    assumptions.assume(
        LinearConstraint(LinearExpression(Rational(-1)),
                         ConstraintKind::LessEqual));
    require(!assumptions.isBottom(), "a true constant condition must be kept");
    OctagonState falseConstant = assumptions;
    falseConstant.assume(
        LinearConstraint(LinearExpression(Rational(1)),
                         ConstraintKind::LessEqual));
    require(falseConstant.isBottom(),
            "a false constant condition must produce bottom");
    OctagonState strictFalse = assumptions;
    strictFalse.assume(
        LinearConstraint(LinearExpression(Rational(0)),
                         ConstraintKind::LessThan));
    require(strictFalse.isBottom(), "0 < 0 must produce bottom");

    OctagonState nonzeroTrue = assumptions;
    nonzeroTrue.assume(
        LinearConstraint(LinearExpression(Rational(1)),
                         ConstraintKind::NotEqual));
    require(!nonzeroTrue.isBottom(), "1 != 0 must be true");
    OctagonState nonzeroFalse = assumptions;
    nonzeroFalse.assume(
        LinearConstraint(LinearExpression(Rational(0)),
                         ConstraintKind::NotEqual));
    require(nonzeroFalse.isBottom(), "0 != 0 must produce bottom");

    OctagonState unsupported = assumptions;
    unsupported.assume(
        LinearConstraint(LinearExpression(x), ConstraintKind::NotEqual));
    LinearExpression unequalCoefficients(x);
    unequalCoefficients.setCoefficient(y, Rational(2));
    unsupported.assume(LinearConstraint(std::move(unequalCoefficients),
                                        ConstraintKind::LessEqual));
    require(diagnostics->diagnostics.size() >= 3,
            "unsupported transfers must emit diagnostics");

    assigned.assign(z, TreeExpression::binary(
                           BinaryOperator::Add,
                           TreeExpression::variable(x, NumericType::integer()),
                           TreeExpression::constant(
                               Rational(2), NumericType::integer()),
                           NumericType::integer()));
    require(assigned.bound(z).lower().value() == Rational(6),
            "an affine tree assignment must use the linear path");
    assigned.assign(
        z, TreeExpression::binary(
               BinaryOperator::Multiply,
               TreeExpression::variable(x, NumericType::integer()),
               TreeExpression::variable(y, NumericType::integer()),
               NumericType::integer()));
    require(assigned.bound(z).isTop(),
            "a nonlinear tree assignment must conservatively forget");

    OctagonConfig relaxedOptions;
    relaxedOptions.strongClosure = false;
    relaxedOptions.integerTightening = false;
    OctagonState untightened =
        OctagonState::top(environment, relaxedOptions);
    require(!untightened.capabilities().integerTightening,
            "capabilities must reflect disabled integer tightening");
    untightened.assume(below(x, Rational("3/2")));
    require(untightened.bound(x).upper().value() == Rational("3/2") &&
                untightened.bound(x).upper().isStrict(),
            "disabled integer tightening must preserve the rational bound");

    const Variable fp(10);
    const VariableEnvironment floating(
        {{fp, NumericType::ieee(FloatFormat::binary32()), "fp"}});
    OctagonState floatingState = OctagonState::top(floating, options);
    floatingState.assume(atMost(fp, Rational(1)));
    floatingState.assign(fp, LinearExpression(Rational(1)));
    require(floatingState.bound(fp).isTop(),
            "IEEE operations must currently use conservative fallback");
}

void testPersistentPerStateConfiguration()
{
    const Variable x(1);
    const Variable y(2);
    const VariableEnvironment environment(
        {{x, NumericType::integer(), "x"},
         {y, NumericType::integer(), "y"}});

    OctagonConfig cheapConfig;
    cheapConfig.strongClosure = false;
    cheapConfig.integerTightening = false;
    const OctagonConfig preciseConfig;

    // This state remains the fact for the ordinary predecessor while a copy
    // enters a locally refined suspicious region.
    OctagonState cheap = OctagonState::top(environment, cheapConfig);
    cheap.assume(below(x, Rational("3/2")));
    OctagonState precise = cheap.reconfigured(preciseConfig);

    LinearExpression xPlusOne(x);
    xPlusOne.setConstant(Rational(1));
    precise.assign(y, xPlusOne);

    require(cheap.bound(x).upper().value() == Rational("3/2") &&
                cheap.bound(x).upper().isStrict() &&
                !cheap.config().integerTightening,
            "refining a branch must not retroactively change the retained "
            "cheap predecessor state");
    require(precise.bound(x).upper().value() == Rational(1) &&
                !precise.bound(x).upper().isStrict() &&
                precise.bound(y).upper().value() == Rational(2),
            "the refined state must apply integer tightening and continue "
            "transfer functions under its own configuration");

    requireThrows([&] { (void)cheap.join(precise); },
                  "a mixed-configuration merge must require explicit "
                  "conversion");

    OctagonState converted = cheap.reconfigured(preciseConfig);
    Z3SoundnessChecker checker(environment);
    requireProof(checker.implies(
        cheap, converted,
        "reconfigure: integer source implies precisely normalized state"));
    OctagonState merged = converted.join(precise);
    requireProof(checker.checkJoin(converted, precise, merged));
}

void testJoinClosureLawsAndRandomSoundness()
{
    const Variable x(1);
    const Variable y(2);
    const Variable z(3);
    const std::vector<Variable> variables{x, y, z};
    const VariableEnvironment environment(
        {{x, NumericType::integer(), "x"},
         {y, NumericType::integer(), "y"},
         {z, NumericType::integer(), "z"}});
    Z3SoundnessChecker checker(environment);

    std::uint32_t random = 0x5eed1234U;
    auto nextRandom = [&random]() {
        random = random * 1664525U + 1013904223U;
        return random;
    };
    auto makeState = [&]() {
        OctagonState state = OctagonState::top(environment);
        for (unsigned constraintIndex = 0; constraintIndex < 5;
             ++constraintIndex)
        {
            const Variable lhs = variables[nextRandom() % variables.size()];
            Variable rhs = variables[nextRandom() % variables.size()];
            if (rhs == lhs)
                rhs = variables[(rhs.id() + 1U) % variables.size()];
            const int lhsSign = (nextRandom() & 1U) == 0 ? -1 : 1;
            const int rhsSign = (nextRandom() & 1U) == 0 ? -1 : 1;
            const Rational value(
                static_cast<std::int64_t>(nextRandom() % 17U) - 4);
            state.assume(
                signedSumAtMost(lhs, lhsSign, rhs, rhsSign, value));
        }
        return state;
    };

    for (unsigned iteration = 0; iteration < 32; ++iteration)
    {
        const OctagonState lhs = makeState();
        const OctagonState rhs = makeState();
        const OctagonState third = makeState();
        const OctagonState joined = lhs.join(rhs);
        requireProof(checker.checkJoin(lhs, rhs, joined));
        require(lhs.isSubsetOf(joined) == CheckResult::True &&
                    rhs.isSubsetOf(joined) == CheckResult::True,
                "join must contain both randomized operands");
        require(joined.isEquivalentTo(rhs.join(lhs)) == CheckResult::True,
                "join must be commutative");
        require(lhs.join(lhs).isEquivalentTo(lhs) == CheckResult::True,
                "join must be idempotent");
        require(lhs.join(rhs).join(third).isEquivalentTo(
                    lhs.join(rhs.join(third))) == CheckResult::True,
                "join must be associative");
        const OctagonState independentlyClosed =
            joined.meet(OctagonState::top(environment));
        require(joined.isEquivalentTo(independentlyClosed) == CheckResult::True,
                "join's closed flag must agree with independent closure");

        // These queries deliberately run immediately after join. If the
        // closed-result fast path marks an invalid matrix as closed, the Z3
        // proof or canonical equality checks above expose the stale result.
        (void)joined.bound(x);
        (void)joined.toConstraints();
    }

    OctagonState strict = OctagonState::top(environment);
    strict.assume(below(x, Rational("3/2")));
    OctagonState closed = OctagonState::top(environment);
    closed.assume(atMost(x, Rational(2)));
    const OctagonState integerJoin = strict.join(closed);
    require(integerJoin.bound(x).upper().value() == Rational(2) &&
                !integerJoin.bound(x).upper().isStrict(),
            "join must preserve integer-tight unary closure");
}

void testUniversalJoinConfigurations()
{
    // Exercise both odd and even dimensions, including the empty DBM. The
    // implementation must not rely on the powers of two used by performance
    // sampling or on a fixed two-variable layout.
    for (std::size_t dimensionCount = 0; dimensionCount <= 7;
         ++dimensionCount)
    {
        std::vector<VariableDeclaration> declarations;
        for (std::size_t dimension = 0; dimension < dimensionCount;
             ++dimension)
        {
            declarations.push_back(
                {Variable(static_cast<std::uint32_t>(dimension + 10)),
                 NumericType::integer(),
                 "i" + std::to_string(dimension)});
        }
        const VariableEnvironment environment(std::move(declarations));
        OctagonState lhs = OctagonState::top(environment);
        OctagonState rhs = OctagonState::top(environment);
        for (std::size_t dimension = 0; dimension < dimensionCount;
             ++dimension)
        {
            const Variable variable(
                static_cast<std::uint32_t>(dimension + 10));
            lhs.assume(equalsConstant(
                variable, Rational(static_cast<std::int64_t>(dimension) - 3)));
            rhs.assume(equalsConstant(
                variable, Rational(static_cast<std::int64_t>(dimension) + 2)));
        }
        const OctagonState joined = lhs.join(rhs);
        Z3SoundnessChecker checker(environment);
        requireProof(checker.checkJoin(lhs, rhs, joined));
        require(joined.isEquivalentTo(rhs.join(lhs)) == CheckResult::True,
                "join must be dimension-independent");
        require(joined.isEquivalentTo(joined.meet(OctagonState::top(environment))) ==
                    CheckResult::True,
                "closed join must equal an independently normalized copy");
        if (dimensionCount == 0)
            require(joined.isTop(), "the zero-dimensional join must be top");
        else
        {
            const Variable first(10);
            require(joined.bound(first).lower().value() == Rational(-3) &&
                        joined.bound(first).upper().value() == Rational(2),
                    "point hull bounds must hold at arbitrary dimensions");
        }
    }

    const Variable integer(1);
    const Variable real(2);
    const Variable otherInteger(3);
    const VariableEnvironment mixed(
        {{integer, NumericType::integer(), "integer"},
         {real, NumericType::real(), "real"},
         {otherInteger, NumericType::integer(), "other_integer"}});

    for (bool strongClosure : {false, true})
    {
        for (bool integerTightening : {false, true})
        {
            OctagonConfig options;
            options.strongClosure = strongClosure;
            options.integerTightening = integerTightening;
            Z3SoundnessChecker checker(mixed);

            OctagonState lhs = OctagonState::top(mixed, options);
            LinearExpression strictRelation(integer);
            strictRelation.setCoefficient(real, Rational(-1));
            strictRelation.setConstant(Rational("-1/3"));
            lhs.assume(LinearConstraint(std::move(strictRelation),
                                        ConstraintKind::LessThan));
            lhs.assume(atLeast(integer, Rational(-2)));
            lhs.assume(signedSumAtMost(otherInteger, 1, real, 1,
                                       Rational("7/2")));

            OctagonState rhs = OctagonState::top(mixed, options);
            LinearExpression closedRelation(integer);
            closedRelation.setCoefficient(real, Rational(-1));
            closedRelation.setConstant(Rational("-2/3"));
            rhs.assume(LinearConstraint(std::move(closedRelation),
                                        ConstraintKind::LessEqual));
            rhs.assume(atMost(integer, Rational(5)));
            rhs.assume(signedSumAtMost(otherInteger, 1, real, 1,
                                       Rational(4)));

            const OctagonState joined = lhs.join(rhs);
            requireProof(checker.checkJoin(lhs, rhs, joined));
            require(joined.isEquivalentTo(rhs.join(lhs)) == CheckResult::True &&
                        joined.join(joined).isEquivalentTo(joined) ==
                            CheckResult::True,
                    "join laws must hold for every closure/tightening option");
            require(joined.isEquivalentTo(joined.meet(
                        OctagonState::top(mixed, options))) ==
                        CheckResult::True,
                    "join closure must hold for every option configuration");
        }
    }

    const Variable value(20);
    const VariableEnvironment reals({{value, NumericType::real(), "value"}});
    OctagonState strict = OctagonState::top(reals);
    strict.assume(below(value, Rational("1/3")));
    OctagonState closed = OctagonState::top(reals);
    closed.assume(atMost(value, Rational("2/3")));
    const OctagonState realJoin = strict.join(closed);
    requireProof(Z3SoundnessChecker(reals).checkJoin(strict, closed, realJoin));
    require(realJoin.bound(value).upper().value() == Rational("2/3") &&
                !realJoin.bound(value).upper().isStrict(),
            "join must preserve exact rational and strict real ordering");
}

void testBottomFixpointAndEnvironmentPaths()
{
    const Variable x(1);
    const VariableEnvironment environment({{x, NumericType::integer(), "x"}});
    OctagonState bottom = OctagonState::bottom(environment);
    OctagonState finite = OctagonState::top(environment);
    finite.assume(equalsConstant(x, Rational(1)));

    require(bottom.widen(finite).isEquivalentTo(finite) == CheckResult::True &&
                finite.widen(bottom).isEquivalentTo(finite) == CheckResult::True,
            "widening must handle bottom operands");
    require(bottom.narrow(bottom).isBottom() &&
                finite.narrow(bottom).isBottom(),
            "narrowing must handle bottom operands");
    require(bottom.projectedLowerBounds().isBottom(),
            "projecting bottom must retain bottom");

    const VariableEnvironment extended(
        {{x, NumericType::integer(), "x"},
         {Variable(2), NumericType::integer(), "y"}});
    require(bottom.withEnvironment(extended).isBottom(),
            "changing a bottom environment must retain bottom");
    const VariableEnvironment changedType({{x, NumericType::real(), "x"}});
    requireThrows([&] { (void)finite.withEnvironment(changedType); },
                  "environment changes must reject type changes");

    OctagonState widened = OctagonState::top(environment);
    widened.assume(atMost(x, Rational(1)));
    OctagonState next = OctagonState::top(environment);
    next.assume(atMost(x, Rational(2)));
    widened.widenWith(next);
    widened.forget(x);
    require(widened.isTop(),
            "forget must normalize a raw widened state before projection");
}

} // namespace

int main()
{
    try
    {
        testExactNumericLayer();
        testPolymorphicStateContract();
        testEnvironment();
        testPublicDomainArchitecture();
        testAssumeClosureAndAssignment();
        testStrictIntegerAndRealBounds();
        testEnvironmentChanges();
        testLatticeAndZ3Soundness();
        testTopBottomIdentitiesAndQueries();
        testFallbacksConstantsAndOptions();
        testPersistentPerStateConfiguration();
        testJoinClosureLawsAndRandomSoundness();
        testUniversalJoinConfigurations();
        testBottomFixpointAndEnvironmentPaths();
        std::cout << "relational-domain tests: PASS\n";
        return EXIT_SUCCESS;
    }
    catch (const std::exception& error)
    {
        std::cerr << "relational-domain tests: FAIL: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
