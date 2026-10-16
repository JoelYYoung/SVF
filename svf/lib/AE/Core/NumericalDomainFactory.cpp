//===- NumericalDomainFactory.cpp -- Select native domains ---------===//
#include "AE/Core/NumericalDomainFactory.h"
#include "AE/Core/ConvexPolyhedraDomain.h"
#include <stdexcept>

namespace SVF::AbstractDomain
{
std::unique_ptr<NumericalDomain> makeOctagonDomain(
    bool bottom, const OctagonConfig& config)
{
    return std::make_unique<OctagonDomain>(
        bottom ? OctagonDomain::bottom(config) : OctagonDomain::top(config));
}

std::unique_ptr<NumericalDomain> makeNumericalDomain(
    DomainKind kind, bool bottom, const OctagonConfig& config)
{
    switch (kind)
    {
    case DomainKind::Box:
        return std::make_unique<BoxDomain>(
            bottom ? BoxDomain::bottom() : BoxDomain::top());
    case DomainKind::Octagon:
        return makeOctagonDomain(bottom, config);
    case DomainKind::ConvexPolyhedra:
        return std::make_unique<ConvexPolyhedraDomain>(
            bottom ? ConvexPolyhedraDomain::bottom() : ConvexPolyhedraDomain::top());
    default:
        throw std::invalid_argument("domain kind is not numerical");
    }
}
}
