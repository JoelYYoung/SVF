//===- AbstractDomainStateTest.cpp -- Abstract-domain state tests -------===//

#include "AE/Core/BoxDomain.h"
#include "AE/Core/ConvexPolyhedraDomain.h"
#include "AE/Core/NonRelationalDomain.h"
#include "AE/Core/OctagonDomain.h"
#include "Z3SoundnessChecker.h"

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
        {{x, NumericType::integer(), "x"},
         {y, NumericType::real(), "y"}});
    requireThrows([&] { state.changeEnvironment(changedType); },
                  "Box environment changes must reject numeric type changes");
}

void testBoxLatticeAndFallbacks()
{
    const Variable x(1);
    const Variable y(2);
    const VariableEnvironment integers(
        {{x, NumericType::integer(), "x"},
         {y, NumericType::integer(), "y"}});
    IntervalBox initial;
    initial.bounds.emplace(
        x, Interval(Bound::finite(Rational("1/2")),
                    Bound::finite(Rational("7/2"))));
    BoxState finite = BoxState::fromBox(integers, initial);
    require(finite.bound(x).lower().value() == Rational(1) &&
                finite.bound(x).upper().value() == Rational(3),
            "integer Box bounds must be tightened to integral endpoints");

    BoxState top = BoxState::top(integers);
    BoxState bottom = BoxState::bottom(integers);
    require(top.isTop() && bottom.isBottom() &&
                bottom.join(finite).isEquivalentTo(finite) == CheckResult::True &&
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
    const VariableEnvironment extended(
        {{x, NumericType::integer(), "x"},
         {y, NumericType::integer(), "y"},
         {z, NumericType::integer(), "z"}});
    fallback.changeEnvironment(extended, true);
    require(fallback.bound(z).lower().value() == Rational(0) &&
                fallback.bound(z).upper().value() == Rational(0),
            "requested Box environment projection must initialize new symbols");
    requireThrows([&] { fallback.forget(Variable(99)); },
                  "Box must reject unknown variables");
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
        {{x, NumericType::integer(), "x"},
         {y, NumericType::real(), "y"}});
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

    ConvexPolyhedraState contradiction =
        ConvexPolyhedraState::top(environment);
    contradiction.assume(atMost(x, Rational(0)));
    contradiction.assume(greaterThan(LinearExpression(x),
                                     LinearExpression(Rational(0))));
    require(contradiction.isBottom() &&
                contradiction.toConstraints().size() == 1,
            "Polyhedra feasibility must detect contradictory constraints");

    LinearExpression sum(x);
    sum.setCoefficient(y, Rational(1));
    ConvexPolyhedraState current =
        ConvexPolyhedraState::top(environment);
    current.assume(
        lessEqual(sum, LinearExpression(Rational(0))));
    ConvexPolyhedraState following =
        ConvexPolyhedraState::top(environment);
    following.assume(
        lessEqual(sum, LinearExpression(Rational(1))));
    const LinearConstraint threshold =
        lessEqual(sum, LinearExpression(Rational(10)));
    WideningPolicy policy;
    policy.linearThresholds.push_back(threshold);
    policy.linearThresholds.push_back(
        lessEqual(sum, LinearExpression(Rational(0))));
    const ConvexPolyhedraState standard = current.widen(following);
    const ConvexPolyhedraState thresholded =
        current.widen(following, policy);
    Z3SoundnessChecker checker(environment);
    requireProof(checker.checkWidening(current, following, thresholded));
    require(current.capabilities().thresholdWidening &&
                standard.entails(threshold) != CheckResult::True &&
                thresholded.entails(threshold) == CheckResult::True &&
                thresholded.entails(
                    lessEqual(sum, LinearExpression(Rational(0)))) !=
                    CheckResult::True &&
                following.isSubsetOf(thresholded) == CheckResult::True,
            "Polyhedra threshold widening must retain exactly applicable "
            "linear thresholds and contain the next state");

    ConvexPolyhedraState scalarCurrent =
        ConvexPolyhedraState::top(environment);
    scalarCurrent.assume(atMost(x, Rational(1)));
    ConvexPolyhedraState scalarFollowing =
        ConvexPolyhedraState::top(environment);
    scalarFollowing.assume(atMost(x, Rational(2)));
    const ConvexPolyhedraState scalarThresholded = scalarCurrent.widen(
        scalarFollowing, WideningPolicy{{Rational(10)}});
    require(scalarThresholded.entails(atMost(x, Rational(10))) ==
                CheckResult::True,
            "Polyhedra threshold widening must support shared scalar "
            "threshold policies");
    requireProof(checker.checkWidening(
        scalarCurrent, scalarFollowing, scalarThresholded));
}

template <typename StateT>
void checkParallelAssignmentFor(const char* domainName)
{
    const Variable x(1);
    const Variable y(2);
    const Variable z(3);
    const VariableEnvironment environment(
        {{x, NumericType::integer(), "x"},
         {y, NumericType::integer(), "y"},
         {z, NumericType::integer(), "z"}});
    Z3SoundnessChecker checker(environment);

    StateT state = StateT::top(environment);
    state.assumeAll(
        {equalsValue(x, Rational(1)), equalsValue(y, Rational(2)),
         equalsValue(z, Rational(9))});
    const StateT before = state;

    LinearExpression difference(x);
    difference.setCoefficient(y, Rational(-1));
    const LinearAssignmentList assignments{
        {x, LinearExpression(y)},
        {y, LinearExpression(x)},
        {z, difference}};
    NumericalState& numerical = state;
    numerical.assignParallel(assignments);

    requireProof(
        checker.checkParallelAssignment(before, assignments, state));
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
        {{x, NumericType::integer(), "x"},
         {y, NumericType::integer(), "y"}});
    BoxState state = BoxState::top(environment);
    NumericalState& numerical = state;
    requireThrows(
        [&]
        {
            numerical.assignParallel(LinearAssignmentList{
                {x, LinearExpression(y)}, {x, LinearExpression(x)}});
        },
        "parallel assignment must reject duplicate targets");

    state.assumeAll(
        {equalsValue(x, Rational(1)), equalsValue(y, Rational(2))});
    state.assignParallel(TreeAssignmentList{
        {x, TreeExpression::variable(y, NumericType::integer())},
        {y, TreeExpression::variable(x, NumericType::integer())}});
    require(state.bound(x).lower().value() == Rational(2) &&
                state.bound(y).lower().value() == Rational(1),
            "affine tree parallel assignment must use simultaneous semantics");
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
