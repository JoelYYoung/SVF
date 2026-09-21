#include "AE/Core/BoxAddressDomain.h"
#include "AE/Core/ConvexPolyhedraDomain.h"
#include "AE/Core/Expression.h"
#include "AE/Core/OctagonDomain.h"
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
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
}
int main()
{
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
    const std::vector<AD::Variable> closure = poly.relationalClosure({z});
    if (closure.size() != 3 || closure[0] != x || closure[1] != y ||
            closure[2] != z)
        fail("polyhedra relation closure omitted a connected variable");
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
    machineInteger8Exhaustive();
    std::cout << "RelationalProductTest: PASS\n";
    return EXIT_SUCCESS;
}
