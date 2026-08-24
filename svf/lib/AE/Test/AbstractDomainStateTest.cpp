//===- AbstractDomainStateTest.cpp -- Abstract-domain state tests -------===//

#include "AE/Core/BoxDomain.h"
#include "AE/Core/ConvexPolyhedraDomain.h"
#include "AE/Core/NonRelationalDomain.h"
#include "AE/Core/OctagonDomain.h"
#include "Z3SoundnessChecker.h"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <utility>

using namespace SVF::AbstractDomain;
using SVF::test::ProofResult;
using SVF::test::Z3SoundnessChecker;

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

class RecordingDiagnosticSink final : public DiagnosticSink
{
public:
    void report(const Diagnostic& diagnostic) override
    {
        diagnostics.push_back(diagnostic);
    }

    std::vector<Diagnostic> diagnostics;
};

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

LinearConstraint atMost(Variable variable, const Rational& value)
{
    return lessEqual(LinearExpression(variable), LinearExpression(value));
}

LinearConstraint atLeast(Variable variable, const Rational& value)
{
    return greaterEqual(LinearExpression(variable), LinearExpression(value));
}

LinearConstraint equalsValue(Variable variable, const Rational& value)
{
    return equal(LinearExpression(variable), LinearExpression(value));
}

void testBoxState()
{
    const Variable x(1);
    const Variable y(2);
    const VariableEnvironment environment(
        {{x, NumericType::real(), "x"}, {y, NumericType::real(), "y"}});
    Z3SoundnessChecker checker(environment);

    BoxState state = BoxState::top(environment);
    state.assume(atLeast(x, Rational(1)));
    state.assume(atLeast(y, Rational(2)));
    LinearExpression sum(x);
    sum.setCoefficient(y, Rational(1));
    const LinearConstraint sumLimit =
        lessEqual(sum, LinearExpression(Rational(5)));
    const BoxState beforeAssume = state;
    state.assume(sumLimit);
    requireProof(checker.checkAssume(beforeAssume, sumLimit, state));
    require(state.bound(x).upper().value() == Rational(3) &&
                state.bound(y).upper().value() == Rational(4),
            "Box propagation must tighten both dimensions");

    LinearExpression twiceX(x);
    twiceX *= Rational(2);
    twiceX.setConstant(Rational(1));
    const BoxState beforeAssign = state;
    state.assign(y, twiceX);
    requireProof(checker.checkAssignment(beforeAssign, y, twiceX, state));
    require(state.bound(y).lower().value() == Rational(3) &&
                state.bound(y).upper().value() == Rational(7),
            "Box assignment must use exact rational interval bounds");

    BoxState next = BoxState::top(environment);
    next.assume(atLeast(x, Rational(0)));
    next.assume(atMost(x, Rational(8)));
    BoxState widened = state.widen(next);
    requireProof(checker.checkWidening(state, next, widened));

    BoxState strict = BoxState::top(environment);
    strict.assume(lessThan(LinearExpression(x), LinearExpression(Rational(0))));
    require(strict.entails(atMost(x, Rational(0))) == CheckResult::True,
            "a strict upper bound must entail the corresponding closed bound");

    const VariableEnvironment changedType(
        {{x, NumericType::integer(), "x"}, {y, NumericType::real(), "y"}});
    requireThrows([&] { state.changeEnvironment(changedType); },
                  "Box environment changes must reject numeric type changes");
}

void testBoxLatticeAndFallbacks()
{
    const Variable x(1);
    const Variable y(2);
    const VariableEnvironment integers(
        {{x, NumericType::integer(), "x"}, {y, NumericType::integer(), "y"}});
    IntervalBox initial;
    initial.bounds.emplace(x, Interval(Bound::finite(Rational("1/2")),
                                       Bound::finite(Rational("7/2"))));
    BoxState finite = BoxState::fromBox(integers, initial);
    require(finite.bound(x).lower().value() == Rational(1) &&
                finite.bound(x).upper().value() == Rational(3),
            "integer Box bounds must be tightened to integral endpoints");

    BoxState top = BoxState::top(integers);
    BoxState bottom = BoxState::bottom(integers);
    require(top.isTop() && bottom.isBottom() &&
                bottom.join(finite).isEquivalentTo(finite) ==
                    CheckResult::True &&
                top.meet(finite).isEquivalentTo(finite) == CheckResult::True,
            "Box top/bottom lattice identities must hold");
    require(finite.toConstraints().size() == 2 &&
                finite.toBox().bounds.size() == integers.size(),
            "Box exchange formats must include finite and top variables");

    BoxState wider = BoxState::top(integers);
    wider.assume(atLeast(x, Rational(0)));
    wider.assume(atMost(x, Rational(5)));
    BoxState current = BoxState::top(integers);
    current.assume(atLeast(x, Rational(1)));
    current.assume(atMost(x, Rational(3)));
    const BoxState thresholdWidened =
        current.widen(wider, WideningPolicy{{Rational(0), Rational(10)}});
    const BoxState widened = current.widen(wider);
    const BoxState narrowed = widened.narrow(wider);
    require(thresholdWidened.bound(x).upper().value() == Rational(10) &&
                narrowed.bound(x).upper().value() == Rational(5),
            "Box threshold widening and narrowing must update unstable bounds");

    auto diagnostics = std::make_shared<RecordingDiagnosticSink>();
    BoxConfig config;
    config.diagnostics = diagnostics;
    BoxState fallback = BoxState::top(integers, config);
    const TreeExpression xTree =
        TreeExpression::variable(x, NumericType::integer());
    const TreeExpression yTree =
        TreeExpression::variable(y, NumericType::integer());
    const TreeExpression nonlinear = TreeExpression::binary(
        BinaryOperator::Multiply, xTree, yTree, NumericType::integer());
    fallback.assign(x, nonlinear);
    fallback.assume(TreeConstraint(nonlinear, ConstraintKind::LessEqual));
    require(fallback.bound(x).isTop() && diagnostics->diagnostics.size() == 2,
            "nonlinear Box operations must report their sound fallbacks");

    const Variable z(3);
    const VariableEnvironment extended({{x, NumericType::integer(), "x"},
                                        {y, NumericType::integer(), "y"},
                                        {z, NumericType::integer(), "z"}});
    fallback.changeEnvironment(extended, true);
    require(fallback.bound(z).lower().value() == Rational(0) &&
                fallback.bound(z).upper().value() == Rational(0),
            "requested Box environment projection must initialize new symbols");
    requireThrows([&] { fallback.forget(Variable(99)); },
                  "Box must reject unknown variables");
}

