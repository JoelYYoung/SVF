//===- AERelationalIntegrationTest.cpp -- End-to-end AE/Octagon test ----===//

#include "AE/Core/BoxDomain.h"
#include "AE/Core/ConvexPolyhedraDomain.h"
#include "AE/Core/NonRelationalDomain.h"
#include "AE/Core/OctagonDomain.h"
#include "AE/Svfexe/AbstractInterpretation.h"
#include "AE/Svfexe/SVFIRAdapter.h"
#include "SVF-LLVM/SVFIRBuilder.h"
#include "Util/CommandLine.h"
#include "Util/Options.h"
#include "WPA/Andersen.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <type_traits>
#include <vector>

using namespace SVF;

namespace
{

namespace AD = SVF::AbstractDomain;

using DenseOctagonState = AD::DomainProductState<AD::OctagonState>;
using DenseBoxState = AD::DomainProductState<AD::BoxState>;
using DensePolyhedraState =
    AD::DomainProductState<AD::ConvexPolyhedraState>;

template <typename DenseState>
void reportNumericalSemanticChecksum(const char* domain,
                                     AbstractInterpretation& analysis)
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
    if (const char* value =
            std::getenv("SVF_RELATIONAL_SEMANTIC_DUMP_VARIABLE"))
    {
        char* end = nullptr;
        const unsigned long parsed = std::strtoul(value, &end, 10);
        if (!end || *end != '\0')
            throw std::runtime_error(
                "SVF_RELATIONAL_SEMANTIC_DUMP_VARIABLE must be a variable ID");
        const NodeID variableId = static_cast<NodeID>(parsed);
        const SVFVar* variable = analysis.getSVFVar(variableId);
        std::vector<const SVFStmt*> definitions(variable->getInEdges().begin(),
                                                variable->getInEdges().end());
        std::sort(definitions.begin(), definitions.end(),
                  [](const SVFStmt* lhs, const SVFStmt* rhs)
                  {
                      const NodeID lhsNode = lhs->getICFGNode()
                                                 ? lhs->getICFGNode()->getId()
                                                 : 0;
                      const NodeID rhsNode = rhs->getICFGNode()
                                                 ? rhs->getICFGNode()->getId()
                                                 : 0;
                      return std::make_tuple(lhsNode, lhs->getSrcID(),
                                             lhs->getDstID(),
                                             lhs->getEdgeKindWithoutMask()) <
                             std::make_tuple(rhsNode, rhs->getSrcID(),
                                             rhs->getDstID(),
                                             rhs->getEdgeKindWithoutMask());
                  });
        std::cout << "AE_NUMERICAL_VARIABLE id=" << variableId
                  << " value=" << variable->toString()
                  << " definitions=" << definitions.size() << '\n';
        for (const SVFStmt* definition : definitions)
            std::cout << "AE_NUMERICAL_VARIABLE_DEF id=" << variableId
                      << " icfg="
                      << (definition->getICFGNode()
                              ? definition->getICFGNode()->getId()
                              : 0)
                      << " value=" << definition->toString() << '\n';
    }
    std::vector<std::pair<NodeID, std::uint64_t>> states;
    states.reserve(analysis.getAnalyzedNodes().size());
    for (const ICFGNode* node : analysis.getAnalyzedNodes())
    {
        if (!analysis.hasAbsState(node))
            continue;
        const AD::AbstractState& abstractState =
            analysis.getAbstractState(node);
        if (!abstractState.isState<DenseState>())
            throw std::runtime_error(
                std::string("AE numerical checksum expected dense ") +
                domain + " product state");
        const auto& state = static_cast<const DenseState&>(abstractState);
        states.emplace_back(node->getId(), state.numerical().hash());
        if (dumpNode == node->getId())
            std::cout << "AE_NUMERICAL_STATE_DUMP domain=" << domain
                      << " node=" << node->getId() << " state="
                      << state.numerical().toString() << '\n';
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
            std::cout << "AE_NUMERICAL_STATE domain=" << domain
                      << " node=" << node << " hash=" << std::hex
                      << std::setfill('0') << std::setw(16) << state
                      << std::dec << '\n';
        append(node);
        append(state);
    }
    std::cout << "AE_NUMERICAL_CHECKSUM domain=" << domain
              << " states=" << states.size() << " checksum="
              << std::hex << std::setfill('0') << std::setw(16) << checksum
              << std::dec << '\n';
}

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

