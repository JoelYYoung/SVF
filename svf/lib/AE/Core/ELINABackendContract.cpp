//===- ELINABackendContract.cpp -- Checked ELINA policy ----------===//

#include "AE/Core/ELINABackendContract.h"

#include <gmpxx.h>

namespace SVF::AbstractDomain
{

bool elinaPolyhedraSupports(ELINAOperation operation) noexcept
{
    switch (operation)
    {
    case ELINAOperation::TopologicalClosure:
        return false;
    case ELINAOperation::Canonicalization:
    case ELINAOperation::Minimize:
        return true;
    }
    return false;
}

CheckResult classifyELINAPredicate(bool value, bool exact, bool best,
                                   bool exception) noexcept
{
    (void)best;
    if (exception)
        return CheckResult::Unknown;
    if (value)
        return CheckResult::True;
    return exact ? CheckResult::False : CheckResult::Unknown;
}

namespace
{

bool isStrict(ConstraintKind kind)
{
    return kind == ConstraintKind::LessThan ||
           kind == ConstraintKind::GreaterThan;
}

} // namespace

std::optional<LinearConstraint> rewriteStrictIntegerConstraint(
    const LinearConstraint& constraint)
{
    if (!isStrict(constraint.kind()))
        return constraint;

    for (const auto& [variable, coefficient] : constraint.expression().terms())
    {
        (void)coefficient;
        if (variable.type().kind != NumericKind::Integer)
            return std::nullopt;
    }

    mpz_class scale = constraint.expression().constant().value().get_den();
    for (const auto& [variable, coefficient] : constraint.expression().terms())
    {
        (void)variable;
        mpz_lcm(scale.get_mpz_t(), scale.get_mpz_t(),
                coefficient.value().get_den_mpz_t());
    }

    LinearExpression scaled(constraint.expression().constant() *
                            Rational(Integer(scale.get_str())));
    for (const auto& [variable, coefficient] : constraint.expression().terms())
    {
        scaled.setCoefficient(variable,
                              coefficient * Rational(Integer(scale.get_str())));
    }

    if (constraint.kind() == ConstraintKind::LessThan)
    {
        // scale * e < 0  iff  scale * e + 1 <= 0 over integers.
        scaled += LinearExpression(Rational(1));
        return LinearConstraint(std::move(scaled), ConstraintKind::LessEqual);
    }

    // scale * e > 0  iff  scale * e - 1 >= 0 over integers.
    scaled -= LinearExpression(Rational(1));
    return LinearConstraint(std::move(scaled), ConstraintKind::GreaterEqual);
}

std::string elinaUnsupportedFallbackReason(const std::string& operation)
{
    return "ELINA Polyhedra operation '" + operation +
           "' is unsupported by fixed revision f524156d; used native B0 fallback";
}

} // namespace SVF::AbstractDomain
