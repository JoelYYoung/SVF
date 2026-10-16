// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef SVF_AE_SAS16_FEATURE_EXTRACTION_H
#define SVF_AE_SAS16_FEATURE_EXTRACTION_H
#include "AE/Core/LearnedRelationPacking.h"
namespace SVF
{
class SVFIR; class SVFIRAdapter; class PointerAnalysis;
/// SVF-coordinate adaptation of all 30 feature formulas; see LearnedPacking.md.
/// It does not run impact or the downstream numerical interpreter.
AbstractDomain::PackingFeatureContext extractSAS16Features(
    SVFIR& ir, const SVFIRAdapter& adapter, PointerAnalysis& pts,
    const std::vector<AbstractDomain::Variable>& vocabulary, bool legacy=false);
}
#endif
