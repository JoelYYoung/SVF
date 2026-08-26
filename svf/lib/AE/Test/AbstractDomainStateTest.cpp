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
    BoxState intervalized = BoxState::top(integers, config);
    const TreeExpression xTree =
        TreeExpression::variable(x, NumericType::integer());
    const TreeExpression yTree =
        TreeExpression::variable(y, NumericType::integer());
    const TreeExpression nonlinear = TreeExpression::binary(
        BinaryOperator::Multiply, xTree, yTree, NumericType::integer());
    intervalized.assign(x, nonlinear);
    intervalized.assume(TreeConstraint(nonlinear, ConstraintKind::LessEqual));
    require(intervalized.bound(x).isTop() &&
                diagnostics->diagnostics.size() == 2,
            "unbounded nonlinear Box operations must report intervalization");

    const Variable z(3);
    const VariableEnvironment extended({{x, NumericType::integer(), "x"},
                                        {y, NumericType::integer(), "y"},
                                        {z, NumericType::integer(), "z"}});
    intervalized.changeEnvironment(extended, true);
    require(intervalized.bound(z).lower().value() == Rational(0) &&
                intervalized.bound(z).upper().value() == Rational(0),
            "requested Box environment projection must initialize new symbols");
    requireThrows([&] { intervalized.forget(Variable(99)); },
                  "Box must reject unknown variables");
}

