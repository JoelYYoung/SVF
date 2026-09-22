#include "AE/Core/ELINABackendContract.h"
#include "AE/Core/NumericalDomainFactory.h"
#include "AE/Core/PartialRelationalDomain.h"
#include "AE/Core/ConvexPolyhedraDomain.h"
#include "AE/Core/OctagonDomain.h"
#if defined(SVF_HAVE_ELINA)
#    include "AE/Core/ElinaOctagonDomain.h"
#endif
#if defined(SVF_HAS_ELINA_POLYHEDRA)
#    include "AE/Core/ELINAPolyhedraDomain.h"
#endif

#include <cfenv>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace AD = SVF::AbstractDomain;

namespace
{

[[noreturn]] void fail(const char* message)
{
    std::cerr << "FAIL: " << message << '\n';
    std::exit(EXIT_FAILURE);
}

void expect(bool condition, const char* message)
{
    if (!condition)
        fail(message);
}

} // namespace

int main()
{
    expect(!AD::elinaPolyhedraSupports(AD::ELINAOperation::TopologicalClosure),
           "unregistered Polyhedra closure was advertised");
    expect(AD::elinaPolyhedraSupports(AD::ELINAOperation::Canonicalization),
           "Polyhedra canonicalization was hidden");

    expect(AD::classifyELINAPredicate(true, false, false, false) ==
               AD::CheckResult::True,
           "true ELINA predicate was not preserved");
    expect(AD::classifyELINAPredicate(false, false, false, false) ==
               AD::CheckResult::Unknown,
           "inexact false ELINA predicate was treated as definite");
    expect(AD::classifyELINAPredicate(false, true, true, false) ==
               AD::CheckResult::False,
           "exact false ELINA predicate was not preserved");
    expect(AD::classifyELINAPredicate(true, true, true, true) ==
               AD::CheckResult::Unknown,
           "ELINA exception did not dominate predicate value");

    const AD::Variable integer(1, AD::NumericType::integer());
    AD::LinearExpression halfInteger;
    halfInteger.setCoefficient(integer,
                               AD::Rational(AD::Integer(1), AD::Integer(2)));
    const auto rewritten = AD::rewriteStrictIntegerConstraint(
        AD::LinearConstraint(halfInteger, AD::ConstraintKind::GreaterThan));
    expect(rewritten.has_value(),
           "integer strict constraint was not rewritten");
    expect(rewritten->kind() == AD::ConstraintKind::GreaterEqual,
           "integer strict constraint kept a strict relation");
    expect(rewritten->expression().coefficient(integer) == AD::Rational(1),
           "integer strict rewrite did not clear denominators");
    expect(rewritten->expression().constant() == AD::Rational(-1),
           "integer x/2 > 0 did not become x - 1 >= 0");

    const AD::Variable real(2, AD::NumericType::real());
    expect(!AD::rewriteStrictIntegerConstraint(AD::LinearConstraint(
               AD::LinearExpression(real), AD::ConstraintKind::LessThan)),
           "real strict constraint was rewritten as an integer guard");

    auto native =
        AD::makeNumericalDomain(AD::DomainKind::ConvexPolyhedra, false,
                                AD::NumericalBackendKind::Native);
    expect(native->isTop(), "native factory did not preserve B0 top");

    AD::OctagonConfig denseConfig;
    denseConfig.storage = AD::OctagonStorageKind::ComponentDense;
    auto nativeOctagon = AD::makeNumericalDomain(
        AD::DomainKind::Octagon, false, AD::NumericalBackendKind::Native,
        denseConfig);
    expect(nativeOctagon->isDomain<AD::OctagonDomain>() &&
               static_cast<const AD::OctagonDomain&>(*nativeOctagon)
                       .config().storage ==
                   AD::OctagonStorageKind::ComponentDense,
           "factory lost AE's dense Octagon storage configuration");

    auto vocabulary = std::make_shared<
        const AD::PartialRelationalDomain::Vocabulary>(
            AD::PartialRelationalDomain::Vocabulary{integer});
    AD::PartialRelationalDomain nativePartialOctagon =
        AD::PartialRelationalDomain::top(
            AD::DomainKind::Octagon, vocabulary,
            AD::NumericalBackendKind::Native);
    expect(nativePartialOctagon.backend() ==
               AD::NumericalBackendKind::Native &&
               nativePartialOctagon.relational().isDomain<AD::OctagonDomain>(),
           "partial Octagon did not use the selected native backend");
    AD::PartialRelationalDomain nativePartialPolyhedra =
        AD::PartialRelationalDomain::top(
            AD::DomainKind::ConvexPolyhedra, vocabulary,
            AD::NumericalBackendKind::Native);
    expect(nativePartialPolyhedra.relational()
               .isDomain<AD::ConvexPolyhedraDomain>(),
           "partial Polyhedra did not use the selected native backend");

    try
    {
        (void)AD::makeNumericalDomain(AD::DomainKind::Box, false,
                                      AD::NumericalBackendKind::ELINA);
        fail("Box silently accepted the ELINA backend");
    }
    catch (const std::invalid_argument&)
    {
    }

#if defined(SVF_HAS_ELINA_POLYHEDRA)
    expect(AD::numericalBackendAvailable(AD::DomainKind::ConvexPolyhedra,
                                         AD::NumericalBackendKind::ELINA),
           "configured ELINA Polyhedra backend was not advertised");
    auto elina = AD::makeNumericalDomain(AD::DomainKind::ConvexPolyhedra, false,
                                         AD::NumericalBackendKind::ELINA);
    elina->assume(AD::equal(AD::LinearExpression(integer),
                            AD::LinearExpression(AD::Rational(5))));
    const AD::Interval bound = elina->bound(integer);
    expect(bound.isSingleton() && bound.singletonValue() == AD::Rational(5),
           "ELINA Polyhedra runtime backend did not preserve x == 5");

    const AD::Variable y(3, AD::NumericType::integer());
    AD::ELINAPolyhedraDomain sequence = AD::ELINAPolyhedraDomain::top();
    const int originalRounding = std::fegetround();
    sequence.assume(AD::greaterThan(AD::LinearExpression(integer),
                                    AD::LinearExpression(AD::Rational(0))));
    expect(sequence.bound(integer).lower().isFinite() &&
               sequence.bound(integer).lower().value() == AD::Rational(1),
           "integer strict guard was not rewritten to x >= 1");
    sequence.assign(y, AD::LinearExpression(integer) +
                           AD::LinearExpression(AD::Rational(2)));
    sequence.assume(AD::lessEqual(AD::LinearExpression(integer),
                                  AD::LinearExpression(AD::Rational(4))));
    expect(sequence.lastOperation().approximation !=
               AD::ApproximationKind::UnsupportedFallback,
           "integer common-operation sequence used the native fallback");
    expect(sequence.bound(y) ==
               AD::Interval::closed(AD::Rational(3), AD::Rational(6)),
           "ELINA assign/assume sequence lost relational bounds");

    sequence.assignParallel(
        {{integer,
          AD::LinearExpression(y) + AD::LinearExpression(AD::Rational(1))},
         {y, AD::LinearExpression(integer) +
                 AD::LinearExpression(AD::Rational(1))}});
    expect(sequence.bound(integer) ==
                   AD::Interval::closed(AD::Rational(4), AD::Rational(7)) &&
               sequence.bound(y) ==
                   AD::Interval::closed(AD::Rational(2), AD::Rational(5)),
           "ELINA parallel assignment was not simultaneous");

    AD::ELINAPolyhedraDomain copied(sequence);
    expect(copied.backendInclusion(sequence) == AD::CheckResult::True &&
               sequence.backendInclusion(copied) == AD::CheckResult::True,
           "ELINA copy did not preserve inclusion");
    copied.forget(integer);
    expect(copied.bound(integer).isTop() && !copied.bound(y).isTop(),
           "ELINA forget did not project exactly one dimension");
    const AD::Interval yAfterForget = copied.bound(y);
    copied.assign(integer, AD::LinearExpression(AD::Rational(8)));
    expect(copied.bound(integer).isSingleton() &&
               copied.bound(integer).singletonValue() == AD::Rational(8) &&
               copied.bound(y) == yAfterForget,
           "ELINA dimension reinsertion changed an existing coordinate");
    AD::ELINAPolyhedraDomain projected(sequence);
    projected.project({y});
    expect(projected.bound(integer).isTop() &&
               projected.supportVariables().size() == 1 &&
               projected.supportVariables().front() == y,
           "ELINA project retained a removed dimension");

    AD::ELINAPolyhedraDomain substituted = AD::ELINAPolyhedraDomain::top();
    substituted.assume(AD::equal(AD::LinearExpression(integer),
                                 AD::LinearExpression(AD::Rational(5))));
    substituted.substitute(integer, AD::LinearExpression(y) +
                                        AD::LinearExpression(AD::Rational(1)));
    expect(substituted.bound(y).isSingleton() &&
               substituted.bound(y).singletonValue() == AD::Rational(4),
           "ELINA substitution did not compute the assignment preimage");

    AD::ELINAPolyhedraDomain left = AD::ELINAPolyhedraDomain::top();
    left.assume(AD::equal(AD::LinearExpression(integer),
                          AD::LinearExpression(AD::Rational(0))));
    AD::ELINAPolyhedraDomain right = AD::ELINAPolyhedraDomain::top();
    right.assume(AD::equal(AD::LinearExpression(integer),
                           AD::LinearExpression(AD::Rational(2))));
    AD::ELINAPolyhedraDomain joined(left);
    joined.joinWith(right);
    expect(joined.bound(integer) ==
               AD::Interval::closed(AD::Rational(0), AD::Rational(2)),
           "ELINA join returned the wrong hull");
    AD::ELINAPolyhedraDomain lowerHalf = AD::ELINAPolyhedraDomain::top();
    lowerHalf.assume(AD::greaterEqual(AD::LinearExpression(integer),
                                      AD::LinearExpression(AD::Rational(1))));
    joined.meetWith(lowerHalf);
    expect(joined.bound(integer) ==
                   AD::Interval::closed(AD::Rational(1), AD::Rational(2)) &&
               joined.entails(
                   AD::greaterEqual(AD::LinearExpression(integer),
                                    AD::LinearExpression(AD::Rational(1)))) ==
                   AD::CheckResult::True,
           "ELINA meet/entailment sequence failed");

    const AD::Variable realValue(2, AD::NumericType::real());
    const AD::Variable lateInteger(2, AD::NumericType::integer());
    AD::ELINAPolyhedraDomain mixed = AD::ELINAPolyhedraDomain::top();
    mixed.assume(AD::equal(
        AD::LinearExpression(realValue),
        AD::LinearExpression(AD::Rational(AD::Integer(1), AD::Integer(2)))));
    expect(mixed.lastOperation().approximation ==
                   AD::ApproximationKind::UnsupportedFallback &&
               !mixed.lastOperation().reason.empty(),
           "fixed ELINA real-dimension limitation was not explicit");
    mixed.assume(AD::equal(AD::LinearExpression(lateInteger),
                           AD::LinearExpression(AD::Rational(9))));
    expect(mixed.bound(realValue).isSingleton() &&
               mixed.bound(realValue).singletonValue() ==
                   AD::Rational(AD::Integer(1), AD::Integer(2)) &&
               mixed.bound(lateInteger).isSingleton() &&
               mixed.bound(lateInteger).singletonValue() == AD::Rational(9),
           "ELINA mixed integer/real dimension insertion changed values");

    AD::ELINAPolyhedraDomain wideningNext = AD::ELINAPolyhedraDomain::top();
    wideningNext.assume(AD::greaterEqual(
        AD::LinearExpression(integer), AD::LinearExpression(AD::Rational(0))));
    wideningNext.assume(AD::lessEqual(AD::LinearExpression(integer),
                                      AD::LinearExpression(AD::Rational(1))));
    AD::ELINAPolyhedraDomain widened(left);
    widened.widenWith(wideningNext);
    expect(wideningNext.backendInclusion(widened) == AD::CheckResult::True,
           "ELINA widening did not include its next state");
    widened.narrowWith(wideningNext);
    expect(widened.bound(integer) ==
                   AD::Interval::closed(AD::Rational(0), AD::Rational(1)) &&
               widened.lastOperation().operation ==
                   AD::OperationKind::Narrowing,
           "ELINA meet-based narrowing did not retain the next state");
    expect(std::fegetround() == originalRounding,
           "ELINA backend leaked its rounding mode");

    AD::PartialRelationalDomain elinaPartialPolyhedra =
        AD::PartialRelationalDomain::top(
            AD::DomainKind::ConvexPolyhedra, vocabulary,
            AD::NumericalBackendKind::ELINA);
    expect(elinaPartialPolyhedra.backend() ==
               AD::NumericalBackendKind::ELINA &&
               elinaPartialPolyhedra.relational()
                   .isDomain<AD::ELINAPolyhedraDomain>(),
           "partial Polyhedra did not use the selected ELINA backend");
#if defined(SVF_HAVE_ELINA)
    AD::PartialRelationalDomain elinaPartialOctagon =
        AD::PartialRelationalDomain::top(
            AD::DomainKind::Octagon, vocabulary,
            AD::NumericalBackendKind::ELINA);
    expect(elinaPartialOctagon.relational()
               .isDomain<AD::ElinaOctagonDomain>(),
           "partial Octagon did not use the selected ELINA backend");
#endif
#else
    expect(!AD::numericalBackendAvailable(AD::DomainKind::ConvexPolyhedra,
                                          AD::NumericalBackendKind::ELINA),
           "ELINA backend reported available in a native-only build");
    try
    {
        (void)AD::makeNumericalDomain(AD::DomainKind::ConvexPolyhedra, false,
                                      AD::NumericalBackendKind::ELINA);
        fail("unconfigured ELINA factory did not fail explicitly");
    }
    catch (const std::invalid_argument&)
    {
    }
    try
    {
        (void)AD::PartialRelationalDomain::top(
            AD::DomainKind::ConvexPolyhedra, vocabulary,
            AD::NumericalBackendKind::ELINA);
        fail("partial domain silently replaced unavailable ELINA");
    }
    catch (const std::invalid_argument&)
    {
    }
#endif

    std::cout << "ELINABackendContractTest: PASS\n";
    return EXIT_SUCCESS;
}
