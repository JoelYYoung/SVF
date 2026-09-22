//===- NumericalDomainFactory.h -- Select numerical backend -*- C++ -*-===//

#ifndef SVF_AE_NUMERICAL_DOMAIN_FACTORY_H
#define SVF_AE_NUMERICAL_DOMAIN_FACTORY_H

#include "AE/Core/AbstractDomain.h"
#include "AE/Core/NumericalDomain.h"

#include <memory>

namespace SVF::AbstractDomain
{

enum class NumericalBackendKind
{
    Native,
    ELINA
};

/// Construct a domain implementation without changing the mathematical domain
/// selected by `kind`. Native is the stable B0 default. ELINA is available only
/// when the corresponding optional build support was configured.
std::unique_ptr<NumericalDomain> makeNumericalDomain(
    DomainKind kind, bool bottom,
    NumericalBackendKind backend = NumericalBackendKind::Native);

bool numericalBackendAvailable(DomainKind kind,
                               NumericalBackendKind backend) noexcept;

} // namespace SVF::AbstractDomain

#endif // SVF_AE_NUMERICAL_DOMAIN_FACTORY_H
