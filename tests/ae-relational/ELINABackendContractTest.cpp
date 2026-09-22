#include "AE/Core/ELINABackendContract.h"
#include "AE/Core/NumericalDomainFactory.h"

#include <cstdlib>
#include <iostream>
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
    expect(!AD::elinaPolyhedraSupports(
               AD::ELINAOperation::TopologicalClosure),
           "unregistered Polyhedra closure was advertised");
    expect(AD::elinaPolyhedraSupports(
               AD::ELINAOperation::Canonicalization),
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
    halfInteger.setCoefficient(
        integer, AD::Rational(AD::Integer(1), AD::Integer(2)));
    const auto rewritten = AD::rewriteStrictIntegerConstraint(
        AD::LinearConstraint(halfInteger, AD::ConstraintKind::GreaterThan));
    expect(rewritten.has_value(), "integer strict constraint was not rewritten");
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

    auto native = AD::makeNumericalDomain(
        AD::DomainKind::ConvexPolyhedra, false,
        AD::NumericalBackendKind::Native);
    expect(native->isTop(), "native factory did not preserve B0 top");

#if !defined(SVF_HAS_ELINA_POLYHEDRA)
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
#endif

    std::cout << "ELINABackendContractTest: PASS\n";
    return EXIT_SUCCESS;
}
