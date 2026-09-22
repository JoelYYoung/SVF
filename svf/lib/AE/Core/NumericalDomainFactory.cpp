//===- NumericalDomainFactory.cpp -- Select numerical backend ------===//

#include "AE/Core/NumericalDomainFactory.h"

#include "AE/Core/ConvexPolyhedraDomain.h"
#include "AE/Core/OctagonDomain.h"
#if defined(SVF_HAS_ELINA_POLYHEDRA)
#include "AE/Core/ELINAPolyhedraDomain.h"
#endif

#include <stdexcept>

namespace SVF::AbstractDomain
{

bool numericalBackendAvailable(DomainKind kind,
                               NumericalBackendKind backend) noexcept
{
    if (backend == NumericalBackendKind::Native)
        return kind == DomainKind::Box || kind == DomainKind::Octagon ||
               kind == DomainKind::ConvexPolyhedra;
#if defined(SVF_HAS_ELINA_POLYHEDRA)
    return kind == DomainKind::ConvexPolyhedra;
#else
    (void)kind;
    return false;
#endif
}

std::unique_ptr<NumericalDomain> makeNumericalDomain(
    DomainKind kind, bool bottom, NumericalBackendKind backend)
{
    if (backend == NumericalBackendKind::ELINA)
    {
#if defined(SVF_HAS_ELINA_POLYHEDRA)
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
    case DomainKind::Octagon:
        return std::make_unique<OctagonDomain>(bottom ? OctagonDomain::bottom()
                                                      : OctagonDomain::top());
    case DomainKind::ConvexPolyhedra:
        return std::make_unique<ConvexPolyhedraDomain>(
            bottom ? ConvexPolyhedraDomain::bottom()
                   : ConvexPolyhedraDomain::top());
    default:
        throw std::invalid_argument("domain kind is not numerical");
    }
}

} // namespace SVF::AbstractDomain