const DensePolyhedraState& requireDensePolyhedraState(
    AbstractInterpretation& analysis, const ICFGNode* node)
{
    const AD::AbstractState* scalarState =
        analysis.getScalarAbstractState(node->getFun());
    const AD::AbstractState& abstractState =
        scalarState ? *scalarState : analysis.getAbstractState(node);
    const auto* state = abstractState.isState<DensePolyhedraState>()
                            ? &static_cast<const DensePolyhedraState&>(
                                  abstractState)
                            : nullptr;
    if (!state)
        throw std::runtime_error(
            "native AE is not using DomainProductState<ConvexPolyhedraState>");
    return *state;
}

const DensePolyhedraState& requireScalarPolyhedraState(
    AbstractInterpretation& analysis, const ValVar* value,
    const ICFGNode* fallback)
{
    const AD::AbstractState* scalarState =
        analysis.getScalarAbstractState(value);
    if (!scalarState)
        return requireDensePolyhedraState(analysis, fallback);
    if (!scalarState->isState<DensePolyhedraState>())
        throw std::runtime_error(
            "sparse scalar checkpoint is not a Polyhedra product state");
    return static_cast<const DensePolyhedraState&>(*scalarState);
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

std::size_t percentile(std::vector<std::size_t> values, double fraction)
{
    if (values.empty())
        return 0;
    std::sort(values.begin(), values.end());
    const std::size_t index =
        static_cast<std::size_t>(
            std::ceil(fraction * static_cast<double>(values.size()))) -
        1;
    return values[std::min(index, values.size() - 1)];
}

class VariableComponents
{
public:
    void connect(const std::vector<AD::Variable>& variables)
    {
        if (variables.empty())
            return;
        const std::size_t first = ensure(variables.front());
        for (std::size_t index = 1; index < variables.size(); ++index)
            unite(first, ensure(variables[index]));
    }

    std::size_t largest() const
    {
        std::map<std::size_t, std::size_t> sizes;
        for (std::size_t index = 0; index < parent_.size(); ++index)
            ++sizes[root(index)];
        std::size_t result = 0;
        for (const auto& [representative, size] : sizes)
        {
            (void)representative;
            result = std::max(result, size);
        }
        return result;
    }

private:
    std::size_t ensure(AD::Variable variable)
    {
        const auto [iterator, inserted] =
            indices_.emplace(variable, parent_.size());
        if (inserted)
        {
            parent_.push_back(iterator->second);
            rank_.push_back(0);
        }
        return iterator->second;
    }

    std::size_t root(std::size_t index) const
    {
        while (parent_[index] != index)
            index = parent_[index];
        return index;
    }

    std::size_t mutableRoot(std::size_t index)
    {
        if (parent_[index] != index)
            parent_[index] = mutableRoot(parent_[index]);
        return parent_[index];
    }

    void unite(std::size_t lhs, std::size_t rhs)
    {
        lhs = mutableRoot(lhs);
        rhs = mutableRoot(rhs);
        if (lhs == rhs)
            return;
        if (rank_[lhs] < rank_[rhs])
            std::swap(lhs, rhs);
        parent_[rhs] = lhs;
        if (rank_[lhs] == rank_[rhs])
            ++rank_[lhs];
    }

    std::map<AD::Variable, std::size_t> indices_;
    std::vector<std::size_t> parent_;
    std::vector<unsigned> rank_;
};

void reportRelationalVocabularyAudit(SVFIR& graph, AndersenWaveDiff& ander)
{
    SVFIRAdapter adapter(graph);
    std::size_t objectDimensions = 0;
    std::size_t integerObjectDimensions = 0;
    std::size_t pointerObjectDimensions = 0;
    std::size_t otherObjectDimensions = 0;
    for (const AD::VariableDeclaration& declaration :
         adapter.environment().variables())
    {
        const ObjVar* object = adapter.contentObject(declaration.variable);
        if (!object)
            continue;
        ++objectDimensions;
        const BaseObjVar* base = graph.getBaseObject(object->getId());
        const SVFType* type = base->getType();
        if (type->getKind() == SVFType::SVFIntegerTy)
            ++integerObjectDimensions;
        else if (type->getKind() == SVFType::SVFPointerTy)
            ++pointerObjectDimensions;
        else
            ++otherObjectDimensions;
    }

    std::size_t numericScalarDimensions = 0;
    std::size_t pointerScalarDimensions = 0;
    for (const AD::VariableDeclaration& declaration :
         adapter.allScalarEnvironment().variables())
    {
        const ValVar* value = adapter.value(declaration.variable);
        if (value && value->isPointer())
            ++pointerScalarDimensions;
        else
            ++numericScalarDimensions;
    }

    Set<const FunObjVar*> functions;
    for (const auto& [nodeId, node] : *graph.getICFG())
    {
        (void)nodeId;
        if (node->getFun())
            functions.insert(node->getFun());
    }

    std::vector<std::size_t> fullEnvironmentSizes;
    std::vector<std::size_t> scalarEnvironmentSizes;
    for (const FunObjVar* function : functions)
    {
        if (function->isDeclaration())
            continue;
        fullEnvironmentSizes.push_back(adapter.environment(function).size());
        scalarEnvironmentSizes.push_back(
            adapter.scalarEnvironment(function).size());
    }

    std::map<NodeID, std::set<AD::Variable>> immediateObjectCache;
    auto objectVariable = [&](NodeID id) -> std::optional<AD::Variable> {
        const SVFVar* variable = graph.getGNode(id);
        const auto* object = SVFUtil::dyn_cast<ObjVar>(variable);
        if (!object || !adapter.contains(*object))
            return std::nullopt;
        return adapter.contentVariable(*object);
    };
    auto immediateObjects =
        [&](const ValVar* pointer) -> const std::set<AD::Variable>& {
        auto found = immediateObjectCache.find(pointer->getId());
        if (found != immediateObjectCache.end())
            return found->second;
        std::set<AD::Variable>& result = immediateObjectCache[pointer->getId()];
        for (NodeID id : ander.getPts(pointer->getId()))
        {
            if (const std::optional<AD::Variable> variable = objectVariable(id))
                result.insert(*variable);
        }
        return result;
    };
    auto numericVariable = [&](const SVFVar* raw,
                               std::vector<AD::Variable>& output) {
        const auto* value = SVFUtil::dyn_cast<ValVar>(raw);
        if (value && !value->isPointer() && adapter.contains(*value))
            output.push_back(adapter.variable(*value));
    };
    auto unique = [](std::vector<AD::Variable>& variables) {
        std::sort(variables.begin(), variables.end());
        variables.erase(std::unique(variables.begin(), variables.end()),
                        variables.end());
    };

    std::map<const FunObjVar*, VariableComponents> localComponents;
    std::vector<std::size_t> eventSupportSizes;
    std::vector<std::size_t> memoryAliasSizes;
    for (const auto& [nodeId, node] : *graph.getICFG())
    {
        (void)nodeId;
        const FunObjVar* function = node->getFun();
        for (const SVFStmt* statement : node->getSVFStmts())
        {
            std::vector<AD::Variable> support;
            if (const auto* load = SVFUtil::dyn_cast<LoadStmt>(statement))
            {
                numericVariable(load->getLHSVar(), support);
                const auto& aliases = immediateObjects(load->getRHSVar());
                support.insert(support.end(), aliases.begin(), aliases.end());
                memoryAliasSizes.push_back(aliases.size());
            }
            else if (const auto* store =
                         SVFUtil::dyn_cast<StoreStmt>(statement))
            {
                numericVariable(store->getRHSVar(), support);
                const auto& aliases = immediateObjects(store->getLHSVar());
                support.insert(support.end(), aliases.begin(), aliases.end());
                memoryAliasSizes.push_back(aliases.size());
            }
            else if (SVFUtil::isa<CallPE>(statement) ||
                     SVFUtil::isa<RetPE>(statement))
            {
                continue;
            }
            else if (const auto* multiple =
                         SVFUtil::dyn_cast<MultiOpndStmt>(statement))
            {
                numericVariable(multiple->getRes(), support);
                for (u32_t index = 0; index < multiple->getOpVarNum(); ++index)
                    numericVariable(multiple->getOpVar(index), support);
            }
            else if (const auto* assignment =
                         SVFUtil::dyn_cast<AssignStmt>(statement))
            {
                numericVariable(assignment->getLHSVar(), support);
                numericVariable(assignment->getRHSVar(), support);
            }
            unique(support);
            if (!support.empty())
            {
                eventSupportSizes.push_back(support.size());
                if (function)
                    localComponents[function].connect(support);
            }
        }
    }

    std::vector<std::size_t> callScalarArities;
    std::vector<std::size_t> callObjectFootprints;
    std::vector<std::size_t> callBoundarySides;
    std::size_t declarationOnlyCallSites = 0;
    CallGraph* callGraph = const_cast<CallGraph*>(graph.getCallGraph());
    for (const CallICFGNode* call : graph.getCallSiteSet())
    {
        std::vector<AD::Variable> actuals;
        std::set<AD::Variable> objects;
        for (const ValVar* actual : call->getActualParms())
        {
            numericVariable(actual, actuals);
            if (actual->isPointer())
            {
                const auto& direct = immediateObjects(actual);
                objects.insert(direct.begin(), direct.end());
            }
        }
        unique(actuals);
        callScalarArities.push_back(actuals.size());
        callObjectFootprints.push_back(objects.size());

        CallGraph::FunctionSet callees;
        callGraph->getCallees(call, callees);
        bool hasBody = false;
        std::size_t largestFormalSide = objects.size();
        for (const FunObjVar* callee : callees)
        {
            if (!callee->isDeclaration())
                hasBody = true;
            std::vector<AD::Variable> formals;
            if (graph.hasFunArgsList(callee))
            {
                for (const ValVar* formal : graph.getFunArgsList(callee))
                    numericVariable(formal, formals);
            }
            unique(formals);
            largestFormalSide =
                std::max(largestFormalSide, formals.size() + objects.size());
            if (!formals.empty())
                localComponents[callee].connect(formals);
            std::vector<AD::Variable> calleeBoundary = formals;
            calleeBoundary.insert(calleeBoundary.end(), objects.begin(),
                                  objects.end());
            localComponents[callee].connect(calleeBoundary);
        }
        if (!hasBody)
            ++declarationOnlyCallSites;

        std::vector<AD::Variable> callerBoundary = actuals;
        callerBoundary.insert(callerBoundary.end(), objects.begin(),
                              objects.end());
        if (call->getCaller())
            localComponents[call->getCaller()].connect(callerBoundary);
        callBoundarySides.push_back(
            std::max(actuals.size() + objects.size(), largestFormalSide));
    }

    std::vector<std::size_t> naivePackSizes;
    for (const FunObjVar* function : functions)
    {
        if (function->isDeclaration())
            continue;
        naivePackSizes.push_back(localComponents[function].largest());
    }

    std::cout << "RELATIONAL_VOCAB_AUDIT"
              << " functions=" << fullEnvironmentSizes.size()
              << " objects=" << objectDimensions
              << " integer_objects=" << integerObjectDimensions
              << " pointer_objects=" << pointerObjectDimensions
              << " other_objects=" << otherObjectDimensions
              << " numeric_scalars=" << numericScalarDimensions
              << " pointer_scalars=" << pointerScalarDimensions
              << " env_p50=" << percentile(fullEnvironmentSizes, 0.50)
              << " env_p95=" << percentile(fullEnvironmentSizes, 0.95)
              << " env_max=" << percentile(fullEnvironmentSizes, 1.00)
              << " scalar_env_p95=" << percentile(scalarEnvironmentSizes, 0.95)
              << " scalar_env_max=" << percentile(scalarEnvironmentSizes, 1.00)
              << " event_support_p95=" << percentile(eventSupportSizes, 0.95)
              << " event_support_max=" << percentile(eventSupportSizes, 1.00)
              << " memory_alias_p95=" << percentile(memoryAliasSizes, 0.95)
              << " memory_alias_max=" << percentile(memoryAliasSizes, 1.00)
              << " call_scalar_p95=" << percentile(callScalarArities, 0.95)
              << " call_scalar_max=" << percentile(callScalarArities, 1.00)
              << " call_direct_objects_p95="
              << percentile(callObjectFootprints, 0.95)
              << " call_direct_objects_max="
              << percentile(callObjectFootprints, 1.00)
              << " call_side_p95=" << percentile(callBoundarySides, 0.95)
              << " call_side_max=" << percentile(callBoundarySides, 1.00)
              << " naive_pack_p95=" << percentile(naivePackSizes, 0.95)
              << " naive_pack_max=" << percentile(naivePackSizes, 1.00)
              << " declaration_only_calls=" << declarationOnlyCallSites << '\n';
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

void validatePolyhedraReducedProductFixture(
    const SVFIR& graph, AbstractInterpretation& analysis)
{
    const auto* input = SVFUtil::cast<ValVar>(requireValue(graph, "x"));
    const auto* affine = SVFUtil::cast<ValVar>(requireValue(graph, "y"));
    const auto* result = SVFUtil::cast<ValVar>(requireValue(graph, "z"));
    const SVFVar* impossible = requireValue(graph, "bad");
    SVFIRAdapter adapter(graph);
    bool sawInterval = false;
    bool sawRelation = false;
    bool impossibleReachable = false;
    for (const ICFGNode* node : analysis.getAnalyzedNodes())
    {
        impossibleReachable |= analysis.hasAbsValue(impossible, node);
        if (!analysis.hasAbsValue(result, node))
            continue;
        const AbstractValue& value = analysis.getAbsValue(result, node);
        sawInterval |= value.isInterval() &&
            value.getInterval().equals(
                IntervalValue(static_cast<s64_t>(1),
                              static_cast<s64_t>(6)));
        const AD::ConvexPolyhedraState& polyhedron =
            requireScalarPolyhedraState(analysis, result, node).numerical();
        if (polyhedron.environment().contains(adapter.variable(*input)) &&
            polyhedron.environment().contains(adapter.variable(*affine)) &&
            polyhedron.entails(AD::equal(
                AD::LinearExpression(adapter.variable(*input)),
                AD::LinearExpression(adapter.variable(*affine)))) ==
                AD::CheckResult::True)
            sawRelation = true;
    }
    if (!sawInterval || !sawRelation || impossibleReachable)
        throw std::runtime_error(
            "Polyhedra AE did not preserve reduced-product fixture semantics");
    std::cout
        << "AE_POLYHEDRA_OBSERVATION fixture=reduced-product"
        << " reduced_interval=1 reduced_relation=1 impossible_reachable=0\n";
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

void auditInterproceduralRelationalFixture(const SVFIR& graph,
                                           AbstractInterpretation& analysis)
{
    const auto* actualX =
        SVFUtil::cast<ValVar>(requireValue(graph, "actual_x"));
    const auto* actualY =
        SVFUtil::cast<ValVar>(requireValue(graph, "actual_y"));
    const auto* difference =
        SVFUtil::cast<ValVar>(requireValue(graph, "formal_difference"));
    const SVFVar* failure = requireValue(graph, "interproc_bad");
    SVFIRAdapter adapter(graph);
    const AD::LinearConstraint callerEquality =
        AD::equal(AD::LinearExpression(adapter.variable(*actualX)),
                  AD::LinearExpression(adapter.variable(*actualY)));
    bool callerRelation = false;
    bool calleeDifferenceZero = false;
    bool failureReachable = false;
    for (const ICFGNode* node : analysis.getAnalyzedNodes())
    {
        if (analysis.hasAbsValue(failure, node))
            failureReachable = true;
        if (analysis.hasAbsValue(actualY, node))
        {
            const AD::OctagonState& state =
                requireScalarOctagonState(analysis, actualY, node).numerical();
            if (state.environment().contains(adapter.variable(*actualX)) &&
                state.environment().contains(adapter.variable(*actualY)) &&
                state.entails(callerEquality) == AD::CheckResult::True)
                callerRelation = true;
        }
        if (analysis.hasAbsValue(difference, node))
        {
            const AD::OctagonState& state =
                requireScalarOctagonState(analysis, difference, node)
                    .numerical();
            if (state.environment().contains(adapter.variable(*difference)) &&
                hasIntegerBounds(state.bound(adapter.variable(*difference)), 0,
                                 0))
                calleeDifferenceZero = true;
        }
    }
    std::cout << "AE_INTERPROCEDURAL_RELATIONAL_AUDIT"
              << " caller_relation=" << callerRelation
              << " callee_difference_zero=" << calleeDifferenceZero
              << " failure_reachable=" << failureReachable << '\n';
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
        const char* selectionOnly =
            std::getenv("SVF_RELATIONAL_SELECTION_ONLY");
        if (selectionOnly == nullptr || selectionOnly[0] == '\0' ||
            std::string(selectionOnly) == "0")
            selectionOnly = std::getenv("SVF_OCTAGON_SELECTION_ONLY");
        if (selectionOnly != nullptr && selectionOnly[0] != '\0' &&
            std::string(selectionOnly) != "0")
        {
            if (const char* vocabularyAudit =
                    std::getenv("SVF_RELATIONAL_VOCABULARY_AUDIT");
                vocabularyAudit != nullptr && vocabularyAudit[0] != '\0' &&
                std::string(vocabularyAudit) != "0")
                reportRelationalVocabularyAudit(*graph, *ander);
            AndersenWaveDiff::releaseAndersenWaveDiff();
            LLVMModuleSet::releaseLLVMModuleSet();
            std::cout << "AE relational selection probe: PASS\n";
            return EXIT_SUCCESS;
        }
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
        else if (Options::AEDensePolyhedra())
        {
            validateNativeProductStorage<DensePolyhedraState>(analysis);
            if (Options::AESparsity() != AbstractInterpretation::Dense)
                validateNativeSparseCarrierSeparation<DensePolyhedraState>(
                    *graph, analysis);
            if (findValueIfPresent(*graph, "z"))
                validatePolyhedraReducedProductFixture(*graph, analysis);
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
        else if (findValueIfPresent(*graph, "interproc_bad"))
        {
            validateNativeProductStorage<DenseOctagonState>(analysis);
            auditInterproceduralRelationalFixture(*graph, analysis);
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
        // Arbitrary benchmark modules do not carry the named fixture markers
        // above.  Their authoritative ICFG state may intentionally be the
        // memory carrier after scalar/memory separation, so imposing a
        // fixture-specific DenseOctagonState type assertion here rejects a
        // successful production run.  The generic observation below is the
        // contract for those inputs.

        if (findValueIfPresent(*graph, "memory_result"))
            validateSparseMemoryFixture(*graph, analysis, nativeProduct);
        const char* checksumRequested =
            std::getenv("SVF_RELATIONAL_SEMANTIC_CHECKSUM");
        if (checksumRequested != nullptr && checksumRequested[0] != '\0' &&
            std::string(checksumRequested) != "0" &&
            Options::AESparsity() == AbstractInterpretation::Dense &&
            !Options::AEDenseLegacyInterval())
        {
            if (Options::AEDensePolyhedra())
                reportNumericalSemanticChecksum<DensePolyhedraState>(
                    "polyhedra", analysis);
            else if (Options::AEDenseOctagon())
                reportNumericalSemanticChecksum<DenseOctagonState>(
                    "octagon", analysis);
            else
                reportNumericalSemanticChecksum<DenseBoxState>("box",
                                                                analysis);
        }
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
