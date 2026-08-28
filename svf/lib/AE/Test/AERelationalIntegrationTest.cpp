//===- AERelationalIntegrationTest.cpp -- End-to-end AE/Octagon test ----===//

#include "AE/Core/BoxDomain.h"
#include "AE/Core/NonRelationalDomain.h"
#include "AE/Core/OctagonDomain.h"
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
#include <type_traits>
#include <vector>

using namespace SVF;

namespace
{

namespace AD = SVF::AbstractDomain;

using DenseOctagonState = AD::DomainProductState<AD::OctagonState>;
using DenseBoxState = AD::DomainProductState<AD::BoxState>;

const DenseOctagonState& requireDenseOctagonState(
    AbstractInterpretation& analysis, const ICFGNode* node)
{
    const AD::AbstractState* scalarState =
        analysis.getScalarAbstractState(node->getFun());
    const AD::AbstractState& abstractState =
        scalarState ? *scalarState : analysis.getAbstractState(node);
    const auto* state =
        abstractState.isState<DenseOctagonState>()
            ? &static_cast<const DenseOctagonState&>(abstractState)
            : nullptr;
    if (!state)
        throw std::runtime_error(
            "dense AE is not using DomainProductState<OctagonState>");
    return *state;
}

const DenseBoxState& requireDenseBoxState(AbstractInterpretation& analysis,
                                          const ICFGNode* node)
{
    const AD::AbstractState* scalarState =
        analysis.getScalarAbstractState(node->getFun());
    const AD::AbstractState& abstractState =
        scalarState ? *scalarState : analysis.getAbstractState(node);
    const auto* state = abstractState.isState<DenseBoxState>()
                            ? &static_cast<const DenseBoxState&>(abstractState)
                            : nullptr;
    if (!state)
        throw std::runtime_error(
            "dense AE is not using DomainProductState<BoxState>");
    return *state;
}

const DenseOctagonState& requireScalarOctagonState(
    AbstractInterpretation& analysis, const ValVar* value,
    const ICFGNode* fallback)
{
    const AD::AbstractState* scalarState =
        analysis.getScalarAbstractState(value);
    if (!scalarState)
        return requireDenseOctagonState(analysis, fallback);
    if (!scalarState->isState<DenseOctagonState>())
        throw std::runtime_error(
            "sparse scalar checkpoint is not an Octagon product state");
    return static_cast<const DenseOctagonState&>(*scalarState);
}

bool hasIntegerBounds(const AD::Interval& interval, s64_t lower, s64_t upper)
{
    return interval.lower().isFinite() && interval.upper().isFinite() &&
           interval.lower().value() == AD::Rational(lower) &&
           interval.upper().value() == AD::Rational(upper);
}

const SVFVar* findValueIfPresent(const SVFIR& graph, const std::string& name);
const SVFVar* requireValue(const SVFIR& graph, const std::string& name);

void validateSparseStorage(AbstractInterpretation& analysis)
{
    if (analysis.getIntervalTraceView().empty())
        throw std::runtime_error("sparse AE produced an empty trace");
    for (const auto& [node, projection] : analysis.getIntervalTraceView())
    {
        (void)projection;
        if (!analysis.getAbstractState(node).isState<IntervalState>())
        {
            throw std::runtime_error(
                "sparse AE stopped using its IntervalState storage");
        }
    }
    std::cout << "AE_STORAGE_OBSERVATION mode=sparse"
              << " authoritative_state=IntervalState\n";
}

void validateLegacyDenseStorage(AbstractInterpretation& analysis)
{
    if (analysis.getIntervalTraceView().empty())
        throw std::runtime_error("legacy dense AE produced an empty trace");
    for (const auto& [node, state] : analysis.getIntervalTraceView())
    {
        (void)state;
        if (!analysis.getAbstractState(node).isState<IntervalState>())
            throw std::runtime_error(
                "legacy dense AE stopped using IntervalState storage");
    }
    std::cout << "AE_STORAGE_OBSERVATION mode=dense-legacy"
              << " authoritative_state=IntervalState\n";
}

void validateLegacyDenseSemantics(const SVFIR& graph,
                                  AbstractInterpretation& analysis)
{
    const bool loopFixture =
        findValueIfPresent(graph, "loop_result") != nullptr;
    const SVFVar* result =
        requireValue(graph, loopFixture ? "loop_result" : "z");
    // This is intentionally the historical baseline, not an equivalence
    // oracle: the original IntervalState engine loses these loop/branch
    // bounds, while native Box retains [4,4] and [1,11], respectively.
    const IntervalValue expected = IntervalValue::top();
    bool sawExpectedResult = false;
    for (const ICFGNode* node : analysis.getAnalyzedNodes())
    {
        if (!analysis.hasAbsValue(result, node))
            continue;
        const AbstractValue value = analysis.getAbsValue(result, node);
        if (value.isInterval() && value.getInterval().equals(expected))
            sawExpectedResult = true;
    }
    if (!sawExpectedResult)
    {
        std::cerr << "legacy result trace for " << result->getValueName()
                  << ":\n";
        for (const ICFGNode* node : analysis.getAnalyzedNodes())
        {
            if (analysis.hasAbsValue(result, node))
                std::cerr << "  node " << node->getId() << " = "
                          << analysis.getAbsValue(result, node).toString()
                          << '\n';
        }
        throw std::runtime_error(
            "legacy dense AE changed its historical interval result");
    }
    std::cout << "AE_LEGACY_OBSERVATION fixture="
              << (loopFixture ? "loop" : "reduced-product")
              << " historical_result=top\n";
}

template <typename DenseStateT>
void validateNativeProductStorage(AbstractInterpretation& analysis)
{
    if (!analysis.getIntervalTraceView().empty())
        throw std::runtime_error(
            "native product AE retained a compatibility IntervalState trace");
    if (analysis.getAnalyzedNodes().empty())
        throw std::runtime_error("native product AE analyzed no nodes");
    for (const ICFGNode* node : analysis.getAnalyzedNodes())
    {
        if (!analysis.getAbstractState(node).isState<DenseStateT>())
            throw std::runtime_error(
                "native product AE exposed a non-product authoritative state");
    }
    const char* mode = Options::AESparsity() == AbstractInterpretation::Dense
                           ? "dense-native"
                       : Options::AESparsity() ==
                                 AbstractInterpretation::SemiSparse
                           ? "semi-sparse-native"
                           : "sparse-native";
    std::cout << "AE_STORAGE_OBSERVATION mode=" << mode
              << " authoritative_state=DomainProductState"
              << " compatibility_trace_entries=0\n";
}

template <typename DenseStateT>
void validateNativeSparseCarrierSeparation(const SVFIR& graph,
                                           AbstractInterpretation& analysis)
{
    SVFIRAdapter adapter(graph);
    bool sawScalarCarrier = false;
    bool sawDefinedScalar = false;
    const AD::AbstractState* boxModuleCarrier = nullptr;
    Set<const FunObjVar*> checkedFunctions;
    for (const ICFGNode* node : analysis.getAnalyzedNodes())
    {
        const auto& memory = static_cast<const DenseStateT&>(
            analysis.getAbstractState(node));
        for (AD::Variable variable : memory.shapes().definedVariables(
                 memory.numerical().environment()))
        {
            if (adapter.value(variable))
                throw std::runtime_error(
                    "native sparse ICFG carrier retained a ValVar value");
        }

        if (!checkedFunctions.insert(node->getFun()).second)
            continue;
        const AD::AbstractState* abstractScalars =
            analysis.getScalarAbstractState(node->getFun());
        if (!abstractScalars)
            continue;
        if constexpr (std::is_same_v<DenseStateT, DenseBoxState>)
        {
            if (!boxModuleCarrier)
                boxModuleCarrier = abstractScalars;
            else if (boxModuleCarrier != abstractScalars)
                throw std::runtime_error(
                    "native sparse Box created more than one scalar carrier");
        }
        sawScalarCarrier = true;
        const auto& scalars = static_cast<const DenseStateT&>(*abstractScalars);
        for (AD::Variable variable : scalars.shapes().definedVariables(
                 scalars.numerical().environment()))
        {
            if (adapter.value(variable))
                sawDefinedScalar = true;
            if (adapter.contentObject(variable))
                throw std::runtime_error(
                    "native sparse scalar carrier retained ObjVar content");
        }
    }
    if (!sawScalarCarrier || !sawDefinedScalar)
        throw std::runtime_error(
            "native sparse analysis did not populate its ValVar carrier");
    if constexpr (std::is_same_v<DenseStateT, DenseBoxState>)
    {
        if (analysis.getScalarAbstractState(
                static_cast<const FunObjVar*>(nullptr)) != boxModuleCarrier)
            throw std::runtime_error(
                "native sparse Box did not expose its module scalar carrier");
        std::cout << "AE_CARRIER_OBSERVATION icfg=memory-only"
                  << " scalar=module-box valvar_attached_to_icfg=0\n";
    }
    else
    {
        std::cout << "AE_CARRIER_OBSERVATION icfg=memory-only"
                  << " scalar=per-function-relational"
                  << " valvar_attached_to_icfg=0\n";
    }
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

    for (const ICFGNode* node : analysis.getAnalyzedNodes())
    {
        if (analysis.hasAbsValue(impossible, node))
            analyzedImpossibleValue = true;
        if (!analysis.hasAbsValue(result, node))
            continue;

        const AbstractValue& value = analysis.getAbsValue(result, node);
        if (value.isInterval() &&
            value.getInterval().equals(IntervalValue((s64_t)1, (s64_t)6)))
            sawReducedInterval = true;

        const DenseOctagonState& domainState =
            requireScalarOctagonState(analysis, resultValue, node);
        const AD::OctagonState& octagon = domainState.numerical();
        const AD::LinearExpression inputExpression(
            adapter.variable(*inputValue));
        const AD::LinearExpression affineExpression(
            adapter.variable(*affineValue));
        if (hasIntegerBounds(octagon.bound(adapter.variable(*resultValue)), 1,
                             6) &&
            octagon.entails(AD::equal(inputExpression, affineExpression)) ==
                AD::CheckResult::True)
            sawReducedRelation = true;
    }

    if (!sawReducedInterval || !sawReducedRelation || analyzedImpossibleValue)
    {
        std::cerr << "relational integration trace:\n";
        for (const ICFGNode* node : analysis.getAnalyzedNodes())
        {
            bool wroteNode = false;
            for (const SVFVar* value : {input, affine, result, impossible})
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
                                     .getInterval()
                                     .toString();
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

void validateRelationalAssertFixture(const SVFIR& graph,
                                     AbstractInterpretation& analysis)
{
    const SVFVar* input = requireValue(graph, "a");
    const SVFVar* copy = requireValue(graph, "assert_x");
    const SVFVar* failure = requireValue(graph, "assert_bad");
    const auto* inputValue = SVFUtil::cast<ValVar>(input);
    const auto* copyValue = SVFUtil::cast<ValVar>(copy);
    SVFIRAdapter adapter(graph);
    const AD::Variable inputVariable = adapter.variable(*inputValue);
    const AD::Variable copyVariable = adapter.variable(*copyValue);
    const AD::LinearConstraint copyRelation = AD::equal(
        AD::LinearExpression(copyVariable),
        AD::LinearExpression(inputVariable));
    const AD::LinearConstraint assertion = AD::greaterThan(
        AD::LinearExpression(copyVariable),
        AD::LinearExpression(AD::Rational(0)));
    const bool separatedScalarCarrier =
        analysis.getScalarAbstractState(copyValue) != nullptr;
    bool sawProvedAssertion = false;
    bool analyzedFailure = false;

    for (const ICFGNode* node : analysis.getAnalyzedNodes())
    {
        if (analysis.hasAbsValue(failure, node))
            analyzedFailure = true;
        if (!analysis.hasAbsValue(copy, node))
            continue;

        const AD::OctagonState& octagon =
            requireScalarOctagonState(analysis, copyValue, node).numerical();
        if (octagon.entails(copyRelation) == AD::CheckResult::True &&
            (separatedScalarCarrier ||
             octagon.entails(assertion) == AD::CheckResult::True))
            sawProvedAssertion = true;
    }

    if (!sawProvedAssertion || analyzedFailure)
    {
        std::cerr << "relational assertion trace:\n";
        for (const ICFGNode* node : analysis.getAnalyzedNodes())
        {
            std::cerr << "  node " << node->getId();
            for (const SVFVar* value : {input, copy, failure})
            {
                std::cerr << ' ' << value->getValueName() << '=';
                if (analysis.hasAbsValue(value, node))
                    std::cerr << analysis.getAbsValue(value, node).toString();
                else
                    std::cerr << "<absent>";
            }
            std::cerr << '\n';
        }
    }

    if (!sawProvedAssertion)
        throw std::runtime_error(
            "Octagon did not retain x == a at the sparse definition");
    if (analyzedFailure)
        throw std::runtime_error(
            "the relationally impossible assertion failure was reachable");

    std::cout << "AE_RELATIONAL_OBSERVATION fixture=assert-copy"
              << " copy_relation=1 assertion_proved=1"
              << " failure_reachable=0\n";
}

void validateLoopFixture(const SVFIR& graph, AbstractInterpretation& analysis)
{
    const SVFVar* induction = requireValue(graph, "i");
    const SVFVar* result = requireValue(graph, "loop_result");
    const SVFVar* impossible = requireValue(graph, "loop_bad");
    const auto* inductionValue = SVFUtil::cast<ValVar>(induction);
    SVFIRAdapter adapter(graph);
    bool sawPreciseResultInterval = false;
    bool sawExitRelation = false;
    bool analyzedImpossibleValue = false;

    for (const ICFGNode* node : analysis.getAnalyzedNodes())
    {
        if (analysis.hasAbsValue(impossible, node))
            analyzedImpossibleValue = true;
        if (!analysis.hasAbsValue(result, node))
            continue;
        const AbstractValue& value = analysis.getAbsValue(result, node);
        if (value.isInterval() &&
            value.getInterval().equals(IntervalValue((s64_t)4)))
            sawPreciseResultInterval = true;
        const AD::Interval inductionBound =
            requireScalarOctagonState(
                analysis, SVFUtil::cast<ValVar>(result), node)
                .numerical()
                .bound(adapter.variable(*inductionValue));
        if (inductionBound.lower().isFinite() &&
            inductionBound.lower().value() == AD::Rational(4))
            sawExitRelation = true;
    }

    if (!sawPreciseResultInterval || !sawExitRelation ||
        analyzedImpossibleValue)
    {
        std::cerr << "loop integration trace:\n";
        for (const ICFGNode* node : analysis.getAnalyzedNodes())
        {
            std::cerr << "  node " << node->getId() << ':';
            for (const SVFVar* value : {induction, result, impossible})
            {
                std::cerr << ' ' << value->getValueName() << "=";
                if (analysis.hasAbsValue(value, node))
                    std::cerr << analysis.getAbsValue(value, node)
                                     .getInterval()
                                     .toString();
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

    std::cout << "AE_RELATIONAL_OBSERVATION fixture=loop"
              << " result_interval=[4,4] induction_lower_bound=4"
              << " impossible_reachable=0\n";
}

void validateBoxProjection(const SVFIR& graph, AbstractInterpretation& analysis)
{
    const bool loopFixture =
        findValueIfPresent(graph, "loop_result") != nullptr;
    const SVFVar* result =
        requireValue(graph, loopFixture ? "loop_result" : "z");
    const auto* resultValue = SVFUtil::cast<ValVar>(result);
    const s64_t expectedLower = loopFixture ? 4 : 1;
    // Box cannot carry the x == y relation through the x <= 5 branch, so the
    // reduced-product fixture intentionally retains y in [0,10].
    const s64_t expectedUpper = loopFixture ? 4 : 11;
    SVFIRAdapter adapter(graph);
    const AD::Variable resultVariable = adapter.variable(*resultValue);
    bool sawExpectedProjection = false;

    for (const ICFGNode* node : analysis.getAnalyzedNodes())
    {
        if (!analysis.hasAbsValue(result, node))
            continue;
        const AbstractValue& cached = analysis.getAbsValue(result, node);
        const AD::Interval domainBound = requireDenseBoxState(analysis, node)
                                             .numerical()
                                             .bound(resultVariable);
        if (cached.isInterval() &&
            cached.getInterval().equals(
                IntervalValue(expectedLower, expectedUpper)) &&
            hasIntegerBounds(domainBound, expectedLower, expectedUpper))
            sawExpectedProjection = true;
    }

    if (!sawExpectedProjection)
    {
        std::cerr << "Box projection trace for " << result->getValueName()
                  << ":\n";
        for (const ICFGNode* node : analysis.getAnalyzedNodes())
        {
            std::cerr << "  node " << node->getId() << " projection=";
            if (analysis.hasAbsValue(result, node))
                std::cerr << analysis.getAbsValue(result, node).toString();
            else
                std::cerr << "<absent>";
            const AD::BoxState& numerical =
                requireDenseBoxState(analysis, node).numerical();
            std::cerr << " box=";
            if (numerical.environment().contains(resultVariable))
                std::cerr << numerical.bound(resultVariable).toString();
            else
                std::cerr << "<absent>";
            std::cerr << '\n';
        }
        throw std::runtime_error(
            "Dense Box state and compatibility projection diverged");
    }
    std::cout << "AE_BOX_OBSERVATION fixture="
              << (loopFixture ? "loop" : "reduced-product")
              << " projection_matches_domain=1\n";
}

void validateSparseMemoryFixture(const SVFIR& graph,
                                 AbstractInterpretation& analysis,
                                 bool requireRefinement)
{
    const SVFVar* result = requireValue(graph, "memory_result");
    bool sawPositiveResult = false;
    for (const ICFGNode* node : analysis.getAnalyzedNodes())
    {
        if (!analysis.hasAbsValue(result, node))
            continue;
        const AbstractValue value = analysis.getAbsValue(result, node);
        if (value.isInterval() && !value.getInterval().lb().is_infinity() &&
            value.getInterval().lb().getNumeral() == 1)
            sawPositiveResult = true;
    }
    if (requireRefinement && !sawPositiveResult)
        throw std::runtime_error(
            "sparse memory branch refinement did not reach the second load");
    if (!requireRefinement && sawPositiveResult)
        throw std::runtime_error(
            "legacy sparse memory result unexpectedly changed");
    std::cout << "AE_SPARSE_MEMORY_OBSERVATION reaching_store=1"
              << " branch_refinement=" << sawPositiveResult
              << " result_lower_bound="
              << (sawPositiveResult ? "1" : "top") << '\n';
}

} // namespace

int main(int argc, char** argv)
{
    try
    {
        const std::vector<std::string> modules = OptionBase::parseOptions(
            argc, argv, "AE relational integration test",
            "[options] <input-bitcode>");
        LLVMModuleSet::getLLVMModuleSet()->buildSVFModule(modules);
        SVFIRBuilder builder;
        SVFIR* graph = builder.build();
        AndersenWaveDiff* ander =
            AndersenWaveDiff::createAndersenWaveDiff(graph);
        builder.updateCallGraph(ander->getCallGraph());

        AbstractInterpretation& analysis =
            AbstractInterpretation::getAEInstance();
        analysis.runOnModule();

        const bool nativeProduct = analysis.getIntervalTraceView().empty();
        if (Options::AESparsity() != AbstractInterpretation::Dense &&
            !nativeProduct)
            validateSparseStorage(analysis);
        else if (Options::AEDenseLegacyInterval())
        {
            validateLegacyDenseStorage(analysis);
            if (findValueIfPresent(*graph, "z") ||
                findValueIfPresent(*graph, "loop_result"))
                validateLegacyDenseSemantics(*graph, analysis);
        }
        else if (!Options::AEDenseOctagon())
        {
            validateNativeProductStorage<DenseBoxState>(analysis);
            const bool carrierFixture = findValueIfPresent(*graph, "z") ||
                findValueIfPresent(*graph, "loop_result") ||
                findValueIfPresent(*graph, "memory_result");
            if (Options::AESparsity() != AbstractInterpretation::Dense &&
                carrierFixture)
                validateNativeSparseCarrierSeparation<DenseBoxState>(
                    *graph, analysis);
            if (findValueIfPresent(*graph, "z") ||
                findValueIfPresent(*graph, "loop_result"))
                validateBoxProjection(*graph, analysis);
        }
        else if (findValueIfPresent(*graph, "assert_bad"))
        {
            validateNativeProductStorage<DenseOctagonState>(analysis);
            if (Options::AESparsity() != AbstractInterpretation::Dense)
                validateNativeSparseCarrierSeparation<DenseOctagonState>(
                    *graph, analysis);
            validateRelationalAssertFixture(*graph, analysis);
        }
        else if (findValueIfPresent(*graph, "loop_result"))
        {
            validateNativeProductStorage<DenseOctagonState>(analysis);
            if (Options::AESparsity() != AbstractInterpretation::Dense)
                validateNativeSparseCarrierSeparation<DenseOctagonState>(
                    *graph, analysis);
            validateLoopFixture(*graph, analysis);
        }
        else if (findValueIfPresent(*graph, "z"))
        {
            validateNativeProductStorage<DenseOctagonState>(analysis);
            if (Options::AESparsity() != AbstractInterpretation::Dense)
                validateNativeSparseCarrierSeparation<DenseOctagonState>(
                    *graph, analysis);
            validateReducedProductFixture(*graph, analysis);
        }
        else
        {
            validateNativeProductStorage<DenseOctagonState>(analysis);
        }

        if (findValueIfPresent(*graph, "memory_result"))
            validateSparseMemoryFixture(*graph, analysis, nativeProduct);
        std::cout << "AE_GENERIC_OBSERVATION analyzed_nodes="
                  << analysis.getAnalyzedNodes().size() << '\n';

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
