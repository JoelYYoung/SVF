//===- SAS16FeatureExtraction.h -- SAS16Feature Extraction --------===//
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

// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef SVF_AE_SAS16_FEATURE_EXTRACTION_H
#define SVF_AE_SAS16_FEATURE_EXTRACTION_H
#include "AE/Core/LearnedRelationPacking.h"
namespace SVF
{
class SVFIR;
class SVFIRAdapter;
class PointerAnalysis;
/// SVF-coordinate adaptation of all 30 feature formulas; see LearnedPacking.md.
/// It does not run impact or the downstream numerical interpreter.
SVF::PackingFeatureContext extractSAS16Features(
    SVFIR& ir, const SVFIRAdapter& adapter, PointerAnalysis& pts,
    const std::vector<SVF::Variable>& vocabulary, bool legacy=false);
}
#endif
