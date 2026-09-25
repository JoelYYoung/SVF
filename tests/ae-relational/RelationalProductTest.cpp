#include "AE/Core/BoxAddressDomain.h"
#include "AE/Core/ConvexPolyhedraDomain.h"
#include "AE/Core/Expression.h"
#include "AE/Core/OctagonDomain.h"
#include "AE/Core/PartialRelationalDomain.h"
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <vector>
namespace AD = SVF::AbstractDomain;
namespace
{
[[noreturn]] void fail(const char* message)
{
    std::cerr << "FAIL: " << message << '\n';
    std::exit(EXIT_FAILURE);
}

int signedWrap8(int value)
{
    int residue = value % 256;
    if (residue < 0)
        residue += 256;
    return residue >= 128 ? residue - 256 : residue;
}

int unsignedWrap8(int value)
{
    int residue = value % 256;
    return residue < 0 ? residue + 256 : residue;
}

void expectSingleton(const AD::Interval& interval, int expected,
                     const char* message)
{
    if (!interval.isSingleton() ||
        interval.singletonValue() != AD::Rational(expected))
        fail(message);
}

void machineInteger8Exhaustive()
{
    for (int lhs = -128; lhs <= 127; ++lhs)
    {
        expectSingleton(
            AD::zeroExtendIntegerInterval(
                AD::Interval::singleton(AD::Rational(lhs)), 8, true),
            unsignedWrap8(lhs), "i8 zero extension");
        for (int rhs = -128; rhs <= 127; ++rhs)
        {
            for (const int mathematical : {lhs + rhs, lhs - rhs, lhs * rhs})
            {
                const AD::Interval value =
                    AD::Interval::singleton(AD::Rational(mathematical));
                expectSingleton(AD::wrapIntegerInterval(value, 8, true),
                                signedWrap8(mathematical),
                                "signed i8 arithmetic wrap");
                expectSingleton(AD::wrapIntegerInterval(value, 8, false),
                                unsignedWrap8(mathematical),
                                "unsigned i8 arithmetic wrap");
            }
        }
    }

    for (int lhs = 0; lhs <= 255; ++lhs)
    {
        for (int rhs = 0; rhs <= 255; ++rhs)
        {
            for (const int mathematical : {lhs + rhs, lhs - rhs, lhs * rhs})
                expectSingleton(
                    AD::wrapIntegerInterval(
                        AD::Interval::singleton(AD::Rational(mathematical)), 8,
                        false),
                    unsignedWrap8(mathematical), "unsigned i8 arithmetic wrap");
        }
    }

    for (int value = -32768; value <= 32767; ++value)
        expectSingleton(
            AD::wrapIntegerInterval(
                AD::Interval::singleton(AD::Rational(value)), 8, true),
            signedWrap8(value), "i16-to-i8 truncation");

    for (int lower = -384; lower <= 383; ++lower)
    {
        for (int width = 0; width <= 32; ++width)
        {
            const int upper = lower + width;
            int expectedLower = 128;
            int expectedUpper = -129;
            for (int value = lower; value <= upper; ++value)
            {
                const int wrapped = signedWrap8(value);
                expectedLower = std::min(expectedLower, wrapped);
                expectedUpper = std::max(expectedUpper, wrapped);
            }
            const AD::Interval actual = AD::wrapIntegerInterval(
                AD::Interval::closed(AD::Rational(lower), AD::Rational(upper)),
                8, true);
            if (actual != AD::Interval::closed(AD::Rational(expectedLower),
                                               AD::Rational(expectedUpper)))
                fail("i8 interval wrap hull");
        }
    }

    for (int lower = -128; lower <= 127; ++lower)
    {
        for (int upper = lower; upper <= 127; ++upper)
        {
            int expectedLower = 256;
            int expectedUpper = -1;
            for (int value = lower; value <= upper; ++value)
            {
                const int extended = unsignedWrap8(value);
                expectedLower = std::min(expectedLower, extended);
                expectedUpper = std::max(expectedUpper, extended);
            }
            const AD::Interval actual = AD::zeroExtendIntegerInterval(
                AD::Interval::closed(AD::Rational(lower), AD::Rational(upper)),
                8, true);
            if (actual != AD::Interval::closed(AD::Rational(expectedLower),
                                               AD::Rational(expectedUpper)))
                fail("i8 zero-extension hull");
        }
    }
}

template <class Domain> void exactSmallModel()
{
    const AD::Variable x(41);
    const AD::Variable y(42);
    const AD::Variable z(43);
    for (int xv = -4; xv <= 4; ++xv)
    {
        for (int yv = -4; yv <= 4; ++yv)
        {
            Domain state = Domain::fromConstraints(
                {AD::equal(AD::LinearExpression(x),
                           AD::LinearExpression(AD::Rational(xv))),
                 AD::equal(AD::LinearExpression(y),
                           AD::LinearExpression(AD::Rational(yv)))});
            state.assign(z, AD::LinearExpression(x) * AD::Rational(2) -
                                AD::LinearExpression(y) +
                                AD::LinearExpression(AD::Rational(1)));
            expectSingleton(state.bound(z), 2 * xv - yv + 1,
                            "exact affine small model");
            Domain point = Domain::fromConstraints(
                {AD::equal(AD::LinearExpression(x),
                           AD::LinearExpression(AD::Rational(xv))),
                 AD::equal(AD::LinearExpression(y),
                           AD::LinearExpression(AD::Rational(yv))),
                 AD::equal(
                     AD::LinearExpression(z),
                     AD::LinearExpression(AD::Rational(2 * xv - yv + 1)))});
            if (point.isSubsetOf(state) != AD::CheckResult::True)
                fail("exact affine state lost concrete point");
        }
    }
}

void partialRelationalModel(AD::DomainKind kind)
{
    const AD::Variable x(51);
    const AD::Variable y(52);
    const AD::Variable outside(53);
    auto vocabulary = std::make_shared<const std::vector<AD::Variable>>(
        std::vector<AD::Variable>{x, y});
    AD::PartialRelationalDomain state =
        AD::PartialRelationalDomain::top(kind, vocabulary);
    state.assume(AD::greaterEqual(AD::LinearExpression(x),
                                  AD::LinearExpression(AD::Rational(0))));
    state.assume(AD::lessEqual(AD::LinearExpression(x),
                               AD::LinearExpression(AD::Rational(10))));
    state.assign(y, AD::LinearExpression(x) +
                    AD::LinearExpression(AD::Rational(1)));
    if (state.entails(AD::equal(
            AD::LinearExpression(y),
            AD::LinearExpression(x) +
            AD::LinearExpression(AD::Rational(1)))) != AD::CheckResult::True)
        fail("partial relational slice lost an inside-vocabulary relation");

    state.assign(outside, AD::LinearExpression(x) +
                          AD::LinearExpression(y));
    if (state.bound(outside) != AD::Interval::closed(
            AD::Rational(1), AD::Rational(21)))
        fail("partial relational slice lost the global Box fallback");

    state.assign(y, AD::LinearExpression(outside) +
                    AD::LinearExpression(x));
    if (state.entails(AD::equal(
            AD::LinearExpression(y),
            AD::LinearExpression(x) +
            AD::LinearExpression(AD::Rational(1)))) == AD::CheckResult::True)
        fail("cross-vocabulary assignment retained a stale relation");
    if (!state.relational().supportVariables().empty())
    {
        for (AD::Variable variable : state.relational().supportVariables())
            if (variable != x && variable != y)
                fail("partial relational support escaped the vocabulary");
    }

    bool rejectedRaw = false;
    try
    {
        (void)state.serializeRaw();
    }
    catch (const std::invalid_argument&)
    {
        rejectedRaw = true;
    }
    if (!rejectedRaw)
        fail("partial relational raw serialization was not rejected");
}

std::vector<AD::Variable> exportedConstraintClosure(
    const AD::NumericalDomain& domain,
    const std::vector<AD::Variable>& seeds)
{
    std::map<AD::Variable, std::set<AD::Variable>> adjacency;
    for (const AD::LinearConstraint& constraint : domain.toConstraints())
    {
        std::vector<AD::Variable> variables;
        for (const auto& [variable, coefficient] :
                constraint.expression().terms())
        {
            (void)coefficient;
            variables.push_back(variable);
        }
        for (AD::Variable lhs : variables)
            for (AD::Variable rhs : variables)
                if (lhs != rhs)
                    adjacency[lhs].insert(rhs);
    }
    std::set<AD::Variable> closure(seeds.begin(), seeds.end());
    std::vector<AD::Variable> worklist(seeds.begin(), seeds.end());
    while (!worklist.empty())
    {
        const AD::Variable variable = worklist.back();
        worklist.pop_back();
        for (AD::Variable neighbor : adjacency[variable])
            if (closure.insert(neighbor).second)
                worklist.push_back(neighbor);
    }
    return std::vector<AD::Variable>(closure.begin(), closure.end());
}

void octagonIndexedClosure()
{
    const AD::Variable x(61);
    const AD::Variable y(62);
    const AD::Variable independent(63);
    const AD::Variable absent(64);
    for (AD::OctagonStorageKind storage :
            {AD::OctagonStorageKind::DenseHalf,
             AD::OctagonStorageKind::SparseFinite,
             AD::OctagonStorageKind::ComponentDense})
    {
        AD::OctagonConfig config;
        config.storage = storage;
        AD::OctagonDomain state = AD::OctagonDomain::top(config);
        state.assign(y, AD::LinearExpression(x) +
                        AD::LinearExpression(AD::Rational(1)));
        state.assign(independent, AD::LinearExpression(AD::Rational(0)));
        state.forget(independent);

        for (const std::vector<AD::Variable>& seeds :
                {std::vector<AD::Variable>{x},
                 std::vector<AD::Variable>{independent},
                 std::vector<AD::Variable>{absent},
                 std::vector<AD::Variable>{x, independent}})
            if (state.relationalClosure(seeds) !=
                    exportedConstraintClosure(state, seeds))
                fail("indexed octagon closure disagrees with constraint oracle");

        AD::NumericalDomain::beginTelemetry();
        (void)state.relationalClosure({x});
        const AD::NumericalTelemetry telemetry =
            AD::NumericalDomain::endTelemetry();
        if (telemetry.relationalClosureCalls != 1 ||
                telemetry.relationalClosureConstraintExports != 0 ||
                telemetry.relationalClosureIndexedQueries != 1)
            fail("octagon closure did not use indexed telemetry path");

        state.forget(x);
        if (state.relationalClosure({y}) !=
                exportedConstraintClosure(state, {y}))
            fail("indexed octagon closure disagrees after forget");
    }
}

void octagonComponentCopyPolicies()
{
    const AD::Variable x(65);
    const AD::Variable y(66);
    std::vector<AD::LinearConstraintSet> results;
    for (const bool copyOnWrite : {true, false})
    {
        AD::OctagonConfig config;
        config.storage = AD::OctagonStorageKind::ComponentDense;
        config.componentCopyOnWrite = copyOnWrite;
        AD::OctagonDomain original = AD::OctagonDomain::top(config);
        original.assign(y, AD::LinearExpression(x) +
                           AD::LinearExpression(AD::Rational(1)));
        AD::OctagonDomain copy(original);
        copy.assign(y, AD::LinearExpression(x) +
                       AD::LinearExpression(AD::Rational(2)));
        if (original.entails(AD::equal(
                AD::LinearExpression(y),
                AD::LinearExpression(x) +
                    AD::LinearExpression(AD::Rational(1)))) !=
                AD::CheckResult::True)
            fail("component copy mutation changed its source state");
        if (copy.entails(AD::equal(
                AD::LinearExpression(y),
                AD::LinearExpression(x) +
                    AD::LinearExpression(AD::Rational(2)))) !=
                AD::CheckResult::True)
            fail("component copy policy changed assignment semantics");
        results.push_back(copy.toConstraints());
    }
    AD::OctagonConfig config;
    config.storage = AD::OctagonStorageKind::ComponentDense;
    AD::OctagonDomain cow = AD::OctagonDomain::fromConstraints(
        results.front(), config);
    AD::OctagonDomain eager = AD::OctagonDomain::fromConstraints(
        results.back(), config);
    if (cow.isSubsetOf(eager) != AD::CheckResult::True ||
            eager.isSubsetOf(cow) != AD::CheckResult::True)
        fail("component COW and eager-copy results disagree");
}

void octagonClosurePolicies()
{
    const AD::Variable x(67);
    const AD::Variable y(68);
    const AD::Variable z(69);
    std::vector<AD::LinearConstraintSet> results;
    for (const bool incremental : {true, false})
    {
        AD::OctagonConfig config;
        config.storage = AD::OctagonStorageKind::ComponentDense;
        config.incrementalClosure = incremental;
        AD::OctagonDomain state = AD::OctagonDomain::top(config);
        state.assignInterval(
            x, AD::Interval::closed(AD::Rational(-4), AD::Rational(4)));
        state.assign(y, AD::LinearExpression(x) +
                        AD::LinearExpression(AD::Rational(1)));
        state.assign(z, AD::LinearExpression(y) -
                        AD::LinearExpression(AD::Rational(2)));
        state.assume(AD::lessEqual(
            AD::LinearExpression(z), AD::LinearExpression(AD::Rational(2))));
        state.assumeAll({AD::greaterEqual(
            AD::LinearExpression(y), AD::LinearExpression(AD::Rational(-1)))});
        results.push_back(state.toConstraints());
    }
    AD::OctagonConfig config;
    config.storage = AD::OctagonStorageKind::ComponentDense;
    AD::OctagonDomain incremental = AD::OctagonDomain::fromConstraints(
        results.front(), config);
    AD::OctagonDomain full = AD::OctagonDomain::fromConstraints(
        results.back(), config);
    if (incremental.isSubsetOf(full) != AD::CheckResult::True ||
            full.isSubsetOf(incremental) != AD::CheckResult::True)
        fail("incremental and full Octagon closure results disagree");
}

void partialRelationalClosureDelegation()
{
    const AD::Variable x(71);
    const AD::Variable y(72);
    auto vocabulary = std::make_shared<const std::vector<AD::Variable>>(
        std::vector<AD::Variable>{x, y});

    for (AD::DomainKind kind :
            {AD::DomainKind::Octagon, AD::DomainKind::ConvexPolyhedra})
    {
        AD::PartialRelationalDomain state =
            AD::PartialRelationalDomain::top(kind, vocabulary);
        state.assign(y, AD::LinearExpression(x) +
                            AD::LinearExpression(AD::Rational(1)));
        AD::NumericalDomain::beginTelemetry();
        const std::vector<AD::Variable> closure =
            state.relationalClosure({x});
        const AD::NumericalTelemetry telemetry =
            AD::NumericalDomain::endTelemetry();
        if (closure != std::vector<AD::Variable>({x, y}))
            fail("partial relational closure omitted its facet component");
        if (telemetry.relationalClosureCalls != 1)
            fail("partial relational closure counted a delegated query twice");
        if (kind == AD::DomainKind::Octagon &&
                (telemetry.relationalClosureConstraintExports != 0 ||
                 telemetry.relationalClosureIndexedQueries != 1))
            fail("partial Octagon closure bypassed its component index");
        if (kind == AD::DomainKind::ConvexPolyhedra &&
                (telemetry.relationalClosureConstraintExports != 1 ||
                 telemetry.relationalClosureIndexedQueries != 0))
            fail("partial Polyhedra closure bypassed its export oracle");
    }
}
}
int main()
{
    octagonComponentCopyPolicies();
    octagonClosurePolicies();
    const AD::Variable x(1);
    const AD::Variable y(2);
    const AD::Variable z(3);
    const AD::Interval unit =
        AD::Interval::closed(AD::Rational(-1), AD::Rational(1));
    AD::OctagonConfig octagonConfig;
    octagonConfig.storage = AD::OctagonStorageKind::ComponentDense;
    AD::BoxAddressDomain left(
        std::make_unique<AD::OctagonDomain>(
            AD::OctagonDomain::top(octagonConfig)),
        AD::MemoryLayout(), true);
    AD::BoxAddressDomain right(left);
    left.setInterval(x, unit);
    left.setInterval(y, unit);
    left.assume(AD::equal(AD::LinearExpression(x), AD::LinearExpression(y)));
    right.setInterval(x, unit);
    right.setInterval(y, unit);
    right.assume(AD::equal(AD::LinearExpression(x),
                           -AD::LinearExpression(y)));
    AD::BoxAddressDomain boundedWithoutRelation(
        std::make_unique<AD::OctagonDomain>(
            AD::OctagonDomain::top(octagonConfig)),
        AD::MemoryLayout(), true);
    boundedWithoutRelation.setInterval(x, unit);
    boundedWithoutRelation.setInterval(y, unit);
    if (boundedWithoutRelation.isSubsetOf(left) == AD::CheckResult::True)
        fail("relational inclusion fell back to equal unary bounds");

    AD::BoxAddressDomain inactivePayloadLeft(
        std::make_unique<AD::OctagonDomain>(
            AD::OctagonDomain::top(octagonConfig)),
        AD::MemoryLayout(), true);
    AD::BoxAddressDomain inactivePayloadRight(inactivePayloadLeft);
    inactivePayloadRight.numerical().assignBound(
        x, AD::Interval::singleton(AD::Rational(0)));
    if (inactivePayloadLeft.isSubsetOf(inactivePayloadRight) !=
            AD::CheckResult::True)
        fail("inactive relational payload affected inclusion");

    AD::BoxAddressDomain activePayloadLeft(
        std::make_unique<AD::OctagonDomain>(
            AD::OctagonDomain::top(octagonConfig)),
        AD::MemoryLayout(), true);
    AD::BoxAddressDomain activePayloadRight(activePayloadLeft);
    activePayloadLeft.setInterval(x, AD::Interval::top());
    activePayloadRight.setInterval(
        x, AD::Interval::singleton(AD::Rational(0)));
    if (activePayloadLeft.isSubsetOf(activePayloadRight) ==
            AD::CheckResult::True)
        fail("active relational payload was ignored by inclusion");

    left.joinWith(right);
    if (left.numerical().entails(
            AD::equal(AD::LinearExpression(x), AD::LinearExpression(y))) ==
            AD::CheckResult::True)
        fail("relational join retained an incompatible left equality");
    AD::BoxAddressDomain initialized(
        AD::BoxDomain::top(), AD::MemoryLayout(), true);
    AD::BoxAddressDomain uninitialized(
        AD::BoxDomain::top(), AD::MemoryLayout(), true);
    initialized.setInterval(x, AD::Interval::singleton(AD::Rational(0)));
    initialized.joinWith(uninitialized);
    if (!initialized.numericalMayBeUninitialized(x) ||
            !initialized.interval(x).isSingleton() ||
            initialized.interval(x).singletonValue() != AD::Rational(0))
        fail("Box conditional initialized payload changed");
    AD::ConvexPolyhedraDomain poly = AD::ConvexPolyhedraDomain::top();
    poly.assign(z, AD::LinearExpression(x) + AD::LinearExpression(y));
    AD::NumericalDomain::beginTelemetry();
    const std::vector<AD::Variable> closure = poly.relationalClosure({z});
    const AD::NumericalTelemetry polyTelemetry =
        AD::NumericalDomain::endTelemetry();
    if (closure.size() != 3 || closure[0] != x || closure[1] != y ||
            closure[2] != z)
        fail("polyhedra relation closure omitted a connected variable");
    if (polyTelemetry.relationalClosureCalls != 1 ||
            polyTelemetry.relationalClosureConstraintExports != 1 ||
            polyTelemetry.relationalClosureIndexedQueries != 0)
        fail("polyhedra closure did not retain the generic export path");
    AD::ConvexPolyhedraDomain integerGuard =
        AD::ConvexPolyhedraDomain::top();
    integerGuard.assign(y, AD::LinearExpression(x) +
                           AD::LinearExpression(AD::Rational(1)));
    (void)integerGuard.toGenerators({x, y});
    integerGuard.assume(AD::lessEqual(AD::LinearExpression(y),
                                      AD::LinearExpression(x)));
    if (!integerGuard.isBottom())
        fail("integer polyhedra guard missed an inconsistent equality");
    exactSmallModel<AD::OctagonDomain>();
    exactSmallModel<AD::ConvexPolyhedraDomain>();
    partialRelationalModel(AD::DomainKind::Octagon);
    partialRelationalModel(AD::DomainKind::ConvexPolyhedra);
    octagonIndexedClosure();
    partialRelationalClosureDelegation();
    machineInteger8Exhaustive();
    std::cout << "RelationalProductTest: PASS\n";
    return EXIT_SUCCESS;
}
