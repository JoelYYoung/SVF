//===- ELINABackendContract.h -- Checked ELINA adapter policy -*- C++ -*-===//

#ifndef SVF_AE_ELINA_BACKEND_CONTRACT_H
#define SVF_AE_ELINA_BACKEND_CONTRACT_H

#include "AE/Core/AbstractDomain.h"
#include "AE/Core/Expression.h"

#include <optional>
#include <string>

namespace SVF::AbstractDomain
{

enum class ELINAOperation
{
    TopologicalClosure,
    Canonicalization,
    Minimize
};

/// Fixed-version capability allowlist for ELINA f524156d Polyhedra.  It is
/// deliberately explicit: an unregistered generic operation is a null function
/// pointer in that revision and cannot be discovered safely by trial calls.
bool elinaPolyhedraSupports(ELINAOperation operation) noexcept;

/// ELINA predicate calls return a bool plus manager metadata.  A false result
/// is a definite counterexample only when the operation was exact; otherwise
/// it denotes Unknown.  Any exception also denotes Unknown.
CheckResult classifyELINAPredicate(bool value, bool exact, bool best,
                                   bool exception) noexcept;

/// Rewrite a strict constraint over mathematical integers to an equivalent
/// non-strict constraint accepted by the loose Polyhedra manager.  Rational
/// coefficients are scaled to integers first.  Returns nullopt when a real
/// variable is present or the relation cannot be represented by one closed
/// linear constraint; callers must then use a sound fallback.
std::optional<LinearConstraint> rewriteStrictIntegerConstraint(
    const LinearConstraint& constraint);

/// Stable diagnostic for an operation that is conservatively delegated to the
/// native B0 implementation.
std::string elinaUnsupportedFallbackReason(const std::string& operation);

} // namespace SVF::AbstractDomain

#endif // SVF_AE_ELINA_BACKEND_CONTRACT_H
