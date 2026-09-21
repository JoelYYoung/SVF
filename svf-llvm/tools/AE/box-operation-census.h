//===- box-operation-census.h -- Box operation reuse census ----*- C++ -*-===//
//
//                     SVF: Static Value-Flow Analysis
//
// Copyright (C) <2013->  <Yulei Sui>
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

#ifndef SVF_BOX_OPERATION_CENSUS_H
#define SVF_BOX_OPERATION_CENSUS_H

#include "AE/Core/AbstractDomain.h"

namespace SVF::BoxOperationCensus
{

void collect(const AbstractDomain::AbstractOperationEvent& event);
void print();
void selfTest();

} // namespace SVF::BoxOperationCensus

#endif // SVF_BOX_OPERATION_CENSUS_H