void testPagedBoxCopyOnWrite()
{
    std::vector<VariableDeclaration> declarations;
    declarations.reserve(130);
    for (std::uint32_t id = 1; id <= 130; ++id)
        declarations.push_back(
            {Variable(id), NumericType::integer(), "v" + std::to_string(id)});
    const VariableEnvironment environment(std::move(declarations));
    const Variable first(1);
    const Variable middle(65);
    const Variable last(130);

    BoxState original = BoxState::top(environment);
    original.assume(equalsValue(first, Rational(1)));
    original.assume(equalsValue(middle, Rational(2)));
    original.assume(equalsValue(last, Rational(3)));

    BoxState copy = original;
    copy.assign(first, LinearExpression(Rational(9)));
    copy.forget(middle);
    require(original.bound(first) == Interval::singleton(Rational(1)) &&
                original.bound(middle) == Interval::singleton(Rational(2)) &&
                copy.bound(first) == Interval::singleton(Rational(9)) &&
                copy.bound(middle).isTop() &&
                copy.bound(last) == Interval::singleton(Rational(3)),
            "paged Box copies must detach only mutated bounds");

    BoxState alternative = BoxState::top(environment);
    alternative.assume(equalsValue(first, Rational(1)));
    alternative.assume(equalsValue(last, Rational(4)));
    const BoxState joined = original.join(alternative);
    require(joined.bound(first) == Interval::singleton(Rational(1)) &&
                joined.bound(middle).isTop() &&
                joined.bound(last).lower().value() == Rational(3) &&
                joined.bound(last).upper().value() == Rational(4),
            "paged Box join must discard one-sided finite dimensions");

    alternative.forget(last);
    alternative.assume(equalsValue(middle, Rational(2)));
    const BoxState met = original.meet(alternative);
    require(met.isEquivalentTo(original) == CheckResult::True,
            "paged Box meet must preserve and import sparse bounds");

    std::vector<VariableDeclaration> reducedDeclarations;
    reducedDeclarations.push_back({last, NumericType::integer(), "v130"});
    reducedDeclarations.push_back({first, NumericType::integer(), "v1"});
    BoxState projected = original;
    projected.changeEnvironment(
        VariableEnvironment(std::move(reducedDeclarations)));
    require(projected.bound(first) == Interval::singleton(Rational(1)) &&
                projected.bound(last) == Interval::singleton(Rational(3)),
            "paged Box environment changes must remap active dimensions");
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
        BinaryOperator::Add, TreeExpression::variable(x, NumericType::real()),
        TreeExpression::constant(Rational(2), NumericType::real()),
        NumericType::real());
    require(state.bound(affineTree).lower().value() == Rational(3) &&
                state.bound(affineTree).upper().value() == Rational(3),
            std::string(state.name()) +
                " must lower affine tree bounds through the same path");
    const TreeExpression nonlinearTree = TreeExpression::binary(
        BinaryOperator::Multiply,
        TreeExpression::variable(x, NumericType::real()),
        TreeExpression::variable(y, NumericType::real()), NumericType::real());
    require(state.bound(nonlinearTree) == Interval::singleton(Rational(4)),
            std::string(state.name()) +
                " must evaluate a bounded nonlinear tree");
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
    requireProof(checker.checkParallelSubstitution(backwardPost,
                                                   {{x, yPlusOne}}, backward));
    require(backward.entails(equalsValue(y, Rational(9))) == CheckResult::True,
            std::string(state.name()) +
                " must compute the affine assignment preimage");

    State parallel = State::top(state.environment(), state.config());
    parallel.assume(equalsValue(x, Rational(1)));
    parallel.assume(equalsValue(y, Rational(2)));
    const State parallelPost = parallel;
    const LinearAssignmentList replacements{{x, LinearExpression(y)},
                                            {y, LinearExpression(x)}};
    parallel.substituteParallel(replacements);
    requireProof(checker.checkParallelSubstitution(parallelPost, replacements,
                                                   parallel));
    require(
        parallel.entails(equalsValue(x, Rational(2))) == CheckResult::True &&
            parallel.entails(equalsValue(y, Rational(1))) == CheckResult::True,
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
    checkExpressionBoundsAndSubstitution(ConvexPolyhedraState::top(reals), x,
                                         y);

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
    ConvexPolyhedraState relationalPolyhedra = ConvexPolyhedraState::top(reals);
    relationalPolyhedra.assume(
        equal(relationalSum, LinearExpression(Rational(5))));
    const Interval polyhedralSum = relationalPolyhedra.bound(relationalSum);
    require(polyhedralSum.lower().value() == Rational(5) &&
                polyhedralSum.upper().value() == Rational(5),
            "Polyhedra expression bounds must optimize the full affine "
            "objective without intervalizing its variables");

    const auto checkClosure = [&](auto state) {
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

    BoxState left =
        BoxState::top(VariableEnvironment({{x, NumericType::real(), "x"}}));
    BoxState right =
        BoxState::top(VariableEnvironment({{y, NumericType::real(), "y"}}));
    left.assume(equalsValue(x, Rational(1)));
    right.assume(equalsValue(y, Rational(2)));
    const VariableEnvironment unified = left.unifyEnvironmentWith(right);
    require(unified.size() == 2 && left.environment() == unified &&
                right.environment() == unified && left.bound(y).isTop() &&
                right.bound(x).isTop(),
            "one-call environment unification must lift both states");
    const BoxState combined = left.meet(right);
    require(
        combined.entails(equalsValue(x, Rational(1))) == CheckResult::True &&
            combined.entails(equalsValue(y, Rational(2))) == CheckResult::True,
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
    const BoxState boxThreshold =
        boxCurrent.widen(boxNext, WideningPolicy({}, {linearThreshold}));
    require(boxThreshold.bound(x).upper().value() == Rational(10) &&
                boxThreshold.bound(y).upper().value() == Rational(10),
            "Box must retain the representable interval consequence of a "
            "general linear widening threshold");

    OctagonState octCurrent = OctagonState::top(reals);
    octCurrent.assume(lessEqual(sum, LinearExpression(Rational(1))));
    OctagonState octNext = OctagonState::top(reals);
    octNext.assume(lessEqual(sum, LinearExpression(Rational(2))));
    const OctagonState octThreshold =
        octCurrent.widen(octNext, WideningPolicy({}, {linearThreshold}));
    require(octThreshold.entails(linearThreshold) == CheckResult::True,
            "Octagon must retain a representable general linear threshold");

    const VariableEnvironment integers({{x, NumericType::integer(), "x"}});
    ConvexPolyhedraState tightened = ConvexPolyhedraState::top(integers);
    LinearExpression twiceX(x);
    twiceX *= Rational(2);
    tightened.assume(lessEqual(twiceX, LinearExpression(Rational(3))));
    require(tightened.bound(x).upper().value() == Rational(1) &&
                !tightened.bound(x).upper().isStrict() &&
                tightened.capabilities().integerTightening,
            "integer Polyhedra must apply per-row gcd tightening");

    ConvexPolyhedraConfig rationalConfig;
    rationalConfig.integerTightening = false;
    ConvexPolyhedraState untightened =
        ConvexPolyhedraState::top(integers, rationalConfig);
    untightened.assume(lessEqual(twiceX, LinearExpression(Rational(3))));
    require(untightened.bound(x).upper().value() == Rational("3/2") &&
                !untightened.capabilities().integerTightening,
            "Polyhedra integer tightening must remain an explicit policy");
    std::unique_ptr<NumericalState> restored =
        NumericalState::deserializeRaw(untightened.serializeRaw());
    require(!restored->capabilities().integerTightening &&
                restored->isEquivalentTo(untightened) == CheckResult::True,
            "raw serialization must preserve the Polyhedra tightening policy");

    const DomainCapabilities capabilities = tightened.capabilities();
    require(capabilities.expressionBounds && capabilities.backwardAssignments &&
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
            "Polyhedra join must compute the convex hull of two points: " +
                hull.toString() + ", x=" + hull.bound(x).toString());

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
    ConvexPolyhedraState intervalized =
        ConvexPolyhedraState::top(environment, config);
    const TreeExpression xTree =
        TreeExpression::variable(x, NumericType::real());
    const TreeExpression yTree =
        TreeExpression::variable(y, NumericType::real());
    const TreeExpression nonlinear = TreeExpression::binary(
        BinaryOperator::Multiply, xTree, yTree, NumericType::real());
    intervalized.assign(x, nonlinear);
    intervalized.assume(TreeConstraint(nonlinear, ConstraintKind::LessEqual));
    intervalized.assume(notEqual(LinearExpression(x), LinearExpression(y)));
    require(intervalized.bound(x).isTop() &&
                diagnostics->diagnostics.size() == 3,
            "non-convex and unbounded nonlinear operations must report "
            "approximation");

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
    const auto* restored = decoded->template isState<StateT>()
                               ? static_cast<const StateT*>(decoded.get())
                               : nullptr;
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
        restoredOctagon->isState<OctagonState>()
            ? static_cast<const OctagonState*>(restoredOctagon.get())
            : nullptr;
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

void testPolyhedraDualRepresentation()
{
    const Variable x(1);
    const Variable y(2);
    const Variable z(3);
    const Variable w(4);
    const VariableEnvironment environment({{x, NumericType::real(), "x"},
                                           {y, NumericType::real(), "y"},
                                           {z, NumericType::real(), "z"},
                                           {w, NumericType::real(), "w"}});
    Z3SoundnessChecker checker(environment);

    // A lower-dimensional, unbounded polyhedron exercises points, rays and
    // lines. Joining it with itself forces H -> V -> H without changing its
    // mathematical value.
    ConvexPolyhedraState halfPlane = ConvexPolyhedraState::top(environment);
    LinearExpression equality(x);
    equality.setCoefficient(y, Rational(-1));
    halfPlane.assume(equal(equality, LinearExpression(Rational(2))));
    halfPlane.assume(atLeast(z, Rational(-3)));
    const ConvexPolyhedraState roundTrip = halfPlane.join(halfPlane);
    require(roundTrip.isEquivalentTo(halfPlane) == CheckResult::True &&
                halfPlane.isEquivalentTo(roundTrip) == CheckResult::True &&
                roundTrip.hash() == halfPlane.hash(),
            "Polyhedra H/V conversion must preserve lower-dimensional "
            "unbounded states and their canonical hash: before=" +
                halfPlane.toString() + ", after=" + roundTrip.toString());
    requireProof(checker.checkJoin(halfPlane, halfPlane, roundTrip));

    // The V form of top contains a point plus a line in every dimension. A
    // finite operand must therefore be redundant in its convex hull.
    ConvexPolyhedraState point = ConvexPolyhedraState::top(environment);
    point.assumeAll({equalsValue(x, Rational(1)), equalsValue(y, Rational(2)),
                     equalsValue(z, Rational(3)), equalsValue(w, Rational(4))});
    const ConvexPolyhedraState topHull =
        ConvexPolyhedraState::top(environment).join(point);
    require(topHull.isTop(),
            "joining top through V representation must remain top");

    ConvexPolyhedraState otherPoint = ConvexPolyhedraState::top(environment);
    otherPoint.assumeAll(
        {equalsValue(x, Rational(-1)), equalsValue(y, Rational(-2)),
         equalsValue(z, Rational(-3)), equalsValue(w, Rational(-4))});
    checkRawRoundTrip(point.join(otherPoint), "V-only Convex Polyhedra hull");

    // Repeated hulls remain in V form between calls. The resulting 4D simplex
    // has five vertices and is intentionally the shape that made the old
    // lifted Fourier-Motzkin join expensive.
    ConvexPolyhedraState simplex = ConvexPolyhedraState::bottom(environment);
    const std::vector<std::vector<std::int64_t>> vertices{
        {0, 0, 0, 0}, {5, 0, 0, 0}, {0, 5, 0, 0}, {0, 0, 5, 0}, {0, 0, 0, 5}};
    for (const std::vector<std::int64_t>& vertex : vertices)
    {
        ConvexPolyhedraState next = ConvexPolyhedraState::top(environment);
        next.assumeAll({equalsValue(x, Rational(vertex[0])),
                        equalsValue(y, Rational(vertex[1])),
                        equalsValue(z, Rational(vertex[2])),
                        equalsValue(w, Rational(vertex[3]))});
        const ConvexPolyhedraState before = simplex;
        simplex = simplex.join(next);
        requireProof(checker.checkJoin(before, next, simplex));
    }
    LinearExpression sum(x);
    sum.setCoefficient(y, Rational(1));
    sum.setCoefficient(z, Rational(1));
    sum.setCoefficient(w, Rational(1));
    require(simplex.entails(atLeast(x, Rational(0))) == CheckResult::True &&
                simplex.entails(atLeast(y, Rational(0))) == CheckResult::True &&
                simplex.entails(atLeast(z, Rational(0))) == CheckResult::True &&
                simplex.entails(atLeast(w, Rational(0))) == CheckResult::True &&
                simplex.entails(lessEqual(
                    sum, LinearExpression(Rational(5)))) == CheckResult::True,
            "V-form repeated joins must recover the exact 4D simplex facets: " +
                simplex.toString());

    // Batch clipping of an already generator-backed state is the common
    // incremental Chernikova path: every new half-space updates the existing
    // saturation matrix instead of rebuilding the conversion from scratch.
    ConvexPolyhedraState clipped = simplex;
    LinearExpression xPlusY(x);
    xPlusY.setCoefficient(y, Rational(1));
    LinearExpression zPlusW(z);
    zPlusW.setCoefficient(w, Rational(1));
    const LinearConstraintSet clips{
        atMost(x, Rational(3)), atMost(y, Rational(4)),
        greaterEqual(xPlusY, LinearExpression(Rational(1))),
        lessEqual(zPlusW, LinearExpression(Rational(4)))};
    clipped.assumeAll(clips);
    for (const LinearConstraint& constraint : clips)
        require(clipped.entails(constraint) == CheckResult::True,
                "incremental V clipping must retain every batch constraint");

    ConvexPolyhedraState cap = point.join(otherPoint);
    cap.assumeAll({atLeast(x, Rational(-1)), atMost(x, Rational(0)),
                   atLeast(y, Rational(-2)), atMost(y, Rational(0))});
    const ConvexPolyhedraState met = clipped.meet(cap);
    require(met.isSubsetOf(clipped) == CheckResult::True &&
                met.isSubsetOf(cap) == CheckResult::True,
            "meeting generator-backed states must incrementally clip one V "
            "representation by the other's H representation");
    checkRawRoundTrip(met, "incrementally met Convex Polyhedra state");

    // Change the dense coordinate order while the state is V-only, remove a
    // coordinate and add both a free and a zero-initialized coordinate.
    const Variable q(5);
    const VariableEnvironment projectedEnvironment(
        {{w, NumericType::real(), "w"},
         {x, NumericType::real(), "x"},
         {z, NumericType::real(), "z"},
         {q, NumericType::real(), "q"}});
    ConvexPolyhedraState projected = simplex;
    projected.changeEnvironment(projectedEnvironment);
    require(!projected.environment().contains(y) &&
                projected.bound(q).isTop() &&
                projected.entails(atLeast(x, Rational(0))) == CheckResult::True,
            "V-only environment change must project removed variables, "
            "permute retained coordinates and add free coordinates");
    const VariableEnvironment zeroExtended =
        projectedEnvironment.add({{Variable(6), NumericType::real(), "zero"}});
    projected.changeEnvironment(zeroExtended, true);
    const Interval zeroBound = projected.bound(Variable(6));
    require(zeroBound.lower().isFinite() && zeroBound.upper().isFinite() &&
                zeroBound.lower().value() == Rational(0) &&
                zeroBound.upper().value() == Rational(0),
            "V-only zero-extending environment change must add an exact "
            "zero coordinate");

    // Alternate generator-friendly and constraint-friendly operations. Each
    // mutation must invalidate exactly the stale side of the dual cache.
    ConvexPolyhedraState alternating = simplex;
    alternating.assume(atMost(x, Rational(2)));
    const ConvexPolyhedraState beforeAssign = alternating;
    LinearExpression image(y);
    image.setCoefficient(z, Rational(2));
    image.setConstant(Rational(1));
    alternating.assign(x, image);
    requireProof(checker.checkAssignment(beforeAssign, x, image, alternating));
    alternating.forget(w);
    require(alternating.bound(w).isTop() &&
                alternating.entails(equal(LinearExpression(x) - image,
                                          LinearExpression(Rational()))) ==
                    CheckResult::True,
            "assume/assign/forget must transparently refresh H/V caches");

    // Exercise repeated representation changes. No public operation is told
    // whether H or V is currently authoritative.
    ConvexPolyhedraState chained = simplex;
    for (std::int64_t iteration = 0; iteration < 6; ++iteration)
    {
        ConvexPolyhedraState vertex = ConvexPolyhedraState::top(environment);
        vertex.assumeAll({equalsValue(x, Rational(iteration)),
                          equalsValue(y, Rational(5 - iteration)),
                          equalsValue(z, Rational(iteration % 2)),
                          equalsValue(w, Rational(0))});
        chained = chained.join(vertex);
        chained.assume(atMost(x, Rational(5)));
        LinearExpression nextY(y);
        nextY.setConstant(Rational(1));
        chained.assign(z, nextY);
        chained.forget(w);
    }
    require(chained.entails(atMost(x, Rational(5))) == CheckResult::True &&
                chained.entails(
                    equal(LinearExpression(z) - (LinearExpression(y) +
                                           LinearExpression(Rational(1))),
                          LinearExpression(Rational()))) == CheckResult::True,
            "long H/V operation chains must preserve exact affine facts");
    checkRawRoundTrip(chained, "long H/V Convex Polyhedra operation chain");

    // NNC generators distinguish included points (epsilon>0) from closure
    // points (epsilon=0), so strictness survives every V-native operation.
    ConvexPolyhedraState strict = ConvexPolyhedraState::top(environment);
    strict.assume(lessThan(LinearExpression(x), LinearExpression(Rational(0))));
    const ConvexPolyhedraState strictSelfHull = strict.join(strict);
    require(strictSelfHull.entails(
                lessThan(LinearExpression(x), LinearExpression(Rational(0)))) ==
                CheckResult::True &&
                strictSelfHull.bound(x).upper().isStrict(),
            "NNC self-join must retain an open boundary through V");
    ConvexPolyhedraState endpoint = ConvexPolyhedraState::top(environment);
    endpoint.assume(equalsValue(x, Rational(1)));
    const ConvexPolyhedraState closedHull = strict.join(endpoint);
    require(closedHull.entails(atMost(x, Rational(1))) == CheckResult::True &&
                closedHull.entails(lessThan(LinearExpression(x),
                                            LinearExpression(Rational(1)))) ==
                    CheckResult::Unknown,
            "an included endpoint must close only its own NNC hull boundary");
    strict.assign(y, LinearExpression(x));
    require(strict.entails(
                lessThan(LinearExpression(x), LinearExpression(Rational(0)))) ==
                CheckResult::True,
            "NNC V-native assignment must preserve strict constraints");
    checkRawRoundTrip(strict, "strict NNC Convex Polyhedra operation chain");
    checkRawRoundTrip(closedHull, "mixed closed/NNC Convex Polyhedra hull");

    const VariableEnvironment emptyEnvironment;
    require(ConvexPolyhedraState::top(emptyEnvironment)
                .join(ConvexPolyhedraState::top(emptyEnvironment))
                .isTop(),
            "zero-dimensional top must survive H/V conversion");

    ConvexPolyhedraState inconsistent = ConvexPolyhedraState::top(environment);
    LinearExpression inconsistentSum(x);
    inconsistentSum.setCoefficient(y, Rational(1));
    inconsistent.assumeAll(
        {equalsValue(x, Rational(0)), equalsValue(y, Rational(0)),
         equal(inconsistentSum, LinearExpression(Rational(1)))});
    require(inconsistent.isBottom(),
            "affine-hull canonicalization must retain inconsistent dependent "
            "equalities");
}

void testExtendedApronSurface()
{
    const Variable x(1);
    const Variable y(2);
    const Variable z(3);
    const Variable a(4);
    const Variable b(5);
    const VariableEnvironment realEnvironment({{x, NumericType::real(), "x"},
                                               {y, NumericType::real(), "y"},
         {z, NumericType::real(), "z"}});

    const auto checkSharedCapabilities = [&](const auto& state) {
        const DomainCapabilities capabilities = state.capabilities();
        require(capabilities.expandFold && capabilities.operationMetadata &&
                    capabilities.ieeeTreeExpressions &&
                    capabilities.nonlinearTreeExpressions,
                std::string(state.name()) +
                    " did not advertise the completed APRON surface");
    };
    checkSharedCapabilities(BoxState::top(realEnvironment));
    checkSharedCapabilities(OctagonState::top(realEnvironment));
    checkSharedCapabilities(ConvexPolyhedraState::top(realEnvironment));
    require(ConvexPolyhedraState::top(realEnvironment)
                .capabilities()
                .generatorExchange,
            "Polyhedra did not advertise public generator exchange");

    BoxState metadataCurrent = BoxState::top(realEnvironment);
    metadataCurrent.assign(x, LinearExpression(Rational(1)));
    require(metadataCurrent.lastOperation().operation ==
                    OperationKind::Assignment &&
                metadataCurrent.lastOperation().exact &&
                metadataCurrent.lastOperation().best,
            "exact affine assignment metadata is incorrect");
    BoxState metadataNext = metadataCurrent;
    metadataNext.assign(x, LinearExpression(Rational(2)));
    const BoxState metadataWidened = metadataCurrent.widen(metadataNext);
    require(metadataWidened.lastOperation().operation ==
                    OperationKind::Widening &&
                !metadataWidened.lastOperation().exact &&
                metadataWidened.lastOperation().best,
            "widening metadata must distinguish exact from best");

    const auto checkNoOpMetadata = [&](auto state) {
        state.assign(x, LinearExpression(Rational(1)));
        require(state.lastOperation().operation == OperationKind::Assignment,
                std::string(state.name()) +
                    " bottom assignment left stale metadata");
        state.assignParallel(LinearAssignmentList{});
        require(state.lastOperation().operation == OperationKind::Assignment,
                std::string(state.name()) +
                    " empty assignment batch left stale metadata");
        state.substituteParallel(LinearAssignmentList{});
        require(state.lastOperation().operation == OperationKind::Substitution,
                std::string(state.name()) +
                    " empty substitution batch left stale metadata");
        state.assumeAll({});
        require(state.lastOperation().operation == OperationKind::Assumption,
                std::string(state.name()) +
                    " empty assumption batch left stale metadata");
        state.close();
        require(state.lastOperation().operation ==
                    OperationKind::TopologicalClosure,
                std::string(state.name()) +
                    " bottom closure left stale metadata");
    };
    checkNoOpMetadata(BoxState::bottom(realEnvironment));
    checkNoOpMetadata(OctagonState::bottom(realEnvironment));
    checkNoOpMetadata(ConvexPolyhedraState::bottom(realEnvironment));

    OctagonState parallelMetadata = OctagonState::top(realEnvironment);
    parallelMetadata.assignParallel(
        {{x, LinearExpression(y)}, {y, LinearExpression(x)}});
    require(parallelMetadata.lastOperation().operation ==
                OperationKind::Assignment,
            "default parallel assignment leaked environment-change metadata");

    const TreeExpression product = TreeExpression::binary(
        BinaryOperator::Multiply,
        TreeExpression::variable(x, NumericType::real()),
        TreeExpression::variable(y, NumericType::real()), NumericType::real());
    const auto checkNonlinear = [&](auto state) {
        state.assumeAll({atLeast(x, Rational(2)), atMost(x, Rational(3)),
                         atLeast(y, Rational(-1)), atMost(y, Rational(4))});
        state.assign(z, product);
        const Interval result = state.bound(z);
        require(result.lower().isFinite() && result.upper().isFinite() &&
                    result.lower().value() <= Rational(-3) &&
                    Rational(12) <= result.upper().value(),
                std::string(state.name()) +
                    " nonlinear multiplication lost a concrete endpoint");
        require(!state.lastOperation().exact && !state.lastOperation().best &&
                    state.lastOperation().approximation ==
                        ApproximationKind::SoundOverApproximation,
                std::string(state.name()) +
                    " did not expose nonlinear approximation metadata");
    };
    checkNonlinear(BoxState::top(realEnvironment));
    checkNonlinear(OctagonState::top(realEnvironment));
    checkNonlinear(ConvexPolyhedraState::top(realEnvironment));

    BoxState parallel = BoxState::top(realEnvironment);
    parallel.assumeAll(
        {equalsValue(x, Rational(2)), equalsValue(y, Rational(3))});
    parallel.assignParallel(TreeAssignmentList{
        {x, TreeExpression::binary(
                BinaryOperator::Multiply,
                TreeExpression::variable(y, NumericType::real()),
                TreeExpression::variable(y, NumericType::real()),
                NumericType::real())},
        {y, TreeExpression::binary(
                BinaryOperator::Multiply,
                TreeExpression::variable(x, NumericType::real()),
                TreeExpression::variable(x, NumericType::real()),
                NumericType::real())}});
    require(parallel.bound(x) == Interval::singleton(Rational(9)) &&
                parallel.bound(y) == Interval::singleton(Rational(4)),
            "parallel nonlinear assignments must read one incoming state");

    BoxState impossible = BoxState::top(realEnvironment);
    impossible.assumeAll({atLeast(x, Rational(1)), atMost(x, Rational(2)),
                          atLeast(y, Rational(1)), atMost(y, Rational(3))});
    impossible.assume(TreeConstraint(product, ConstraintKind::LessEqual));
    require(impossible.isBottom(),
            "interval-disproved nonlinear guards must produce bottom");

    const TreeExpression shiftedProduct = TreeExpression::binary(
        BinaryOperator::Subtract, product,
        TreeExpression::constant(Rational(5), NumericType::real()),
        NumericType::real());
    const auto checkShiftedProductGuard = [&](auto state) {
        state.assumeAll({atLeast(x, Rational(2)), atMost(x, Rational(3)),
                         atLeast(y, Rational(2)), atMost(y, Rational(3))});
        state.assume(TreeConstraint(shiftedProduct, ConstraintKind::LessEqual));
        require(state.bound(x).upper().isFinite() &&
                    state.bound(x).upper().value() <= Rational("5/2") &&
                    state.bound(y).upper().isFinite() &&
                    state.bound(y).upper().value() <= Rational("5/2"),
                std::string(state.name()) +
                    " did not retain a shifted-product McCormick consequence");
    };
    checkShiftedProductGuard(BoxState::top(realEnvironment));
    checkShiftedProductGuard(OctagonState::top(realEnvironment));
    checkShiftedProductGuard(ConvexPolyhedraState::top(realEnvironment));

    BoxState nonlinearOperators = BoxState::top(realEnvironment);
    nonlinearOperators.assumeAll(
        {equalsValue(x, Rational(9)), equalsValue(y, Rational(4))});
    const TreeExpression quotient = TreeExpression::binary(
        BinaryOperator::Divide,
        TreeExpression::variable(x, NumericType::real()),
        TreeExpression::variable(y, NumericType::real()), NumericType::real());
    const TreeExpression remainder = TreeExpression::binary(
        BinaryOperator::Remainder,
        TreeExpression::variable(x, NumericType::real()),
        TreeExpression::variable(y, NumericType::real()), NumericType::real());
    const TreeExpression squareRoot = TreeExpression::unary(
        UnaryOperator::SquareRoot,
        TreeExpression::variable(y, NumericType::real()), NumericType::real());
    require(nonlinearOperators.bound(quotient) ==
                    Interval::singleton(Rational("9/4")) &&
                nonlinearOperators.bound(remainder) ==
                    Interval::singleton(Rational(1)) &&
                nonlinearOperators.bound(squareRoot) ==
                    Interval::singleton(Rational(2)),
            "divide, remainder, and square-root tree evaluation is incorrect");
    const TreeExpression zeroDivisor = TreeExpression::binary(
        BinaryOperator::Divide,
        TreeExpression::variable(x, NumericType::real()),
        TreeExpression::constant(Rational(), NumericType::real()),
        NumericType::real());
    require(nonlinearOperators.bound(zeroDivisor).isTop(),
            "possible division by zero must conservatively produce top");

    BoxState unboundedOperators = BoxState::top(realEnvironment);
    unboundedOperators.assumeAll({atLeast(x, Rational(2)),
                                  atLeast(y, Rational(3)),
         atMost(y, Rational(4))});
    const Interval unboundedProduct = unboundedOperators.bound(product);
    require(unboundedProduct.lower().isFinite() &&
                unboundedProduct.lower().value() == Rational(6) &&
                unboundedProduct.upper().isPlusInfinity(),
            "semi-infinite multiplication lost its finite lower endpoint");
    const Interval unboundedQuotient = unboundedOperators.bound(
        TreeExpression::binary(BinaryOperator::Divide,
            TreeExpression::variable(x, NumericType::real()),
            TreeExpression::variable(y, NumericType::real()),
            NumericType::real()));
    require(unboundedQuotient.lower().isFinite() &&
                unboundedQuotient.lower().value() == Rational("1/2") &&
                unboundedQuotient.upper().isPlusInfinity(),
            "semi-infinite division lost its finite lower endpoint");

    BoxState castSource = BoxState::top(realEnvironment);
    castSource.assumeAll(
        {atLeast(x, Rational("-3/2")), atMost(x, Rational("7/2"))});
    const TreeExpression integerCast = TreeExpression::unary(
        UnaryOperator::Cast, TreeExpression::variable(x, NumericType::real()),
        NumericType::integer(), RoundingMode::TowardZero);
    require(
        castSource.bound(integerCast) ==
            Interval(Bound::finite(Rational(-1)), Bound::finite(Rational(3))),
            "real-to-integer casts must truncate toward zero");

    const Variable f(11);
    const Variable g(12);
    const Variable h(13);
    const NumericType binary32 = NumericType::ieee(FloatFormat::binary32());
    const VariableEnvironment floatEnvironment(
        {{f, binary32, "f"}, {g, binary32, "g"}, {h, binary32, "h"}});
    BoxState floating = BoxState::top(floatEnvironment);
    floating.assign(f, TreeExpression::constant(Rational("1/10"), binary32));
    floating.assign(g, TreeExpression::constant(Rational("1/5"), binary32));
    floating.assign(h, TreeExpression::binary(
               BinaryOperator::Add,
               TreeExpression::variable(f, binary32),
               TreeExpression::variable(g, binary32), binary32));
    const Rational rounded = FloatSemantics::add(
        FloatSemantics::add(Rational("1/10"), Rational(), 24,
                            RoundingMode::NearestTiesToEven),
        FloatSemantics::add(Rational("1/5"), Rational(), 24,
                            RoundingMode::NearestTiesToEven),
        24, RoundingMode::NearestTiesToEven);
    require(floating.bound(h).lower().isFinite() &&
                floating.bound(h).upper().isFinite() &&
                floating.bound(h).lower().value() <= rounded &&
                rounded <= floating.bound(h).upper().value(),
            "outward IEEE interval evaluation excluded the rounded result");

    const Rational halfUlp("1/16777216");
    for (RoundingMode rounding :
         {RoundingMode::NearestTiesToEven, RoundingMode::TowardZero,
          RoundingMode::TowardPositive, RoundingMode::TowardNegative})
    {
        const TreeExpression roundedAddition = TreeExpression::binary(
            BinaryOperator::Add,
            TreeExpression::constant(Rational(1), binary32),
            TreeExpression::constant(halfUlp, binary32), binary32, rounding);
        const Rational expected =
            FloatSemantics::add(Rational(1), halfUlp, 24, rounding);
        require(floating.bound(roundedAddition) ==
                    Interval::singleton(expected),
                "binary32 addition did not honor its rounding mode");
    }

    const Rational minimumSubnormal(
        "1/713623846352979940529142984724747568191373312");
    const TreeExpression subnormalTie = TreeExpression::unary(
        UnaryOperator::Cast,
        TreeExpression::constant(minimumSubnormal * Rational("3/2"),
                                 NumericType::real()),
        binary32, RoundingMode::NearestTiesToEven);
    require(floating.bound(subnormalTie) ==
                Interval::singleton(minimumSubnormal * Rational(2)),
            "binary32 subnormal ties must round to an even significand");
    floating.assign(
        h,
        TreeExpression::constant(
            Rational("10000000000000000000000000000000000000000"), binary32));
    require(floating.bound(h).isTop(),
            "IEEE overflow must conservatively produce top");

    const VariableEnvironment summaryEnvironment(
        {{x, NumericType::real(), "summary"},
         {y, NumericType::real(), "other"}});
    const std::vector<VariableDeclaration> copies{
        {a, NumericType::real(), "materialized_a"},
        {b, NumericType::real(), "materialized_b"}};
    const auto checkExpandFold = [&](auto state) {
        state.assume(equalsValue(x, Rational(1)));
        state.assume(equalsValue(y, Rational(2)));
        state.expand(x, copies);
        require(state.environment().contains(a) &&
                    state.environment().contains(b) &&
                    state.bound(a) == Interval::singleton(Rational(1)) &&
                    state.bound(b) == Interval::singleton(Rational(1)),
                std::string(state.name()) +
                    " expand did not duplicate the summary value");
        state.assign(a, LinearExpression(Rational(10)));
        state.fold(x, {a});
        require(!state.environment().contains(a) &&
                    state.environment().contains(b) &&
                    state.bound(x).lower().isFinite() &&
                    state.bound(x).upper().isFinite() &&
                    state.bound(x).lower().value() == Rational(1) &&
                    state.bound(x).upper().value() == Rational(10) &&
                    state.lastOperation().operation == OperationKind::Fold,
                std::string(state.name()) +
                    " fold did not retain every representative");
    };
    checkExpandFold(BoxState::top(summaryEnvironment));
    checkExpandFold(OctagonState::top(summaryEnvironment));
    checkExpandFold(ConvexPolyhedraState::top(summaryEnvironment));

    const VariableEnvironment generatorEnvironment(
        {{x, NumericType::real(), "x"}, {y, NumericType::real(), "y"}});
    const PolyhedraGeneratorSet nncGenerators{
        {PolyhedraGeneratorKind::ClosurePoint, {Rational(0), Rational(0)}},
        {PolyhedraGeneratorKind::Point, {Rational(1), Rational(0)}},
        {PolyhedraGeneratorKind::Ray, {Rational(0), Rational(1)}}};
    ConvexPolyhedraState generated = ConvexPolyhedraState::fromGenerators(
        generatorEnvironment, nncGenerators);
    require(
        generated.lastOperation().operation == OperationKind::GeneratorImport &&
            generated.lastOperation().exact && generated.lastOperation().best &&
                generated.bound(x).lower().isStrict() &&
                !generated.bound(x).upper().isStrict() &&
                generated.bound(x).lower().value() == Rational(0) &&
                generated.bound(x).upper().value() == Rational(1) &&
                generated.bound(y).lower().value() == Rational(0) &&
                generated.bound(y).upper().isPlusInfinity(),
            "public NNC generators lost included/closure/ray semantics");
    const PolyhedraGeneratorSet exported = generated.toGenerators();
    require(generated.lastOperation().operation ==
                OperationKind::GeneratorExport,
            "generator export did not update operation metadata");
    const ConvexPolyhedraState regenerated =
        ConvexPolyhedraState::fromGenerators(generatorEnvironment, exported);
    require(generated.isEquivalentTo(regenerated) == CheckResult::True,
            "public generator export/import changed an NNC polyhedron: " +
                generated.toString() + " versus " + regenerated.toString());

    const PolyhedraGeneratorSet lineGenerators{
        {PolyhedraGeneratorKind::Point, {Rational(0), Rational(0)}},
        {PolyhedraGeneratorKind::Line, {Rational(0), Rational(1)}}};
    const ConvexPolyhedraState line = ConvexPolyhedraState::fromGenerators(
        generatorEnvironment, lineGenerators);
    require(line.bound(x) == Interval::singleton(Rational(0)) &&
                line.bound(y).isTop(),
            "public line generator did not create a bidirectional direction");
    const ConvexPolyhedraState emptyGenerators =
        ConvexPolyhedraState::fromGenerators(generatorEnvironment, {});
    require(emptyGenerators.isBottom() &&
                emptyGenerators.lastOperation().operation ==
                    OperationKind::GeneratorImport,
            "an empty public generator system must denote bottom");
    requireThrows(
        [&] {
            (void)ConvexPolyhedraState::fromGenerators(
                generatorEnvironment,
                {{PolyhedraGeneratorKind::Ray, {Rational(1), Rational(0)}}});
        },
        "a generator system without an included point must be rejected");
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
        testPagedBoxCopyOnWrite();
        testConvexPolyhedraState();
        testPolyhedraLatticeAndFallbacks();
        testParallelAssignments();
        testCompletedNumericalSurface();
        testNumericalHashAndRawSerialization();
        testRandomizedNumericalSoundness();
        testPolyhedraDualRepresentation();
        testExtendedApronSurface();
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