template <typename State>
void checkExpressionBoundsAndSubstitution(State state, Variable x, Variable y)
{
    state.assume(equalsValue(x, Rational(1)));
    state.assume(equalsValue(y, Rational(4)));
    LinearExpression expression(x);
    expression.setCoefficient(y, Rational(1));
    expression.setConstant(Rational(2));
    const Interval expressionBound = state.bound(expression);
    require(expressionBound.lower().value() == Rational(7) &&
                expressionBound.upper().value() == Rational(7),
            std::string(state.name()) +
                " must bound a complete affine expression");

    const TreeExpression affineTree = TreeExpression::binary(
        BinaryOperator::Add,
        TreeExpression::variable(x, NumericType::real()),
        TreeExpression::constant(Rational(2), NumericType::real()),
        NumericType::real());
    require(state.bound(affineTree).lower().value() == Rational(3) &&
                state.bound(affineTree).upper().value() == Rational(3),
            std::string(state.name()) +
                " must lower affine tree bounds through the same path");
    const TreeExpression nonlinearTree = TreeExpression::binary(
        BinaryOperator::Multiply,
        TreeExpression::variable(x, NumericType::real()),
        TreeExpression::variable(y, NumericType::real()),
        NumericType::real());
    require(state.bound(nonlinearTree).isTop(),
            std::string(state.name()) +
                " must return top for an unsupported nonlinear bound");
    State unsupportedPreimage = state;
    unsupportedPreimage.substitute(x, nonlinearTree);
    require(unsupportedPreimage.bound(x).isTop() &&
                unsupportedPreimage.entails(equalsValue(y, Rational(4))) ==
                    CheckResult::True,
            std::string(state.name()) +
                " must project only the unknown nonlinear output in a "
                "backward transfer");

    State backward = State::top(state.environment(), state.config());
    backward.assume(equalsValue(x, Rational(10)));
    const State backwardPost = backward;
    LinearExpression yPlusOne(y);
    yPlusOne.setConstant(Rational(1));
    backward.substitute(x, yPlusOne);
    Z3SoundnessChecker checker(state.environment());
    requireProof(checker.checkParallelSubstitution(
        backwardPost, {{x, yPlusOne}}, backward));
    require(backward.entails(equalsValue(y, Rational(9))) == CheckResult::True,
            std::string(state.name()) +
                " must compute the affine assignment preimage");

    State parallel = State::top(state.environment(), state.config());
    parallel.assume(equalsValue(x, Rational(1)));
    parallel.assume(equalsValue(y, Rational(2)));
    const State parallelPost = parallel;
    const LinearAssignmentList replacements{
        {x, LinearExpression(y)}, {y, LinearExpression(x)}};
    parallel.substituteParallel(replacements);
    requireProof(checker.checkParallelSubstitution(
        parallelPost, replacements, parallel));
    require(parallel.entails(equalsValue(x, Rational(2))) == CheckResult::True &&
                parallel.entails(equalsValue(y, Rational(1))) ==
                    CheckResult::True,
            std::string(state.name()) +
                " must make backward parallel substitution simultaneous");
}

