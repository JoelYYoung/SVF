//===- ApronOctagonAE.cpp -- Dense AE backed by APRON octMPQ -------------===//

#include "ApronOctagonState.h"

#include "AE/Svfexe/DenseAbstractInterpretation.h"
#include "SVF-LLVM/SVFIRBuilder.h"
#include "Util/CommandLine.h"
#include "WPA/Andersen.h"

#include <algorithm>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace SVF;

namespace
{

using DenseApronState = AbstractDomain::DomainProductState<
    AbstractDomain::ApronOctagonState>;

void reportSemanticChecksum(
    DenseAbstractInterpretation<AbstractDomain::ApronOctagonState>& analysis)
{
    std::vector<std::pair<NodeID, std::uint64_t>> states;
    for (const ICFGNode* node : analysis.getAnalyzedNodes())
    {
        if (!analysis.hasAbsState(node))
            continue;
        const AbstractDomain::AbstractState& abstractState =
            analysis.getAbstractState(node);
        if (!abstractState.isState<DenseApronState>())
            throw std::runtime_error(
                "APRON checksum expected a dense product state");
        const auto& state = static_cast<const DenseApronState&>(abstractState);
        states.emplace_back(node->getId(), state.numerical().hash());
    }
    std::sort(states.begin(), states.end());
    std::uint64_t checksum = 1469598103934665603ULL;
    auto append = [&](std::uint64_t value)
    {
        for (unsigned byte = 0; byte != sizeof(value); ++byte)
        {
            checksum ^= (value >> (byte * 8U)) & 0xffU;
            checksum *= 1099511628211ULL;
        }
    };
    for (const auto& [node, state] : states)
    {
        append(node);
        append(state);
    }
    std::cout << "AE_NUMERICAL_CHECKSUM domain=octagon states="
              << states.size() << " checksum=" << std::hex
              << std::setfill('0') << std::setw(16) << checksum << std::dec
              << '\n';
}

} // namespace

int main(int argc, char** argv)
{
    try
    {
        std::vector<std::string> modules = OptionBase::parseOptions(
            argc, argv, "Dense AE with APRON octMPQ",
            "[options] <input-bitcode>");
        LLVMModuleSet::getLLVMModuleSet()->buildSVFModule(modules);
        SVFIRBuilder builder;
        SVFIR* graph = builder.build();
        AndersenWaveDiff* ander =
            AndersenWaveDiff::createAndersenWaveDiff(graph);
        builder.updateCallGraph(ander->getCallGraph());

        DenseAbstractInterpretation<
            AbstractDomain::ApronOctagonState> analysis;
        analysis.runOnModule();
        const char* checksum =
            std::getenv("SVF_RELATIONAL_SEMANTIC_CHECKSUM");
        if (checksum && checksum[0] != '\0' && std::string(checksum) != "0")
            reportSemanticChecksum(analysis);
        std::cout << "AE_GENERIC_OBSERVATION analyzed_nodes="
                  << analysis.getAnalyzedNodes().size() << '\n';

        AndersenWaveDiff::releaseAndersenWaveDiff();
        LLVMModuleSet::releaseLLVMModuleSet();
        std::cout << "APRON Octagon dense AE: PASS\n";
        return EXIT_SUCCESS;
    }
    catch (const std::exception& error)
    {
        std::cerr << "APRON Octagon dense AE: FAIL: " << error.what()
                  << '\n';
        return EXIT_FAILURE;
    }
}
