//===- AERelationalIntegrationTest.cpp -- End-to-end AE/Octagon test ----===//

#include "AE/Svfexe/AbstractInterpretation.h"
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

const SVFVar* findValue(const SVFIR& graph, const std::string& name)
{
    for (SVFIR::const_iterator it = graph.begin(); it != graph.end(); ++it)
    {
        const SVFVar* value = it->second;
        const std::string& valueName = value->getValueName();
        if (valueName == name || valueName.rfind(name + " ", 0) == 0)
            return value;
    }
    std::cerr << "available named SVF values:";
    for (SVFIR::const_iterator it = graph.begin(); it != graph.end(); ++it)
    {
        const std::string valueName = it->second->getValueName();
        if (!valueName.empty())
            std::cerr << ' ' << valueName;
    }
    std::cerr << '\n';
    throw std::runtime_error("missing SVF value: " + name);
}

} // namespace

int main(int argc, char** argv)
{
    try
    {
        const std::vector<std::string> modules = OptionBase::parseOptions(
            argc, argv, "AE relational integration test",
            "[options] <input-bitcode>");
        if (!Options::AERelational())
            throw std::runtime_error("test requires -ae-relational=true");

        LLVMModuleSet::getLLVMModuleSet()->buildSVFModule(modules);
        SVFIRBuilder builder;
        SVFIR* graph = builder.build();
        AndersenWaveDiff* ander =
            AndersenWaveDiff::createAndersenWaveDiff(graph);
        builder.updateCallGraph(ander->getCallGraph());

        AbstractInterpretation& analysis =
            AbstractInterpretation::getAEInstance();
        analysis.runOnModule();

        const SVFVar* input = findValue(*graph, "x");
        const SVFVar* affine = findValue(*graph, "y");
        const SVFVar* result = findValue(*graph, "z");
        const SVFVar* impossible = findValue(*graph, "bad");
        bool sawReducedInterval = false;
        bool sawReducedRelation = false;
        bool analyzedImpossibleValue = false;

        for (const auto& [node, state] : analysis.getTrace())
        {
            (void)state;
            if (analysis.hasAbsValue(impossible, node))
                analyzedImpossibleValue = true;
            if (!analysis.hasAbsValue(result, node))
                continue;

            const AbstractValue& value = analysis.getAbsValue(result, node);
            if (value.isInterval() && value.getInterval().equals(
                    IntervalValue((s64_t)1, (s64_t)6)))
                sawReducedInterval = true;

            const SVFRelationalBridge* relational =
                analysis.getRelationalState(node);
            if (relational && relational->tracks(result->getId()) &&
                    relational->projectInterval(result->getId()).equals(
                        IntervalValue((s64_t)1, (s64_t)6)))
                sawReducedRelation = true;
        }

        if (!sawReducedInterval || !sawReducedRelation ||
                analyzedImpossibleValue)
        {
            std::cerr << "relational integration trace:\n";
            for (const auto& [node, state] : analysis.getTrace())
            {
                (void)state;
                bool wroteNode = false;
                for (const SVFVar* value :
                        {input, affine, result, impossible})
                {
                    const SVFRelationalBridge* relational =
                        analysis.getRelationalState(node);
                    const bool hasInterval = analysis.hasAbsValue(value, node);
                    const bool hasRelation =
                        relational && relational->tracks(value->getId());
                    if (!hasInterval && !hasRelation)
                        continue;
                    if (!wroteNode)
                    {
                        std::cerr << "  node " << node->getId() << ":\n";
                        wroteNode = true;
                    }
                    std::cerr << "    " << value->getValueName() << " AE=";
                    if (hasInterval)
                        std::cerr << analysis.getAbsValue(value, node)
                                         .getInterval().toString();
                    else
                        std::cerr << "<absent>";
                    std::cerr << " Octagon=";
                    if (hasRelation)
                        std::cerr << relational->projectInterval(
                                             value->getId()).toString();
                    else
                        std::cerr << "<untracked>";
                    std::cerr << '\n';
                }
            }
        }

        if (!sawReducedInterval)
            throw std::runtime_error(
                "Octagon bounds were not reduced into the AE interval trace");
        if (!sawReducedRelation)
            throw std::runtime_error(
                "AE transfers did not reach the Octagon trace");
        if (analyzedImpossibleValue)
            throw std::runtime_error(
                "a relationally infeasible CFG edge remained reachable");

        AndersenWaveDiff::releaseAndersenWaveDiff();
        LLVMModuleSet::releaseLLVMModuleSet();
        std::cout << "AE relational integration test: PASS\n";
        return EXIT_SUCCESS;
    }
    catch (const std::exception& error)
    {
        std::cerr << "AE relational integration test: FAIL: " << error.what()
                  << '\n';
        return EXIT_FAILURE;
    }
}