void testCompletedNumericalSurface()
{
    const Variable x(1);
    const Variable y(2);
    const VariableEnvironment reals(
        {{x, NumericType::real(), "x"}, {y, NumericType::real(), "y"}});

    checkExpressionBoundsAndSubstitution(BoxState::top(reals), x, y);
    checkExpressionBoundsAndSubstitution(OctagonState::top(reals), x, y);
    checkExpressionBoundsAndSubstitution(
        ConvexPolyhedraState::top(reals), x, y);

    LinearExpression relationalSum(x);
    relationalSum.setCoefficient(y, Rational(1));
    OctagonState relationalOctagon = OctagonState::top(reals);
    relationalOctagon.assume(
        equal(relationalSum, LinearExpression(Rational(5))));
    const Interval octagonalSum = relationalOctagon.bound(relationalSum);
    require(octagonalSum.lower().value() == Rational(5) &&
                octagonalSum.upper().value() == Rational(5),
            "Octagon expression bounds must read a directly represented "
            "signed-sum DBM edge");
    ConvexPolyhedraState relationalPolyhedra =
        ConvexPolyhedraState::top(reals);
    relationalPolyhedra.assume(
        equal(relationalSum, LinearExpression(Rational(5))));
    const Interval polyhedralSum = relationalPolyhedra.bound(relationalSum);
    require(polyhedralSum.lower().value() == Rational(5) &&
                polyhedralSum.upper().value() == Rational(5),
            "Polyhedra expression bounds must optimize the full affine "
            "objective without intervalizing its variables");

    const auto checkClosure = [&](auto state)
    {
        state.assume(
            lessThan(LinearExpression(x), LinearExpression(Rational(1))));
        require(state.bound(x).upper().isStrict(),
                std::string(state.name()) +
                    " must retain an open boundary before closure");
        state.close();
        state.canonicalize();
        state.minimize();
        require(state.bound(x).upper().value() == Rational(1) &&
                    !state.bound(x).upper().isStrict(),
                std::string(state.name()) +
                    " topological closure must close strict boundaries");
    };
    checkClosure(BoxState::top(reals));
    checkClosure(OctagonState::top(reals));
    checkClosure(ConvexPolyhedraState::top(reals));

    BoxState left = BoxState::top(
        VariableEnvironment({{x, NumericType::real(), "x"}}));
    BoxState right = BoxState::top(
        VariableEnvironment({{y, NumericType::real(), "y"}}));
    left.assume(equalsValue(x, Rational(1)));
    right.assume(equalsValue(y, Rational(2)));
    const VariableEnvironment unified = left.unifyEnvironmentWith(right);
    require(unified.size() == 2 && left.environment() == unified &&
                right.environment() == unified && left.bound(y).isTop() &&
                right.bound(x).isTop(),
            "one-call environment unification must lift both states");
    const BoxState combined = left.meet(right);
    require(combined.entails(equalsValue(x, Rational(1))) == CheckResult::True &&
                combined.entails(equalsValue(y, Rational(2))) ==
                    CheckResult::True,
            "aligned states must compose without another remap");

    LinearExpression sum(x);
    sum.setCoefficient(y, Rational(1));
    const LinearConstraint linearThreshold =
        lessEqual(sum, LinearExpression(Rational(10)));
    BoxState boxCurrent = BoxState::top(reals);
    boxCurrent.assume(atLeast(x, Rational(0)));
    boxCurrent.assume(atLeast(y, Rational(0)));
    boxCurrent.assume(atMost(x, Rational(1)));
    boxCurrent.assume(atMost(y, Rational(1)));
    BoxState boxNext = BoxState::top(reals);
    boxNext.assume(atLeast(x, Rational(0)));
    boxNext.assume(atLeast(y, Rational(0)));
    boxNext.assume(atMost(x, Rational(2)));
    boxNext.assume(atMost(y, Rational(2)));
    const BoxState boxThreshold = boxCurrent.widen(
        boxNext, WideningPolicy({}, {linearThreshold}));
    require(boxThreshold.bound(x).upper().value() == Rational(10) &&
                boxThreshold.bound(y).upper().value() == Rational(10),
            "Box must retain the representable interval consequence of a "
            "general linear widening threshold");

    OctagonState octCurrent = OctagonState::top(reals);
    octCurrent.assume(lessEqual(sum, LinearExpression(Rational(1))));
    OctagonState octNext = OctagonState::top(reals);
    octNext.assume(lessEqual(sum, LinearExpression(Rational(2))));
    const OctagonState octThreshold = octCurrent.widen(
        octNext, WideningPolicy({}, {linearThreshold}));
    require(octThreshold.entails(linearThreshold) == CheckResult::True,
            "Octagon must retain a representable general linear threshold");

    const VariableEnvironment integers(
        {{x, NumericType::integer(), "x"}});
    ConvexPolyhedraState tightened = ConvexPolyhedraState::top(integers);
    LinearExpression twiceX(x);
    twiceX *= Rational(2);
    tightened.assume(
        lessEqual(twiceX, LinearExpression(Rational(3))));
    require(tightened.bound(x).upper().value() == Rational(1) &&
                !tightened.bound(x).upper().isStrict() &&
                tightened.capabilities().integerTightening,
            "integer Polyhedra must apply per-row gcd tightening");

    ConvexPolyhedraConfig rationalConfig;
    rationalConfig.integerTightening = false;
    ConvexPolyhedraState untightened =
        ConvexPolyhedraState::top(integers, rationalConfig);
    untightened.assume(
        lessEqual(twiceX, LinearExpression(Rational(3))));
    require(untightened.bound(x).upper().value() == Rational("3/2") &&
                !untightened.capabilities().integerTightening,
            "Polyhedra integer tightening must remain an explicit policy");
    std::unique_ptr<NumericalState> restored =
        NumericalState::deserializeRaw(untightened.serializeRaw());
    require(!restored->capabilities().integerTightening &&
                restored->isEquivalentTo(untightened) == CheckResult::True,
            "raw serialization must preserve the Polyhedra tightening policy");

    const DomainCapabilities capabilities = tightened.capabilities();
    require(capabilities.expressionBounds &&
                capabilities.backwardAssignments &&
                capabilities.topologicalClosure &&
                capabilities.canonicalization,
            "capabilities must expose the completed numerical surface");
}

