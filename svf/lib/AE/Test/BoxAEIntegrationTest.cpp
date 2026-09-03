//===- BoxAEIntegrationTest.cpp -- Box-backed AE integration test -------===//

#include "AE/Core/BoxProgramState.h"
#include "AE/Core/NumericalDomain.h"
#include "AE/Svfexe/AbstractInterpretation.h"
#include "AE/Svfexe/SVFIRAdapter.h"
#include "SVF-LLVM/SVFIRBuilder.h"
#include "Util/CommandLine.h"
#include "Util/Options.h"
#include "WPA/Andersen.h"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace SVF;

namespace
{
namespace AD = SVF::AbstractDomain;
using BoxProgramState = AD::BoxProgramState;

const SVFVar* findValue(const SVFIR& graph, const std::string& name)
{
    for (auto iterator = graph.begin(); iterator != graph.end(); ++iterator)
    {
        const SVFVar* value = iterator->second;
        const std::string& candidate = value->getValueName();
        if (candidate == name || candidate.rfind(name + " ", 0) == 0)
            return value;
    }
    return nullptr;
}

const BoxProgramState& requireBoxState(const AD::AbstractDomain& state)
{
    if (!state.isDomain<BoxProgramState>())
        throw std::runtime_error("AE property is not Box-backed");
    return static_cast<const BoxProgramState&>(state);
}

const BoxProgramState& stateForValue(AbstractInterpretation& analysis,
                                     const ValVar* value, const ICFGNode* node)
{
    if (const AD::AbstractDomain* scalar = analysis.getScalarAbstractState())
        return requireBoxState(*scalar);
    return requireBoxState(analysis.getAbstractState(node));
}

bool hasFiniteBounds(const AD::Interval& interval, s64_t lower, s64_t upper)
{
    return interval.lower().isFinite() && interval.upper().isFinite() &&
           interval.lower().value() == AD::Rational(lower) &&
           interval.upper().value() == AD::Rational(upper);
}

void validateAuthoritativeStorage(AbstractInterpretation& analysis)
{
    if (analysis.getAnalyzedNodes().empty())
        throw std::runtime_error("Box AE analyzed no ICFG nodes");
    for (const ICFGNode* node : analysis.getAnalyzedNodes())
        requireBoxState(analysis.getAbstractState(node));
}

void validateProjection(const SVFIR& graph, AbstractInterpretation& analysis)
{
    const bool loopFixture = findValue(graph, "loop_result") != nullptr;
    const SVFVar* result = findValue(graph, loopFixture ? "loop_result" : "z");
    if (!result)
        return;
    const auto* scalar = SVFUtil::dyn_cast<ValVar>(result);
    if (!scalar)
        throw std::runtime_error("Box fixture result is not an SSA value");

    const s64_t expectedLower = loopFixture ? 4 : 1;
    const s64_t expectedUpper = loopFixture ? 4 : 11;
    SVFIRAdapter adapter(graph);
    const AD::Variable variable = adapter.variable(*scalar);
    bool observed = false;
    std::string lastProjection = "<absent>";
    std::string lastStored = "<absent>";
    for (const ICFGNode* node : analysis.getAnalyzedNodes())
    {
        if (!analysis.hasAbsValue(scalar, node))
            continue;
        const AD::Interval projected = analysis.getInterval(scalar, node);
        const BoxProgramState& state = stateForValue(analysis, scalar, node);
        lastProjection = projected.toString();
        if (state.numerical().environment().contains(variable))
            lastStored = state.numerical().bound(variable).toString();
        if (hasFiniteBounds(projected, expectedLower, expectedUpper) &&
            state.numerical().environment().contains(variable) &&
            hasFiniteBounds(state.numerical().bound(variable), expectedLower,
                            expectedUpper))
            observed = true;
    }
    if (!observed)
        throw std::runtime_error(
            "Box numerical state and AE value projection diverged: projected=" +
            lastProjection + ", stored=" + lastStored);
}

void validateSparseMemoryRefinement(const SVFIR& graph,
                                    AbstractInterpretation& analysis)
{
    const SVFVar* result = findValue(graph, "memory_result");
    if (!result)
        return;
    bool observedPositive = false;
    for (const ICFGNode* node : analysis.getAnalyzedNodes())
    {
        if (!analysis.hasAbsValue(result, node))
            continue;
        const AD::Interval value = analysis.getInterval(result, node);
        observedPositive |= value.lower().isFinite() &&
                            value.lower().value() == AD::Rational(1);
    }
    if (!observedPositive)
        throw std::runtime_error(
            "Box sparse memory refinement did not reach the second load");
}
} // namespace

int main(int argc, char** argv)
{
    try
    {
        const std::vector<std::string> modules = OptionBase::parseOptions(
            argc, argv, "Box AE integration test", "[options] <input-bitcode>");
        LLVMModuleSet::getLLVMModuleSet()->buildSVFModule(modules);
        SVFIRBuilder builder;
        SVFIR* graph = builder.build();
        AndersenWaveDiff* ander =
            AndersenWaveDiff::createAndersenWaveDiff(graph);
        builder.updateCallGraph(ander->getCallGraph());

        AbstractInterpretation& analysis =
            AbstractInterpretation::getAEInstance();
        analysis.runOnModule();
        validateAuthoritativeStorage(analysis);
        validateProjection(*graph, analysis);
        validateSparseMemoryRefinement(*graph, analysis);

        std::cout << "Box AE integration test: PASS\n";
        AndersenWaveDiff::releaseAndersenWaveDiff();
        LLVMModuleSet::releaseLLVMModuleSet();
        return EXIT_SUCCESS;
    }
    catch (const std::exception& error)
    {
        std::cerr << "Box AE integration test: FAIL: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
