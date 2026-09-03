//===- CoreDomainTest.cpp -- Abstract, Box, and Address domain tests ----===//

#include "AE/Core/AddressDomain.h"
#include "AE/Core/BoxProgramState.h"
#include "AE/Core/NumericalDomain.h"

#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace SVF::AbstractDomain;

namespace
{
void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

template <typename Action>
void requireThrows(Action&& action, const std::string& message)
{
    try
    {
        action();
    }
    catch (const std::exception&)
    {
        return;
    }
    throw std::runtime_error(message);
}

LinearConstraint atLeast(Variable variable, const Rational& value)
{
    return greaterEqual(LinearExpression(variable), LinearExpression(value));
}

LinearConstraint atMost(Variable variable, const Rational& value)
{
    return lessEqual(LinearExpression(variable), LinearExpression(value));
}

bool hasBounds(const Interval& interval, const Rational& lower,
               const Rational& upper)
{
    return interval.lower().isFinite() && interval.upper().isFinite() &&
           interval.lower().value() == lower &&
           interval.upper().value() == upper;
}

Interval integerInterval(std::int64_t value)
{
    return Interval::singleton(Rational(value));
}

void testScalarTransferOperations()
{
    const Interval two = integerInterval(2);
    const Interval four = integerInterval(4);
    require(add(two, four) == integerInterval(6) &&
                subtract(four, two) == two &&
                multiply(two, four) == integerInterval(8) &&
                divide(four, two) == two &&
                remainder(integerInterval(5), two) == integerInterval(1),
            "native scalar arithmetic produced an incorrect singleton");

    require(bitwiseAnd(integerInterval(6), integerInterval(3)) ==
                    integerInterval(2) &&
                bitwiseOr(integerInterval(4), integerInterval(1)) ==
                    integerInterval(5) &&
                bitwiseXor(integerInterval(7), integerInterval(3)) ==
                    integerInterval(4) &&
                shiftLeft(two, integerInterval(3)) == integerInterval(16) &&
                shiftRight(integerInterval(15), two) == integerInterval(3),
            "native bitwise or shift transfer produced an incorrect result");
    require(
        hasBounds(shiftLeft(Interval::closed(Rational(1), Rational(3)),
                            Interval::closed(Rational(1), Rational(2))),
                  Rational(2), Rational(12)) &&
            hasBounds(shiftRight(Interval::closed(Rational(-17), Rational(15)),
                                 Interval::closed(Rational(1), Rational(2))),
                      Rational(-9), Rational(7)) &&
            hasBounds(bitwiseOr(Interval::closed(Rational(1), Rational(3)),
                                Interval::closed(Rational(4), Rational(4))),
                      Rational(0), Rational(7)),
        "native range bitwise or shift transfer was not sound");

    const Interval low = Interval::closed(Rational(0), Rational(3));
    const Interval high = Interval::closed(Rational(5), Rational(8));
    require(equalTo(two, two) == integerInterval(1) &&
                notEqualTo(low, high) == integerInterval(1) &&
                lessThan(low, high) == integerInterval(1) &&
                greaterEqual(high, low) == integerInterval(1) &&
                equalTo(low, Interval::closed(Rational(2), Rational(6))) ==
                    Interval::closed(Rational(0), Rational(1)),
            "native comparison transfer lost definite or unknown outcomes");

    const Interval closedZeroOne = Interval::closed(Rational(0), Rational(1));
    const Interval openZeroOne(Bound::finite(Rational(0), true),
                               Bound::finite(Rational(1)));
    Interval joined = openZeroOne;
    joined.joinWith(closedZeroOne);
    Interval met = closedZeroOne;
    met.meetWith(openZeroOne);
    require(openZeroOne.isSubsetOf(closedZeroOne) && joined == closedZeroOne &&
                met == openZeroOne,
            "native interval lattice mishandled a strict lower bound");
    require(lessThan(Interval(Bound::finite(Rational(0)),
                              Bound::finite(Rational(1), true)),
                     Interval::closed(Rational(1), Rational(2))) ==
                integerInterval(1),
            "native interval comparison ignored a strict endpoint");
}

void testLatticeAndTransferSurface()
{
    const Variable x(1);
    const Variable y(2);
    const Variable z(3);
    const VariableEnvironment environment({{x, NumericType::integer(), "x"},
                                           {y, NumericType::integer(), "y"},
                                           {z, NumericType::real(), "z"}});

    BoxDomain state = BoxDomain::top(environment);
    require(state.kind() == DomainKind::Box,
            "Box property reported the wrong DomainKind");
    const std::unique_ptr<AbstractDomain> cloned = state.clone();
    require(cloned->isDomain<BoxDomain>() && cloned->kind() == DomainKind::Box,
            "AbstractDomain clone lost the concrete Box property type");
    state.assume(atLeast(x, Rational(0)));
    state.assume(atMost(x, Rational(10)));
    state.assign(y, LinearExpression(x) + LinearExpression(Rational(2)));
    require(hasBounds(state.bound(x), Rational(0), Rational(10)) &&
                hasBounds(state.bound(y), Rational(2), Rational(12)),
            "Box assumptions and affine assignment lost interval bounds");
    require(hasBounds(state.bound(LinearExpression(x) + LinearExpression(y)),
                      Rational(2), Rational(22)),
            "Box expression bounds did not use all terms");

    BoxDomain simultaneous = state;
    simultaneous.assignParallel(
        {{x, LinearExpression(y)}, {y, LinearExpression(x)}});
    require(hasBounds(simultaneous.bound(x), Rational(2), Rational(12)) &&
                hasBounds(simultaneous.bound(y), Rational(0), Rational(10)),
            "Box parallel assignment was not simultaneous");

    BoxDomain post = BoxDomain::top(environment);
    post.assume(atLeast(y, Rational(5)));
    post.assume(atMost(y, Rational(7)));
    post.substitute(y, LinearExpression(x) + LinearExpression(Rational(1)));
    require(hasBounds(post.bound(x), Rational(4), Rational(6)),
            "Box backward substitution computed the wrong preimage");

    BoxDomain alternative = BoxDomain::top(environment);
    alternative.assume(atLeast(x, Rational(5)));
    alternative.assume(atMost(x, Rational(20)));
    const BoxDomain joined = state.join(alternative);
    const BoxDomain met = state.meet(alternative);
    require(hasBounds(joined.bound(x), Rational(0), Rational(20)) &&
                hasBounds(met.bound(x), Rational(5), Rational(10)),
            "Box join/meet did not compute interval hull/intersection");
    require(state.isSubsetOf(joined) == CheckResult::True &&
                met.isSubsetOf(state) == CheckResult::True,
            "Box lattice ordering disagrees with join/meet");

    const BoxDomain widened = state.widen(alternative);
    require(widened.bound(x).upper().isPlusInfinity(),
            "Box widening did not extrapolate an unstable upper bound");
    require(widened.narrow(alternative).bound(x).upper().value() ==
                Rational(20),
            "Box narrowing did not recover the finite successor bound");

    BoxDomain contradiction = BoxDomain::top(environment);
    contradiction.assume(atLeast(x, Rational(2)));
    contradiction.assume(atMost(x, Rational(1)));
    require(contradiction.isBottom(),
            "Box failed to detect contradictory bounds");
}

void testEnvironmentExpandFoldAndTrees()
{
    const Variable x(1);
    const Variable y(2);
    const Variable copy(4097);
    const VariableEnvironment base(
        {{x, NumericType::integer(), "x"}, {y, NumericType::integer(), "y"}});
    BoxDomain state = BoxDomain::top(base);
    state.assume(atLeast(x, Rational(1)));
    state.assume(atMost(x, Rational(3)));
    state.expand(x, {{copy, NumericType::integer(), "copy"}});
    require(hasBounds(state.bound(copy), Rational(1), Rational(3)),
            "Box expand did not duplicate the source interval");
    state.assume(atLeast(copy, Rational(2)));
    state.fold(x, {copy});
    require(!state.environment().contains(copy) &&
                hasBounds(state.bound(x), Rational(1), Rational(3)),
            "Box fold did not merge and remove the expanded dimension");

    const VariableEnvironment extended =
        state.environment().add({{copy, NumericType::integer(), "copy"}});
    state.changeEnvironment(extended, true);
    require(hasBounds(state.bound(copy), Rational(0), Rational(0)),
            "Box environment extension did not initialize a new variable");
    state.changeEnvironment(base);
    require(!state.environment().contains(copy),
            "Box environment projection retained a removed variable");
    requireThrows(
        [&] {
            state.changeEnvironment(
                VariableEnvironment({{x, NumericType::real(), "x"},
                                     {y, NumericType::integer(), "y"}}));
        },
        "Box accepted an environment type change");

    TreeExpression xTree = TreeExpression::variable(x, NumericType::integer());
    TreeExpression two =
        TreeExpression::constant(Rational(2), NumericType::integer());
    state.assign(y, TreeExpression::binary(BinaryOperator::Multiply, xTree, two,
                                           NumericType::integer()));
    require(hasBounds(state.bound(y), Rational(2), Rational(6)),
            "Box nonlinear tree interval evaluation lost finite bounds");
}

void testPagedCopyOnWriteAndSerialization()
{
    std::vector<VariableDeclaration> declarations;
    for (std::uint32_t id = 0; id < 256; ++id)
        declarations.push_back({Variable(id * 17 + 1), NumericType::integer(),
                                "v" + std::to_string(id)});
    const VariableEnvironment environment(std::move(declarations));
    const Variable first = environment.variableOf(0);
    const Variable distant = environment.variableOf(200);

    BoxDomain original = BoxDomain::top(environment);
    original.assume(atLeast(first, Rational(1)));
    original.assume(atMost(first, Rational(3)));
    original.assume(atLeast(distant, Rational(9)));
    original.assume(atMost(distant, Rational(11)));
    BoxDomain copy = original;
    copy.assign(first, LinearExpression(Rational(7)));
    require(hasBounds(original.bound(first), Rational(1), Rational(3)) &&
                hasBounds(copy.bound(first), Rational(7), Rational(7)) &&
                hasBounds(copy.bound(distant), Rational(9), Rational(11)),
            "paged Box COW mutated a source or detached unrelated data");

    const NumericalDomain::RawBuffer raw = original.serializeRaw();
    std::unique_ptr<NumericalDomain> restored =
        NumericalDomain::deserializeRaw(raw);
    require(restored->isDomain<BoxDomain>() &&
                restored->isEquivalentTo(original) == CheckResult::True &&
                restored->hash() == original.hash(),
            "Box raw round-trip changed semantic state or hash");
    NumericalDomain::RawBuffer corrupt = raw;
    corrupt[corrupt.size() / 2] ^= 1U;
    requireThrows([&] { (void)NumericalDomain::deserializeRaw(corrupt); },
                  "Box raw deserialization accepted corrupt data");
}

void testProgramStateMemoryFacet()
{
    const Variable pointer(1);
    const Variable source(2);
    const Variable target(3);
    const Variable cell(4);
    const VariableEnvironment environment(
        {{pointer, NumericType::integer(), "pointer"},
         {source, NumericType::integer(), "source"},
         {target, NumericType::integer(), "target"},
         {cell, NumericType::integer(), "cell"}});
    const Location object(10);
    BoxProgramState state(BoxDomain::top(environment),
                          MemoryLayout({{object, cell}}));
    state.allocate(object);
    state.assignPointer(pointer, AddressSet::singleton(object));
    state.assignNumeric(source, LinearExpression(Rational(7)));
    state.store(pointer, source);
    state.load(target, pointer);
    require(
        hasBounds(state.numerical().bound(target), Rational(7), Rational(7)),
        "Box program state did not preserve a strong store/load");
    state.release(pointer);
    require(state.lifetimes().mustBeFreed(object),
            "Box program state did not preserve released-memory status");

    BoxProgramState other = state;
    other.assignNumeric(source, LinearExpression(Rational(9)));
    BoxProgramState joined = state;
    joined.joinWith(other);
    require(other.isSubsetOf(joined) == CheckResult::True,
            "Box program-state join omitted a component");
}

void testAddressDomain()
{
    const Variable p(1);
    const Variable q(2);
    const Location first(10);
    const Location second(20);
    const VariableEnvironment environment(
        {{p, NumericType::integer(), "p"}, {q, NumericType::integer(), "q"}});

    AddressDomain addresses = AddressDomain::bottom(environment);
    require(addresses.kind() == DomainKind::Address && addresses.isBottom() &&
                addresses.addressSet(p).isBottom(),
            "Address bottom did not define the vocabulary-wide default");
    require(
        Location::null().isNull() &&
            AddressSet::singleton(Location::null()).contains(Location::null()),
        "Address domain did not preserve the explicit null location");

    addresses.assign(p, AddressSet::singleton(first));
    AddressDomain copy = addresses;
    copy.assign(p, AddressSet::singleton(second));
    require(addresses.addressSet(p).contains(first) &&
                !addresses.addressSet(p).contains(second) &&
                copy.addressSet(p).contains(second),
            "Address copy-on-write changed the source property");

    AddressDomain joined = addresses;
    joined.joinWith(copy);
    require(joined.addressSet(p).contains(first) &&
                joined.addressSet(p).contains(second) &&
                joined.addressSet(p).hasIntersection(addresses.addressSet(p)) &&
                addresses.isSubsetOf(joined) == CheckResult::True &&
                copy.isSubsetOf(joined) == CheckResult::True,
            "Address join or ordering lost a possible location");

    AddressDomain met = joined;
    met.meetWith(addresses);
    require(met.isEquivalentTo(addresses) == CheckResult::True,
            "Address meet did not compute set intersection");

    const VariableEnvironment projected({{q, NumericType::integer(), "q"}});
    joined.changeEnvironment(projected);
    require(!joined.environment().contains(p) && joined.isBottom(),
            "Address environment projection retained an out-of-scope fact");
    requireThrows([&] { (void)joined.addressSet(p); },
                  "Address query accepted a variable outside its environment");

    AddressDomain incompatible = AddressDomain::top(projected);
    requireThrows([&] { incompatible.joinWith(addresses); },
                  "Address join silently guessed an environment conversion");
    BoxDomain numerical = BoxDomain::top(projected);
    requireThrows([&] { incompatible.joinWith(numerical); },
                  "AbstractDomain accepted a cross-kind lattice operation");
}
} // namespace

int main()
{
    try
    {
        testLatticeAndTransferSurface();
        testScalarTransferOperations();
        testEnvironmentExpandFoldAndTrees();
        testPagedCopyOnWriteAndSerialization();
        testProgramStateMemoryFacet();
        testAddressDomain();
        std::cout << "SVF AE core domain test: PASS\n";
        return EXIT_SUCCESS;
    }
    catch (const std::exception& error)
    {
        std::cerr << "SVF AE core domain test: FAIL: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