void testConvexPolyhedraState()
{
    const Variable x(1);
    const Variable y(2);
    const VariableEnvironment environment(
        {{x, NumericType::real(), "x"}, {y, NumericType::real(), "y"}});
    Z3SoundnessChecker checker(environment);

    LinearExpression sum(x);
    sum.setCoefficient(y, Rational(1));
    const LinearConstraint sumLimit =
        lessEqual(sum, LinearExpression(Rational(5)));
    ConvexPolyhedraState state = ConvexPolyhedraState::top(environment);
    state.assume(atLeast(x, Rational(0)));
    state.assume(atLeast(y, Rational(0)));
    const ConvexPolyhedraState beforeAssume = state;
    state.assume(sumLimit);
    requireProof(checker.checkAssume(beforeAssume, sumLimit, state));
    require(state.entails(sumLimit) == CheckResult::True,
            "Polyhedra must retain a general linear relation");

    LinearExpression yPlusOne(y);
    yPlusOne.setConstant(Rational(1));
    const ConvexPolyhedraState beforeAssign = state;
    state.assign(x, yPlusOne);
    requireProof(checker.checkAssignment(beforeAssign, x, yPlusOne, state));
    require(state.entails(equal(LinearExpression(x), yPlusOne)) ==
                CheckResult::True,
            "Polyhedra affine assignment must preserve equality");

    ConvexPolyhedraState origin = ConvexPolyhedraState::top(environment);
    origin.assume(equalsValue(x, Rational(0)));
    origin.assume(equalsValue(y, Rational(0)));
    ConvexPolyhedraState endpoint = ConvexPolyhedraState::top(environment);
    endpoint.assume(equalsValue(x, Rational(2)));
    endpoint.assume(equalsValue(y, Rational(2)));
    const ConvexPolyhedraState hull = origin.join(endpoint);
    requireProof(checker.checkJoin(origin, endpoint, hull));
    require(hull.entails(equal(LinearExpression(x), LinearExpression(y))) ==
                    CheckResult::True &&
                hull.bound(x).lower().value() == Rational(0) &&
                hull.bound(x).upper().value() == Rational(2),
            "Polyhedra join must compute the convex hull of two points");

    ConvexPolyhedraState forgotten = hull;
    forgotten.forget(y);
    requireProof(checker.checkForget(hull, y, forgotten));
    require(forgotten.bound(y).isTop(),
            "Polyhedra forget must existentially eliminate the variable");

    const VariableEnvironment changedType(
        {{x, NumericType::integer(), "x"}, {y, NumericType::real(), "y"}});
    requireThrows([&] { state.changeEnvironment(changedType); },
                  "Polyhedra environment changes must reject type changes");
}

