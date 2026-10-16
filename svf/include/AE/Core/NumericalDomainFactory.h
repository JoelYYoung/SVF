//===- NumericalDomainFactory.h -- Select native domains -*- C++ -*-===//
#ifndef SVF_AE_NUMERICAL_DOMAIN_FACTORY_H
#define SVF_AE_NUMERICAL_DOMAIN_FACTORY_H

#include "AE/Core/OctagonDomain.h"
#include <memory>

namespace SVF::AbstractDomain
{
std::unique_ptr<NumericalDomain> makeNumericalDomain(
    DomainKind kind, bool bottom, const OctagonConfig& config = {});
std::unique_ptr<NumericalDomain> makeOctagonDomain(
    bool bottom = false, const OctagonConfig& config = {});
}
#endif
