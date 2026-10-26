//===- NumericalDomainFactory.h -- Select native domains -*- C++ -*-===//
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

#ifndef SVF_AE_NUMERICAL_DOMAIN_FACTORY_H
#define SVF_AE_NUMERICAL_DOMAIN_FACTORY_H

#include "AE/Core/OctagonDomain.h"
#include <memory>

namespace SVF
{
std::unique_ptr<NumericalDomain> makeNumericalDomain(
    DomainKind kind, bool bottom, const OctagonConfig& config = {});
std::unique_ptr<NumericalDomain> makeOctagonDomain(
    bool bottom = false, const OctagonConfig& config = {});
}
#endif
