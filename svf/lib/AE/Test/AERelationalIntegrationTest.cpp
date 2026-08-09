//===- AERelationalIntegrationTest.cpp -- End-to-end AE/Octagon test ----===//

#include "AE/Svfexe/AbstractInterpretation.h"
#include "AE/Core/NonRelationalDomain.h"
#include "AE/Core/OctagonDomain.h"
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

using DenseOctagonState = AD::DomainProductState<AD::OctagonState>;

const DenseOctagonState& requireDenseOctagonState(
    AbstractInterpretation& analysis, const ICFGNode* node)
{
    const auto* state = dynamic_cast<const DenseOctagonState*>(
        &analysis.getAbstractState(node));
    if (!state)
        throw std::runtime_error(
            "dense AE is not using DomainProductState<OctagonState>");
    return *state;
}

bool hasIntegerBounds(const AD::Interval& interval, s64_t lower,
                      s64_t upper)
{
    return interval.lower().isFinite() && interval.upper().isFinite() &&
           interval.lower().value() == AD::Rational(lower) &&
           interval.upper().value() == AD::Rational(upper);
}

void validateSparseStorage(AbstractInterpretation& analysis)
{
    if (analysis.getIntervalTraceView().empty())
        throw std::runtime_error("sparse AE produced an empty trace");
    for (const auto& [node, projection] : analysis.getIntervalTraceView())
    {
        (void)projection;
        if (!dynamic_cast<const IntervalState*>(
                &analysis.getAbstractState(node)))
        {
            throw std::runtime_error(
                "sparse AE stopped using its IntervalState storage");
        }
    }
    std::cout << "AE_STORAGE_OBSERVATION mode=sparse"
              << " authoritative_state=IntervalState\n";
}

const SVFVar* findValueIfPresent(const SVFIR& graph, const std::string& name)
{
    for (SVFIR::const_iterator it = graph.begin(); it != graph.end(); ++it)
    {
        const SVFVar* value = it->second;
        const std::string& valueName = value->getValueName();
        if (valueName == name || valueName.rfind(name + " ", 0) == 0)
            return value;
    }
    return nullptr;
}

