//===- NumericalDomainFactory.cpp -- Select numerical backends ---------===//

#include "AE/Core/NumericalDomainFactory.h"

#ifdef SVF_HAVE_ELINA
#include "AE/Core/ElinaOctagonDomain.h"
#endif

#include <stdexcept>

namespace SVF::AbstractDomain
{

const char* numericalBackendName(NumericalBackendKind backend)
{
    switch (backend)
    {
    case NumericalBackendKind::Native:
        return "native";
    case NumericalBackendKind::Elina:
        return "elina";
    }
    return "unknown";
}

bool octagonBackendAvailable(NumericalBackendKind backend) noexcept
{
    if (backend == NumericalBackendKind::Native)
        return true;
#ifdef SVF_HAVE_ELINA
    return backend == NumericalBackendKind::Elina &&
           ElinaOctagonDomain::runtimeFpuSupported();
#else
    (void)backend;
    return false;
#endif
}

std::unique_ptr<NumericalDomain> makeOctagonDomain(
    NumericalBackendKind backend, bool bottom, const OctagonConfig& config)
{
    switch (backend)
    {
    case NumericalBackendKind::Native:
        return std::make_unique<OctagonDomain>(
            bottom ? OctagonDomain::bottom(config)
                   : OctagonDomain::top(config));
    case NumericalBackendKind::Elina:
#ifdef SVF_HAVE_ELINA
        if (!ElinaOctagonDomain::runtimeFpuSupported())
            throw std::runtime_error(
                "ELINA Octagon requires verified FE_UPWARD support");
        return std::make_unique<ElinaOctagonDomain>(
            bottom ? ElinaOctagonDomain::bottom(config)
                   : ElinaOctagonDomain::top(config));
#else
        throw std::invalid_argument(
            "ELINA Octagon was not enabled in this SVF build");
#endif
    }
    throw std::invalid_argument("unknown numerical backend");
}

} // namespace SVF::AbstractDomain
