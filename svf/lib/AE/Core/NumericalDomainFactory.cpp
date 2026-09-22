//===- NumericalDomainFactory.cpp -- Select numerical backends ---------===//

#include "AE/Core/NumericalDomainFactory.h"

#include "AE/Core/ConvexPolyhedraDomain.h"
#ifdef SVF_HAVE_ELINA
#include "AE/Core/ElinaOctagonDomain.h"
#endif
#ifdef SVF_HAS_ELINA_POLYHEDRA
#include "AE/Core/ELINAPolyhedraDomain.h"
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

bool numericalBackendAvailable(DomainKind kind,
                               NumericalBackendKind backend) noexcept
{
    if (backend == NumericalBackendKind::Native)
        return kind == DomainKind::Box || kind == DomainKind::Octagon ||
               kind == DomainKind::ConvexPolyhedra;
    if (kind == DomainKind::Octagon)
    {
#ifdef SVF_HAVE_ELINA
        return ElinaOctagonDomain::runtimeFpuSupported();
#else
        return false;
#endif
    }
    if (kind == DomainKind::ConvexPolyhedra)
    {
#ifdef SVF_HAS_ELINA_POLYHEDRA
        return true;
#else
        return false;
#endif
    }
    return false;
}

bool octagonBackendAvailable(NumericalBackendKind backend) noexcept
{
    return numericalBackendAvailable(DomainKind::Octagon, backend);
}

std::unique_ptr<NumericalDomain> makeOctagonDomain(
    NumericalBackendKind backend, bool bottom, const OctagonConfig& config)
{
    if (backend == NumericalBackendKind::Native)
        return std::make_unique<OctagonDomain>(
            bottom ? OctagonDomain::bottom(config) : OctagonDomain::top(config));
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

std::unique_ptr<NumericalDomain> makeNumericalDomain(
    DomainKind kind, bool bottom, NumericalBackendKind backend)
{
    if (kind == DomainKind::Octagon)
        return makeOctagonDomain(backend, bottom);
    if (backend == NumericalBackendKind::Elina)
    {
#ifdef SVF_HAS_ELINA_POLYHEDRA
        if (kind == DomainKind::ConvexPolyhedra)
            return std::make_unique<ELINAPolyhedraDomain>(
                bottom ? ELINAPolyhedraDomain::bottom()
                       : ELINAPolyhedraDomain::top());
#endif
        throw std::invalid_argument(
            "requested ELINA backend was not configured for this domain");
    }

    switch (kind)
    {
    case DomainKind::Box:
        return std::make_unique<BoxDomain>(bottom ? BoxDomain::bottom()
                                                  : BoxDomain::top());
    case DomainKind::ConvexPolyhedra:
        return std::make_unique<ConvexPolyhedraDomain>(
            bottom ? ConvexPolyhedraDomain::bottom()
                   : ConvexPolyhedraDomain::top());
    default:
        throw std::invalid_argument("domain kind is not numerical");
    }
}

} // namespace SVF::AbstractDomain
