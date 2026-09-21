#include "AE/Core/BoxAddressDomain.h"
#include "AE/Core/ConvexPolyhedraDomain.h"
#include "AE/Core/Expression.h"
#include "AE/Core/OctagonDomain.h"
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
    std::cout << "RelationalProductTest: PASS\n";
    return EXIT_SUCCESS;
}
