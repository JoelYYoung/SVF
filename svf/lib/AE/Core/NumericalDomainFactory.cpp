//===- NumericalDomainFactory.cpp -- Select native domains ---------===//
//
//                     SVF: Static Value-Flow Analysis
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU Affero General Public License for more details.
//
// You should have received a copy of the GNU Affero General Public License
// along with this program. If not, see <http://www.gnu.org/licenses/>.
//
//===----------------------------------------------------------------------===//

#include "AE/Core/NumericalDomainFactory.h"
#include "AE/Core/ConvexPolyhedraDomain.h"
#include <stdexcept>

namespace SVF
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
