//===- NumericalDomainFactory.h -- Select numerical backends -*- C++ -*-===//

#ifndef SVF_AE_NUMERICAL_DOMAIN_FACTORY_H
#define SVF_AE_NUMERICAL_DOMAIN_FACTORY_H

#include "AE/Core/AbstractDomain.h"
#include "AE/Core/NumericalDomain.h"
#include "AE/Core/OctagonDomain.h"

#include <memory>

namespace SVF::AbstractDomain
{

enum class NumericalBackendKind
{
    Native,
    Elina,
    ELINA = Elina
};

const char* numericalBackendName(NumericalBackendKind backend);
bool numericalBackendAvailable(DomainKind kind,
                               NumericalBackendKind backend) noexcept;
bool octagonBackendAvailable(NumericalBackendKind backend) noexcept;

std::unique_ptr<NumericalDomain> makeNumericalDomain(
    DomainKind kind, bool bottom,
    NumericalBackendKind backend = NumericalBackendKind::Native);
std::unique_ptr<NumericalDomain> makeOctagonDomain(
    NumericalBackendKind backend = NumericalBackendKind::Native,
    bool bottom = false, const OctagonConfig& config = {});

} // namespace SVF::AbstractDomain

#endif // SVF_AE_NUMERICAL_DOMAIN_FACTORY_H