void testPolyhedraLatticeAndFallbacks()
{
    const Variable x(1);
    const Variable y(2);
    const VariableEnvironment environment(
        {{x, NumericType::real(), "x"}, {y, NumericType::real(), "y"}});
    IntervalBox box;
    box.bounds.emplace(
        x, Interval(Bound::finite(Rational(-1)), Bound::finite(Rational(2))));
    ConvexPolyhedraState finite =
        ConvexPolyhedraState::fromBox(environment, box);
    ConvexPolyhedraState top = ConvexPolyhedraState::top(environment);
    ConvexPolyhedraState bottom = ConvexPolyhedraState::bottom(environment);
    require(top.isTop() && bottom.isBottom() &&
                bottom.join(finite).isEquivalentTo(finite) ==
                    CheckResult::True &&
                top.meet(finite).isEquivalentTo(finite) == CheckResult::True,
            "Polyhedra top/bottom lattice identities must hold");

    ConvexPolyhedraState next = finite;
    next.assume(atLeast(y, Rational(0)));
    const ConvexPolyhedraState meet = finite.meet(next);
    require(meet.isEquivalentTo(next) == CheckResult::True,
            "Polyhedra meet must represent constraint intersection");
    const ConvexPolyhedraState widened = finite.widen(next);
    const ConvexPolyhedraState narrowed = widened.narrow(next);
    require(next.isSubsetOf(widened) == CheckResult::True &&
                narrowed.isEquivalentTo(next) == CheckResult::True,
            "Polyhedra widening/narrowing inclusion must hold");

    auto diagnostics = std::make_shared<RecordingDiagnosticSink>();
    ConvexPolyhedraConfig config;
    config.diagnostics = diagnostics;
    ConvexPolyhedraState fallback =
        ConvexPolyhedraState::top(environment, config);
    const TreeExpression xTree =
        TreeExpression::variable(x, NumericType::real());
    const TreeExpression yTree =
        TreeExpression::variable(y, NumericType::real());
    const TreeExpression nonlinear = TreeExpression::binary(
        BinaryOperator::Multiply, xTree, yTree, NumericType::real());
    fallback.assign(x, nonlinear);
    fallback.assume(TreeConstraint(nonlinear, ConstraintKind::LessEqual));
    fallback.assume(notEqual(LinearExpression(x), LinearExpression(y)));
    require(fallback.bound(x).isTop() && diagnostics->diagnostics.size() == 3,
            "non-convex/nonlinear Polyhedra operations must report fallbacks");

    ConvexPolyhedraState contradiction = ConvexPolyhedraState::top(environment);
    contradiction.assume(atMost(x, Rational(0)));
    contradiction.assume(
        greaterThan(LinearExpression(x), LinearExpression(Rational(0))));
    require(contradiction.isBottom() &&
                contradiction.toConstraints().size() == 1,
            "Polyhedra feasibility must detect contradictory constraints");

    LinearExpression sum(x);
    sum.setCoefficient(y, Rational(1));
    ConvexPolyhedraState current = ConvexPolyhedraState::top(environment);
    current.assume(lessEqual(sum, LinearExpression(Rational(0))));
    ConvexPolyhedraState following = ConvexPolyhedraState::top(environment);
    following.assume(lessEqual(sum, LinearExpression(Rational(1))));
    const LinearConstraint threshold =
        lessEqual(sum, LinearExpression(Rational(10)));
    WideningPolicy policy;
    policy.linearThresholds.push_back(threshold);
    policy.linearThresholds.push_back(
        lessEqual(sum, LinearExpression(Rational(0))));
    const ConvexPolyhedraState standard = current.widen(following);
    const ConvexPolyhedraState thresholded = current.widen(following, policy);
    Z3SoundnessChecker checker(environment);
    requireProof(checker.checkWidening(current, following, thresholded));
    require(current.capabilities().thresholdWidening &&
                standard.entails(threshold) != CheckResult::True &&
                thresholded.entails(threshold) == CheckResult::True &&
                thresholded.entails(lessEqual(
                    sum, LinearExpression(Rational(0)))) != CheckResult::True &&
                following.isSubsetOf(thresholded) == CheckResult::True,
            "Polyhedra threshold widening must retain exactly applicable "
            "linear thresholds and contain the next state");

    ConvexPolyhedraState scalarCurrent = ConvexPolyhedraState::top(environment);
    scalarCurrent.assume(atMost(x, Rational(1)));
    ConvexPolyhedraState scalarFollowing =
        ConvexPolyhedraState::top(environment);
    scalarFollowing.assume(atMost(x, Rational(2)));
    const ConvexPolyhedraState scalarThresholded =
        scalarCurrent.widen(scalarFollowing, WideningPolicy{{Rational(10)}});
    require(scalarThresholded.entails(atMost(x, Rational(10))) ==
                CheckResult::True,
            "Polyhedra threshold widening must support shared scalar "
            "threshold policies");
    requireProof(checker.checkWidening(scalarCurrent, scalarFollowing,
                                       scalarThresholded));
}

template <typename StateT>
void checkParallelAssignmentFor(const char* domainName)
{
    const Variable x(1);
    const Variable y(2);
    const Variable z(3);
    const VariableEnvironment environment({{x, NumericType::integer(), "x"},
                                           {y, NumericType::integer(), "y"},
                                           {z, NumericType::integer(), "z"}});
    Z3SoundnessChecker checker(environment);

    StateT state = StateT::top(environment);
    state.assumeAll({equalsValue(x, Rational(1)), equalsValue(y, Rational(2)),
                     equalsValue(z, Rational(9))});
    const StateT before = state;

    LinearExpression difference(x);
    difference.setCoefficient(y, Rational(-1));
    const LinearAssignmentList assignments{
        {x, LinearExpression(y)}, {y, LinearExpression(x)}, {z, difference}};
    NumericalState& numerical = state;
    numerical.assignParallel(assignments);

    requireProof(checker.checkParallelAssignment(before, assignments, state));
    require(state.capabilities().parallelAssignments &&
                state.environment() == environment &&
                state.bound(x).lower().value() == Rational(2) &&
                state.bound(x).upper().value() == Rational(2) &&
                state.bound(y).lower().value() == Rational(1) &&
                state.bound(y).upper().value() == Rational(1) &&
                state.bound(z).lower().value() == Rational(-1) &&
                state.bound(z).upper().value() == Rational(-1),
            std::string(domainName) +
                " parallel assignment must read every RHS from the old state");

    StateT sequential = before;
    sequential.assign(x, LinearExpression(y));
    sequential.assign(y, LinearExpression(x));
    sequential.assign(z, difference);
    require(sequential.bound(y).lower().value() == Rational(2) &&
                sequential.bound(z).lower().value() == Rational(0),
            std::string(domainName) +
                " test must distinguish parallel from sequential assignment");
}

void testParallelAssignments()
{
    checkParallelAssignmentFor<BoxState>("Box");
    checkParallelAssignmentFor<OctagonState>("Octagon");
    checkParallelAssignmentFor<ConvexPolyhedraState>("Polyhedra");

    const Variable x(1);
    const Variable y(2);
    const VariableEnvironment environment(
        {{x, NumericType::integer(), "x"}, {y, NumericType::integer(), "y"}});
    BoxState state = BoxState::top(environment);
    NumericalState& numerical = state;
    requireThrows(
        [&] {
            numerical.assignParallel(LinearAssignmentList{
                {x, LinearExpression(y)}, {x, LinearExpression(x)}});
        },
        "parallel assignment must reject duplicate targets");

    state.assumeAll({equalsValue(x, Rational(1)), equalsValue(y, Rational(2))});
    state.assignParallel(TreeAssignmentList{
        {x, TreeExpression::variable(y, NumericType::integer())},
        {y, TreeExpression::variable(x, NumericType::integer())}});
    require(state.bound(x).lower().value() == Rational(2) &&
                state.bound(y).lower().value() == Rational(1),
            "affine tree parallel assignment must use simultaneous semantics");
}

