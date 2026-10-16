#pragma once
#include "AE/Core/NumericalDomain.h"
#include <map>
namespace SVF
{
class SVFIR;class SVFIRAdapter;class PointerAnalysis;class AllocationSizeModel;
// Feature-only interval equations over all definitions, weak memory writes,
// actual/formal/return bindings. Never consumes or changes downstream AE state.
std::map<AbstractDomain::Variable,AbstractDomain::Interval> flowInsensitivePackingRanges(
    SVFIR&,const SVFIRAdapter&,PointerAnalysis&,
    const std::vector<AbstractDomain::Variable>&,const AllocationSizeModel&);
}
