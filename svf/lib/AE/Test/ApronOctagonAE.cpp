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
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace SVF;

namespace
{

using DenseApronState = AbstractDomain::DomainProductState<
    AbstractDomain::ApronOctagonState>;

std::uint64_t supportNormalizedHash(
    const AbstractDomain::ApronOctagonState& numerical)
{
    const AbstractDomain::LinearConstraintSet constraints =
        numerical.toConstraints();
    std::set<AbstractDomain::Variable> support;
    for (const AbstractDomain::LinearConstraint& constraint : constraints)
        for (const auto& [variable, coefficient] :
             constraint.expression().terms())
        {
            (void)coefficient;
            support.insert(variable);
        }
    std::vector<AbstractDomain::VariableDeclaration> declarations;
    declarations.reserve(support.size());
    for (const AbstractDomain::VariableDeclaration& declaration :
         numerical.environment().variables())
        if (support.count(declaration.variable) != 0)
            declarations.push_back(declaration);
    AbstractDomain::ApronOctagonState projected = numerical;
    projected.changeEnvironment(AbstractDomain::VariableEnvironment(
        std::move(declarations)));
    return projected.hash();
}

void reportSemanticChecksum(
    DenseAbstractInterpretation<AbstractDomain::ApronOctagonState>& analysis)
{
    const bool traceStates =
        std::getenv("SVF_RELATIONAL_SEMANTIC_TRACE") != nullptr;
    std::optional<NodeID> dumpNode;
    if (const char* value =
            std::getenv("SVF_RELATIONAL_SEMANTIC_DUMP_NODE"))
    {
        char* end = nullptr;
        const unsigned long parsed = std::strtoul(value, &end, 10);
        if (!end || *end != '\0')
            throw std::runtime_error(
                "SVF_RELATIONAL_SEMANTIC_DUMP_NODE must be a node ID");
        dumpNode = static_cast<NodeID>(parsed);
    }
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
        states.emplace_back(node->getId(),
                            supportNormalizedHash(state.numerical()));
        if (dumpNode == node->getId())
        {
            const auto canonical = AbstractDomain::OctagonState::fromConstraints(
                state.numerical().environment(),
                state.numerical().toConstraints());
            std::cout << "AE_NUMERICAL_STATE_DUMP domain=octagon"
                      << " node=" << node->getId() << " state="
                      << canonical.toString() << '\n';
            std::cout << "AE_NUMERICAL_ENVIRONMENT domain=octagon"
                      << " node=" << node->getId() << " variables=";
            bool first = true;
            for (const AbstractDomain::VariableDeclaration& declaration :
                 state.numerical().environment().variables())
            {
                if (!first)
                    std::cout << ',';
                first = false;
                std::cout << declaration.variable.id() << ':'
                          << static_cast<unsigned>(declaration.type.kind) << ':'
                          << declaration.name;
            }
            std::cout << '\n';
        }
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
        if (traceStates)
            std::cout << "AE_NUMERICAL_STATE domain=octagon"
                      << " node=" << node << " hash=" << std::hex
                      << std::setfill('0') << std::setw(16) << state
                      << std::dec << '\n';
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