template <typename StateT>
void checkRawRoundTrip(const StateT& state, const char* domainName)
{
    const NumericalState::RawBuffer first = state.serializeRaw();
    const NumericalState::RawBuffer second = state.serializeRaw();
    require(first == second, std::string(domainName) +
                                 " raw serialization must be deterministic");

    std::unique_ptr<NumericalState> decoded =
        NumericalState::deserializeRaw(first);
    const auto* restored = dynamic_cast<const StateT*>(decoded.get());
    require(restored != nullptr,
            std::string(domainName) +
                " raw deserialization must restore the concrete domain");
    require(restored->environment() == state.environment() &&
                restored->isEquivalentTo(state) == CheckResult::True &&
                state.isEquivalentTo(*restored) == CheckResult::True,
            std::string(domainName) +
                " raw round-trip must preserve environment and semantics");
    require(restored->hash() == state.hash() &&
                restored->serializeRaw() == first,
            std::string(domainName) +
                " raw round-trip must preserve canonical bytes and hash");
}

void testNumericalHashAndRawSerialization()
{
    const Variable x(1);
    const Variable y(2);
    const Variable ieee(3);
    const VariableEnvironment environment(
        {{x, NumericType::integer(), std::string("x\0raw", 5)},
         {y, NumericType::real(), "y"},
         {ieee, NumericType::ieee(FloatFormat::binary32()), "ieee"}});

    BoxConfig boxConfig;
    boxConfig.integerTightening = false;
    BoxState box = BoxState::top(environment, boxConfig);
    box.assume(atLeast(x, Rational("1/2")));
    box.assume(
        lessThan(LinearExpression(y), LinearExpression(Rational("7/3"))));
    checkRawRoundTrip(box, "Box");
    checkRawRoundTrip(BoxState::bottom(environment, boxConfig), "Box bottom");

    OctagonConfig octagonConfig;
    octagonConfig.strongClosure = false;
    octagonConfig.integerTightening = false;
    LinearExpression difference(x);
    difference.setCoefficient(y, Rational(-1));
    LinearExpression sum(x);
    sum.setCoefficient(y, Rational(1));
    OctagonState octagon = OctagonState::top(environment, octagonConfig);
    octagon.assume(lessEqual(difference, LinearExpression(Rational(3))));
    octagon.assume(lessThan(sum, LinearExpression(Rational(5))));
    checkRawRoundTrip(octagon, "Octagon");
    checkRawRoundTrip(OctagonState::bottom(environment, octagonConfig),
                      "Octagon bottom");

    OctagonState wideningCurrent =
        OctagonState::top(environment, octagonConfig);
    wideningCurrent.assume(atMost(x, Rational(0)));
    OctagonState wideningNext = OctagonState::top(environment, octagonConfig);
    wideningNext.assume(atMost(x, Rational(1)));
    checkRawRoundTrip(wideningCurrent.widen(wideningNext),
                      "raw widened Octagon");

    std::unique_ptr<NumericalState> restoredOctagon =
        NumericalState::deserializeRaw(octagon.serializeRaw());
    const auto* restoredOctagonState =
        dynamic_cast<const OctagonState*>(restoredOctagon.get());
    require(restoredOctagonState &&
                !restoredOctagonState->config().strongClosure &&
                !restoredOctagonState->config().integerTightening,
            "Octagon raw serialization must preserve operation configuration");
    requireThrows(
        [&] {
            OctagonState incompatible = OctagonState::top(environment);
            incompatible.joinWith(*restoredOctagonState);
        },
        "deserialized Octagon configuration must remain "
        "compatibility-relevant");

    ConvexPolyhedraState polyhedron = ConvexPolyhedraState::top(environment);
    LinearExpression general;
    general.setCoefficient(x, Rational(2));
    general.setCoefficient(y, Rational(3));
    polyhedron.assume(lessEqual(general, LinearExpression(Rational("11/2"))));
    polyhedron.assume(atLeast(x, Rational(-2)));
    checkRawRoundTrip(polyhedron, "Convex Polyhedra");
    checkRawRoundTrip(ConvexPolyhedraState::bottom(environment),
                      "Convex Polyhedra bottom");

    BoxState boxEquivalent = BoxState::top(environment, boxConfig);
    boxEquivalent.assume(
        lessThan(LinearExpression(y), LinearExpression(Rational("7/3"))));
    boxEquivalent.assume(atLeast(x, Rational("1/2")));
    boxEquivalent.assume(atLeast(x, Rational(-10)));
    require(box.isEquivalentTo(boxEquivalent) == CheckResult::True &&
                box.hash() == boxEquivalent.hash(),
            "equivalent Box construction histories must have equal hashes");

    OctagonState octagonEquivalent =
        OctagonState::top(environment, octagonConfig);
    octagonEquivalent.assume(lessThan(sum, LinearExpression(Rational(5))));
    octagonEquivalent.assume(
        lessEqual(difference, LinearExpression(Rational(4))));
    octagonEquivalent.assume(
        lessEqual(difference, LinearExpression(Rational(3))));
    require(octagon.isEquivalentTo(octagonEquivalent) == CheckResult::True &&
                octagon.hash() == octagonEquivalent.hash(),
            "equivalent Octagon construction histories must have equal hashes");

    ConvexPolyhedraState polyhedronEquivalent =
        ConvexPolyhedraState::top(environment);
    LinearExpression scaledGeneral = general * Rational(2);
    polyhedronEquivalent.assume(atLeast(x, Rational(-2)));
    polyhedronEquivalent.assume(
        lessEqual(scaledGeneral, LinearExpression(Rational(11))));
    polyhedronEquivalent.assume(
        lessEqual(general, LinearExpression(Rational(100))));
    require(polyhedron.isEquivalentTo(polyhedronEquivalent) ==
                    CheckResult::True &&
                polyhedron.hash() == polyhedronEquivalent.hash(),
            "equivalent Polyhedra representations must have equal hashes");

    NumericalState::RawBuffer corrupt = polyhedron.serializeRaw();
    corrupt[12] ^= 0x40U;
    requireThrows([&] { (void)NumericalState::deserializeRaw(corrupt); },
                  "raw deserialization must reject checksum corruption");

    NumericalState::RawBuffer truncated = polyhedron.serializeRaw();
    truncated.resize(truncated.size() - 3);
    requireThrows([&] { (void)NumericalState::deserializeRaw(truncated); },
                  "raw deserialization must reject truncation");
    requireThrows(
        [&] {
            (void)NumericalState::deserializeRaw(NumericalState::RawBuffer{});
        },
        "raw deserialization must reject an empty buffer");
}

