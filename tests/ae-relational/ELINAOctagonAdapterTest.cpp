#include "AE/Core/ElinaOctagonDomain.h"
#include "AE/Core/NumericalDomainFactory.h"

#include <cfenv>
#include <cstdlib>
#include <iostream>

namespace AD = SVF::AbstractDomain;

namespace
{

[[noreturn]] void fail(const char* message)
{
    std::cerr << "FAIL: " << message << '\n';
    std::exit(EXIT_FAILURE);
}

#ifdef SVF_HAVE_ELINA
void expectSingleton(const AD::Interval& interval, int expected,
                     const char* message)
{
    if (!interval.isSingleton() ||
            interval.singletonValue() != AD::Rational(expected))
        fail(message);
}
#endif

} // namespace

int main()
{
    if (!AD::octagonBackendAvailable(AD::NumericalBackendKind::Native))
        fail("native Octagon factory capability changed");
#ifndef SVF_HAVE_ELINA
    if (AD::octagonBackendAvailable(AD::NumericalBackendKind::Elina))
        fail("disabled ELINA Octagon was reported available");
    std::unique_ptr<AD::NumericalDomain> selected = AD::makeOctagonDomain(
        AD::NumericalBackendKind::Native);
    if (!selected->isDomain<AD::OctagonDomain>())
        fail("default factory did not preserve the native B0 backend");
    std::cout << "ELINAOctagonAdapterTest: PASS (native-only build)\n";
    return EXIT_SUCCESS;
#else
    if (!AD::octagonBackendAvailable(AD::NumericalBackendKind::Elina))
        fail("ELINA Octagon factory is unavailable");

    const auto capabilities = AD::ElinaOctagonDomain::capabilities();
    if (!capabilities.parallelAssignment || !capabilities.substitution ||
            !capabilities.projection || !capabilities.widening ||
            capabilities.canonicalization || capabilities.narrowing ||
            capabilities.thresholdWidening || capabilities.minimization)
        fail("ELINA Octagon capability table is inconsistent");

    const int initialRounding = std::fegetround();
    const AD::Variable x(1);
    const AD::Variable y(2);
    AD::ElinaOctagonDomain point = AD::ElinaOctagonDomain::fromConstraints({
        AD::equal(AD::LinearExpression(x),
                  AD::LinearExpression(AD::Rational(5))),
        AD::equal(AD::LinearExpression(y),
                  AD::LinearExpression(x) +
                      AD::LinearExpression(AD::Rational(2)))
    });
    if (std::fegetround() != initialRounding)
        fail("ELINA adapter leaked its FPU rounding mode");
    expectSingleton(point.bound(x), 5, "ELINA point x bound");
    expectSingleton(point.bound(y), 7, "ELINA point y bound");
    if (point.entails(AD::equal(
            AD::LinearExpression(y),
            AD::LinearExpression(x) +
                AD::LinearExpression(AD::Rational(2)))) !=
            AD::CheckResult::True)
        fail("ELINA relation entailment");

    AD::ElinaOctagonDomain top = AD::ElinaOctagonDomain::top();
    if (point.subsetOf(top) != AD::CheckResult::True)
        fail("ELINA true inclusion was not proved");
    if (top.subsetOf(point) != AD::CheckResult::False)
        fail("ELINA exact false inclusion lost its tri-state result");

    AD::WideningPolicy threshold({AD::Rational(0)});
    AD::ElinaOctagonDomain widened = point.widen(top, threshold);
    if (widened.lastOperation().approximation !=
            AD::ApproximationKind::UnsupportedFallback)
        fail("threshold widening capability fallback was not explicit");

    std::unique_ptr<AD::NumericalDomain> selected = AD::makeOctagonDomain(
        AD::NumericalBackendKind::Elina);
    if (!selected->isDomain<AD::ElinaOctagonDomain>())
        fail("factory did not select the ELINA Octagon class");
    if (std::fegetround() != initialRounding)
        fail("ELINA factory leaked its FPU rounding mode");

    std::cout << "ELINAOctagonAdapterTest: PASS\n";
    return EXIT_SUCCESS;
#endif
}
