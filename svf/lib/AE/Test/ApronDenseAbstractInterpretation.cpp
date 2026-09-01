// Instantiate the production Dense AE template for the optional APRON state.
#include "ApronOctagonState.h"

#define SVF_DENSE_AE_SUPPRESS_EXPLICIT_INSTANTIATIONS
#include "../Svfexe/DenseAbstractInterpretation.cpp"

namespace SVF
{
template class DenseAbstractInterpretation<
    AbstractDomain::ApronOctagonState>;
}