void testRandomizedNumericalSoundness()
{
    const Variable x(1);
    const Variable y(2);
    const VariableEnvironment environment(
        {{x, NumericType::real(), "x"}, {y, NumericType::real(), "y"}});
    Z3SoundnessChecker checker(environment);
    std::mt19937 generator(0x5A17U);
    std::uniform_int_distribution<int> value(-8, 8);

    for (unsigned iteration = 0; iteration < 4; ++iteration)
    {
        const Rational ax(value(generator));
        const Rational ay(value(generator));
        const Rational bx(value(generator));
        const Rational by(value(generator));

        ConvexPolyhedraState lhs = ConvexPolyhedraState::top(environment);
        lhs.assume(equalsValue(x, ax));
        lhs.assume(equalsValue(y, ay));
        ConvexPolyhedraState rhs = ConvexPolyhedraState::top(environment);
        rhs.assume(equalsValue(x, bx));
        rhs.assume(equalsValue(y, by));
        const ConvexPolyhedraState hull = lhs.join(rhs);
        requireProof(checker.checkJoin(lhs, rhs, hull));

        BoxState lhsBox = BoxState::top(environment);
        lhsBox.assume(equalsValue(x, ax));
        lhsBox.assume(equalsValue(y, ay));
        BoxState rhsBox = BoxState::top(environment);
        rhsBox.assume(equalsValue(x, bx));
        rhsBox.assume(equalsValue(y, by));
        const BoxState boxJoin = lhsBox.join(rhsBox);
        requireProof(checker.checkJoin(lhsBox, rhsBox, boxJoin));

        LinearExpression assignment(x);
        assignment.setCoefficient(y, Rational(value(generator)));
        assignment.setConstant(Rational(value(generator)));
        ConvexPolyhedraState assigned = hull;
        assigned.assign(x, assignment);
        requireProof(checker.checkAssignment(hull, x, assignment, assigned));
    }
}