const SVFVar* requireValue(const SVFIR& graph, const std::string& name)
{
    if (const SVFVar* value = findValueIfPresent(graph, name))
        return value;
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

void validateReducedProductFixture(const SVFIR& graph,
                                   AbstractInterpretation& analysis)
{
    const SVFVar* input = requireValue(graph, "x");
    const SVFVar* affine = requireValue(graph, "y");
    const SVFVar* result = requireValue(graph, "z");
    const SVFVar* impossible = requireValue(graph, "bad");
    const auto* inputValue = SVFUtil::cast<ValVar>(input);
    const auto* affineValue = SVFUtil::cast<ValVar>(affine);
    const auto* resultValue = SVFUtil::cast<ValVar>(result);
    SVFIRAdapter adapter(graph);
    bool sawReducedInterval = false;
    bool sawReducedRelation = false;
    bool analyzedImpossibleValue = false;

    for (const auto& [node, state] : analysis.getIntervalTraceView())
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

        const DenseOctagonState& domainState =
            requireDenseOctagonState(analysis, node);
        const AD::OctagonState& octagon = domainState.numerical();
        const AD::LinearExpression inputExpression(
            adapter.variable(*inputValue));
        const AD::LinearExpression affineExpression(
            adapter.variable(*affineValue));
        if (hasIntegerBounds(octagon.bound(adapter.variable(*resultValue)),
                             1, 6) &&
                octagon.entails(AD::equal(inputExpression,
                                          affineExpression)) ==
                    AD::CheckResult::True)
            sawReducedRelation = true;
    }

    if (!sawReducedInterval || !sawReducedRelation ||
            analyzedImpossibleValue)
    {
        std::cerr << "relational integration trace:\n";
        for (const auto& [node, state] : analysis.getIntervalTraceView())
        {
            (void)state;
            bool wroteNode = false;
            for (const SVFVar* value :
                    {input, affine, result, impossible})
            {
                const bool hasInterval = analysis.hasAbsValue(value, node);
                if (!hasInterval)
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
                const auto* scalar = SVFUtil::dyn_cast<ValVar>(value);
                std::cerr << " Domain=";
                if (scalar && adapter.contains(*scalar))
                    std::cerr << requireDenseOctagonState(analysis, node)
                                     .numerical()
                                     .bound(adapter.variable(*scalar))
                                     .toString();
                else
                    std::cerr << "<not-numeric>";
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

    std::cout
        << "AE_RELATIONAL_OBSERVATION fixture=reduced-product"
        << " reduced_interval=1 reduced_relation=1 impossible_reachable=0\n";
}

void validateLoopFixture(const SVFIR& graph,
                         AbstractInterpretation& analysis)
{
    const SVFVar* induction = requireValue(graph, "i");
    const SVFVar* result = requireValue(graph, "loop_result");
    const SVFVar* impossible = requireValue(graph, "loop_bad");
    const auto* inductionValue = SVFUtil::cast<ValVar>(induction);
    SVFIRAdapter adapter(graph);
    bool sawPreciseResultInterval = false;
    bool sawExitRelation = false;
    bool analyzedImpossibleValue = false;

    for (const auto& [node, state] : analysis.getIntervalTraceView())
    {
        (void)state;
        if (analysis.hasAbsValue(impossible, node))
            analyzedImpossibleValue = true;
        if (!analysis.hasAbsValue(result, node))
            continue;
        const AbstractValue& value = analysis.getAbsValue(result, node);
        if (value.isInterval() && value.getInterval().equals(
                IntervalValue((s64_t)4)))
            sawPreciseResultInterval = true;
        const AD::Interval inductionBound =
            requireDenseOctagonState(analysis, node).numerical().bound(
                adapter.variable(*inductionValue));
        if (inductionBound.lower().isFinite() &&
                inductionBound.lower().value() == AD::Rational(4))
            sawExitRelation = true;
    }

    if (!sawPreciseResultInterval || !sawExitRelation ||
            analyzedImpossibleValue)
    {
        std::cerr << "loop integration trace:\n";
        for (const auto& [node, state] : analysis.getIntervalTraceView())
        {
            (void)state;
            std::cerr << "  node " << node->getId() << ':';
            for (const SVFVar* value : {induction, result, impossible})
            {
                std::cerr << ' ' << value->getValueName() << "=";
                if (analysis.hasAbsValue(value, node))
                    std::cerr << analysis.getAbsValue(value, node)
                                     .getInterval().toString();
                else
                    std::cerr << "<absent>";
                std::cerr << '/';
                const auto* scalar = SVFUtil::dyn_cast<ValVar>(value);
                if (scalar && adapter.contains(*scalar))
                    std::cerr << requireDenseOctagonState(analysis, node)
                                     .numerical()
                                     .bound(adapter.variable(*scalar))
                                     .toString();
                else
                    std::cerr << "<not-numeric>";
            }
            std::cerr << '\n';
        }
    }

    if (!sawPreciseResultInterval)
        throw std::runtime_error(
            "loop result did not retain the precise interval [4,4]");
    if (!sawExitRelation)
        throw std::runtime_error(
            "loop exit did not preserve the Octagon lower bound [4, +oo]");
    if (analyzedImpossibleValue)
        throw std::runtime_error(
            "post-loop infeasible branch remained reachable");

    std::cout
        << "AE_RELATIONAL_OBSERVATION fixture=loop"
        << " result_interval=[4,4] induction_lower_bound=4"
        << " impossible_reachable=0\n";
}

} // namespace

int main(int argc, char** argv)
{
    try
    {
        const std::vector<std::string> modules = OptionBase::parseOptions(
            argc, argv, "AE relational integration test",
            "[options] <input-bitcode>");
        if (!Options::AEDenseOctagon())
            throw std::runtime_error(
                "test requires -ae-dense-octagon=true");

        LLVMModuleSet::getLLVMModuleSet()->buildSVFModule(modules);
        SVFIRBuilder builder;
        SVFIR* graph = builder.build();
        AndersenWaveDiff* ander =
            AndersenWaveDiff::createAndersenWaveDiff(graph);
        builder.updateCallGraph(ander->getCallGraph());

        AbstractInterpretation& analysis =
            AbstractInterpretation::getAEInstance();
        analysis.runOnModule();

        if (Options::AESparsity() != AbstractInterpretation::Dense)
            validateSparseStorage(analysis);
        else if (findValueIfPresent(*graph, "loop_result"))
            validateLoopFixture(*graph, analysis);
        else
            validateReducedProductFixture(*graph, analysis);

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