void testNonRelationalState()
{
    const Location first(10);
    const Location second(20);
    AddressSet addresses = AddressSet::singleton(first);
    AddressSet alternatives = AddressSet::singleton(second);
    addresses.joinWith(alternatives);
    require(addresses.locations().size() == 2 && addresses.contains(first) &&
                addresses.contains(second),
            "AddressSet join must union finite location sets");
    addresses.meetWith(AddressSet::singleton(first));
    require(addresses == AddressSet::singleton(first),
            "AddressSet meet must intersect finite location sets");
    AddressSet unknown = AddressSet::top();
    require(unknown.isTop() && unknown.contains(first) &&
                AddressSet::bottom().isSubsetOf(unknown),
            "AddressSet top/bottom ordering must hold");
    requireThrows([&] { (void)unknown.locations(); },
                  "top AddressSet must not expose a finite enumeration");
    unknown.meetWith(AddressSet::singleton(second));
    require(unknown.isSingleton() &&
                unknown.toString() == AddressSet::singleton(second).toString(),
            "meeting top with a singleton must retain the singleton");

    const Variable pointer(1);
    AddressState addressBottom = AddressState::bottom();
    addressBottom.assign(pointer, AddressSet::singleton(first));
    AddressState addressAlternative = AddressState::bottom();
    addressAlternative.assign(pointer, AddressSet::singleton(second));
    AddressState addressJoin = addressBottom;
    addressJoin.joinWith(addressAlternative);
    require(addressJoin.addressesOf(pointer).locations().size() == 2 &&
                addressBottom.isSubsetOf(addressJoin) == CheckResult::True,
            "AddressState join and subset must be pointwise");
    addressJoin.meetWith(addressBottom);
    require(addressJoin.isEquivalentTo(addressBottom) == CheckResult::True,
            "AddressState meet must recover the common singleton");
    AddressState addressTop = AddressState::top();
    addressTop.narrowWith(addressBottom);
    require(addressTop.isEquivalentTo(addressBottom) == CheckResult::True,
            "AddressState narrowing must use pointwise meet");
    addressTop.forget(pointer);
    require(addressTop.addressesOf(pointer).isTop(),
            "AddressState forget must assign unknown addresses");

    LifetimeState lifetime = LifetimeState::bottom();
    lifetime.allocate(first);
    require(!lifetime.mayBeFreed(first),
            "a newly allocated location must be alive");
    lifetime.release(first);
    require(lifetime.mustBeFreed(first),
            "a released singleton location must be definitely freed");
    LifetimeState alive = LifetimeState::bottom();
    alive.allocate(first);
    LifetimeState freed = alive;
    freed.release(first);
    LifetimeState maybe = alive;
    maybe.joinWith(freed);
    require(maybe.statusOf(first) == Lifetime::MaybeFreed &&
                alive.isSubsetOf(maybe) == CheckResult::True &&
                std::string(toString(maybe.statusOf(first))) == "maybe-freed",
            "Lifetime join must preserve both alive and freed paths");
    maybe.narrowWith(freed);
    require(maybe.isEquivalentTo(freed) == CheckResult::True,
            "Lifetime narrowing must meet a more precise successor");

    const Variable source(2);
    const Variable target(3);
    const Variable cell(4);
    const Variable secondCell(5);
    const VariableEnvironment environment(
        {{pointer, NumericType::integer(), "pointer"},
         {source, NumericType::integer(), "source"},
         {target, NumericType::integer(), "target"},
         {cell, NumericType::integer(), "cell"},
         {secondCell, NumericType::integer(), "second_cell"}});
    DomainProductState<BoxState> state(
        BoxState::top(environment),
        MemoryLayout({{first, cell}, {second, secondCell}}));
    state.allocate(first);
    state.assignAddress(pointer, AddressSet::singleton(first));
    state.assignNumeric(source, LinearExpression(Rational(7)));
    state.store(pointer, source);
    state.load(target, pointer);
    const Interval loaded = state.numerical().bound(target);
    require(loaded.lower().isFinite() && loaded.upper().isFinite() &&
                loaded.lower().value() == Rational(7) &&
                loaded.upper().value() == Rational(7),
            "strong store followed by load must preserve the exact value");
    state.release(pointer);
    require(state.lifetimes().mustBeFreed(first),
            "release through a singleton address must update lifetime");

    state.allocate(second);
    state.assignNumeric(source, LinearExpression(Rational(9)));
    state.assignNumeric(cell, LinearExpression(Rational(1)));
    state.assignNumeric(secondCell, LinearExpression(Rational(2)));
    AddressSet twoTargets = AddressSet::singleton(first);
    twoTargets.insert(second);
    state.assignAddress(pointer, twoTargets);
    state.store(pointer, source);
    require(state.numerical().bound(cell).lower().value() == Rational(1) &&
                state.numerical().bound(cell).upper().value() == Rational(9) &&
                state.numerical().bound(secondCell).lower().value() ==
                    Rational(2) &&
                state.numerical().bound(secondCell).upper().value() ==
                    Rational(9),
            "multi-target stores must weakly update every possible cell");

    DomainProductState<BoxState> parallelState = state;
    parallelState.assignAddress(source, AddressSet::singleton(first));
    parallelState.assignAddress(target, AddressSet::singleton(second));
    parallelState.assignNumericParallel(
        {{source, LinearExpression(Rational(3))},
         {target, LinearExpression(Rational(4))}});
    require(parallelState.addresses().addressesOf(source).isBottom() &&
                parallelState.addresses().addressesOf(target).isBottom(),
            "parallel numeric assignment must clear every target's address "
            "facts in the product state");

    DomainProductState<BoxState> joined = state;
    DomainProductState<BoxState> alternative = state;
    alternative.assignNumeric(source, LinearExpression(Rational(11)));
    joined.joinWith(alternative);
    require(alternative.isSubsetOf(joined) == CheckResult::True &&
                std::string(joined.name()) == "DomainProductState",
            "domain-product lattice operations must include every component");
    requireThrows([&] { (void)state.memoryLayout().contentOf(Location(99)); },
                  "MemoryLayout must reject unknown locations");
}

} // namespace

int main()
{
    try
    {
        testBoxState();
        testBoxLatticeAndFallbacks();
        testConvexPolyhedraState();
        testPolyhedraLatticeAndFallbacks();
        testParallelAssignments();
        testCompletedNumericalSurface();
        testNumericalHashAndRawSerialization();
        testRandomizedNumericalSoundness();
        testNonRelationalState();
        std::cout << "abstract-domain state tests: PASS\n";
        return EXIT_SUCCESS;
    }
    catch (const std::exception& error)
    {
        std::cerr << "abstract-domain state tests: FAIL: " << error.what()
                  << '\n';
        return EXIT_FAILURE;
    }
}
