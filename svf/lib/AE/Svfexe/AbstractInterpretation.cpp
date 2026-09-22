//===- AbstractExecution.cpp -- Abstract
// Execution---------------------------------//
//
//                     SVF: Static Value-Flow Analysis
//
// Copyright (C) <2013->  <Yulei Sui>
//

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Affero General Public License for more details.

// You should have received a copy of the GNU Affero General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
//===----------------------------------------------------------------------===//

//
//  Created on: Jan 10, 2024
//      Author: Xiao Cheng, Jiawei Wang
//

#include "AE/Svfexe/AbstractInterpretation.h"
#include "AE/Svfexe/AbsExtAPI.h"
#include "AE/Svfexe/SparseAbstractInterpretation.h"
#include "AE/Core/PartialRelationalDomain.h"
#include "Graphs/CallGraph.h"
#include "SVFIR/SVFIR.h"
#include "Util/Options.h"
#include "Util/WorkList.h"
#include "WPA/Andersen.h"
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <iomanip>
#include <memory>
#include <set>
#include <stdexcept>
#include <tuple>
#include <unordered_set>

using namespace SVF;
using namespace SVFUtil;
namespace AD = SVF::AbstractDomain;

static std::vector<const ICFGEdge*> orderedIncomingEdges(const ICFGNode* node)
{
    std::vector<const ICFGEdge*> edges(node->getInEdges().begin(),
                                       node->getInEdges().end());
    std::sort(edges.begin(), edges.end(),
              [](const ICFGEdge* lhs, const ICFGEdge* rhs)
    {
        return std::make_tuple(lhs->getSrcID(),
                               lhs->getEdgeKindWithoutMask()) <
               std::make_tuple(rhs->getSrcID(),
                               rhs->getEdgeKindWithoutMask());
    });
    return edges;
}

static const char* queryDetectorName(AEDetector::DetectorKind detector)
{
    switch (detector)
    {
    case AEDetector::BUF_OVERFLOW:
        return "buffer-overflow";
    case AEDetector::NULL_DEREF:
        return "null-dereference";
    case AEDetector::UNKNOWN:
    default:
        return "unknown";
    }
}

static const char* queryOutcomeName(AbstractInterpretation::QueryOutcome outcome)
{
    switch (outcome)
    {
    case AbstractInterpretation::QueryOutcome::Safe:
        return "Safe";
    case AbstractInterpretation::QueryOutcome::May:
        return "May";
    case AbstractInterpretation::QueryOutcome::Unsupported:
        return "Unsupported";
    case AbstractInterpretation::QueryOutcome::Unreachable:
    default:
        return "Unreachable";
    }
}

static unsigned queryOutcomeRank(AbstractInterpretation::QueryOutcome outcome)
{
    switch (outcome)
    {
    case AbstractInterpretation::QueryOutcome::Safe:
        return 1;
    case AbstractInterpretation::QueryOutcome::Unsupported:
        return 2;
    case AbstractInterpretation::QueryOutcome::May:
        return 3;
    case AbstractInterpretation::QueryOutcome::Unreachable:
    default:
        return 0;
    }
}

static std::vector<const ValVar*> definitionOperands(const SVFStmt* statement)
{
    std::vector<const ValVar*> result;
    const auto add = [&](const SVFVar* variable) {
        if (const auto* value = SVFUtil::dyn_cast<ValVar>(variable))
            result.push_back(value);
    };
    // Loads are an explicit memory boundary for the initial static slice. The
    // loaded result can still be selected, but its relational payload is
    // reconstructed from the global Box hull rather than following a pointer.
    if (SVFUtil::isa<LoadStmt>(statement))
        return result;
    if (const auto* multi = SVFUtil::dyn_cast<MultiOpndStmt>(statement))
    {
        for (const ValVar* operand : multi->getOpndVars())
            add(operand);
    }
    else if (const auto* unary = SVFUtil::dyn_cast<UnaryOPStmt>(statement))
        add(unary->getOpVar());
    else if (const auto* assignment =
                 SVFUtil::dyn_cast<AssignStmt>(statement))
        add(assignment->getRHSVar());
    if (const auto* address = SVFUtil::dyn_cast<AddrStmt>(statement))
        for (const SVFVar* size : address->getArrSize())
            add(size);
    std::sort(result.begin(), result.end(),
              [](const ValVar* left, const ValVar* right) {
                  return left->getId() < right->getId();
              });
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

static std::string queryKey(AEDetector::DetectorKind detector,
                            const ICFGNode* node, const SVFVar* operand,
                            const std::string& queryKind)
{
    return std::to_string(static_cast<unsigned>(detector)) + ':' +
           std::to_string(node ? node->getId() : 0) + ':' +
           std::to_string(operand ? operand->getId() : 0) + ':' + queryKind;
}

static std::string escapeQueryField(const std::string& field)
{
    std::string result;
    result.reserve(field.size());
    for (char character : field)
    {
        switch (character)
        {
        case '\t':
            result += "\\t";
            break;
        case '\n':
            result += "\\n";
            break;
        case '\r':
            result += "\\r";
            break;
        default:
            result += character;
            break;
        }
    }
    return result;
}

AD::AddressSet AbstractInterpretation::blackHoleAddressSet() const
{
    const auto* object = SVFUtil::dyn_cast<ObjVar>(
                             svfir->getGNode(IRGraph::BlackHole));
    if (!object)
        throw std::runtime_error("SVFIR has no BlackHole object");
    return AD::AddressSet::singleton(adapter_.location(*object));
}

void AbstractInterpretation::handleGlobalNode()
{
    const ICFGNode* node = icfg->getGlobalICFGNode();
    stateTrace_.insert_or_assign(node, topState());
    for (const SVFStmt* statement : node->getSVFStmts())
        handleSVFStatement(statement);

    if (const auto* variable = SVFUtil::dyn_cast<ValVar>(
                                   svfir->getGNode(PAG::getPAG()->getBlkPtr())))
        updateValue(variable, AD::Interval::top(),
                    blackHoleAddressSet(), node);
}

void AbstractInterpretation::initializeObjectValue(
    const ObjVar* object, AD::Interval& interval, AD::AddressSet& addresses,
    const ICFGNode* node)
{
    interval = AD::Interval::bottom();
    addresses = AD::AddressSet::bottom();
    State& denseState = ensureState(node);
    denseState.allocate(adapter_.location(*object));

    const BaseObjVar* base = PAG::getPAG()->getBaseObject(object->getId());
    if (base->isStack())
    {
        // Fresh payload facets are uninitialized, independently of their
        // physical Box/Address slots (whose missing payload still means Top).
        // A heap allocation site can also represent earlier live allocations;
        // revisiting it must not erase those objects' summarized contents.
        denseState.resetValue(adapter_.contentVariable(*object));
        for (NodeID fieldId : svfir->getAllFieldsObjVars(base->getId()))
        {
            const auto* field = SVFUtil::dyn_cast<ObjVar>(svfir->getGNode(fieldId));
            if (field)
                denseState.resetValue(adapter_.contentVariable(*field));
        }
    }
    if (base->isBlackHoleObj())
    {
        addresses = blackHoleAddressSet();
        return;
    }
    if (base->isConstDataOrConstGlobal() || base->isConstantArray() ||
            base->isConstantStruct())
    {
        if (const auto* integer = SVFUtil::dyn_cast<ConstIntObjVar>(object))
            interval =
                AD::Interval::singleton(AD::Rational(integer->getSExtValue()));
        else if (const auto* floating =
                     SVFUtil::dyn_cast<ConstFPObjVar>(object))
            interval = SVFIRAdapter::floatingConstant(floating->getFPValue());
        else if (SVFUtil::isa<ConstNullPtrObjVar>(object))
            addresses = AD::AddressSet::singleton(AD::Location::null());
        else if (!SVFUtil::isa<GlobalObjVar>(object))
            interval = AD::Interval::top();
        if (!interval.isBottom() || !addresses.isBottom())
            return;
    }
    addresses = AD::AddressSet::singleton(adapter_.location(*object));
}

void AbstractInterpretation::initializeRelationalPolicy()
{
    if (Options::AERelationalPolicy() != QuerySliceRelational)
        return;
    if (Options::AEDomain() == AENumericalDomain::Box)
        throw std::invalid_argument(
            "-ae-relational-policy=query-slice requires octagon or polyhedra");

    std::vector<const ValVar*> seeds;
    const auto addSeed = [&](const SVFVar* variable) {
        if (const auto* value = SVFUtil::dyn_cast<ValVar>(variable))
            if (adapter_.contains(*value))
                seeds.push_back(value);
    };
    for (auto iterator = icfg->begin(); iterator != icfg->end(); ++iterator)
    {
        const ICFGNode* node = iterator->second;
        if (const auto* call = SVFUtil::dyn_cast<CallICFGNode>(node))
        {
            const FunObjVar* function = call->getCalledFunction();
            if (function &&
                    (function->getName() == "SAFE_BUFACCESS" ||
                     function->getName() == "UNSAFE_BUFACCESS") &&
                    call->arg_size() >= 2)
                addSeed(call->getArgument(1));
            if (function && SVFUtil::isExtCall(function) &&
                    call->arg_size() != 0)
            {
                bool sizedMemoryOperation = false;
                for (const std::string& annotation :
                        ExtAPI::getExtAPI()->getExtFuncAnnotations(function))
                    sizedMemoryOperation |=
                        annotation.find("MEMCPY") != std::string::npos ||
                        annotation.find("MEMSET") != std::string::npos;
                if (sizedMemoryOperation)
                    addSeed(call->getArgument(call->arg_size() - 1));
            }
        }
        for (const SVFStmt* statement : node->getSVFStmts())
        {
            if (const auto* gep = SVFUtil::dyn_cast<GepStmt>(statement))
                for (const auto& [index, type] :
                        gep->getOffsetVarAndGepTypePairVec())
                {
                    (void)type;
                    addSeed(index);
                }
            if (const auto* address = SVFUtil::dyn_cast<AddrStmt>(statement))
                for (const SVFVar* size : address->getArrSize())
                    addSeed(size);
        }
    }

    const auto byVariable = [&](const ValVar* left, const ValVar* right) {
        return adapter_.variable(*left) < adapter_.variable(*right);
    };
    std::sort(seeds.begin(), seeds.end(), byVariable);
    seeds.erase(std::unique(seeds.begin(), seeds.end()), seeds.end());
    relationalSeedCount_ = seeds.size();

    std::deque<const ValVar*> worklist(seeds.begin(), seeds.end());
    std::unordered_set<NodeID> seen;
    std::vector<AD::Variable> closure;
    while (!worklist.empty())
    {
        const ValVar* value = worklist.front();
        worklist.pop_front();
        if (!seen.insert(value->getId()).second)
            continue;
        if (adapter_.contains(*value))
            closure.push_back(adapter_.variable(*value));

        std::vector<const SVFStmt*> definitions(
            value->getInEdges().begin(), value->getInEdges().end());
        std::sort(definitions.begin(), definitions.end(),
                  [](const SVFStmt* left, const SVFStmt* right) {
                      return left->getEdgeID() < right->getEdgeID();
                  });
        std::vector<const ValVar*> operands;
        for (const SVFStmt* definition : definitions)
        {
            std::vector<const ValVar*> current =
                definitionOperands(definition);
            operands.insert(operands.end(), current.begin(), current.end());
        }
        std::sort(operands.begin(), operands.end(), byVariable);
        operands.erase(std::unique(operands.begin(), operands.end()),
                       operands.end());
        for (const ValVar* operand : operands)
            if (seen.count(operand->getId()) == 0)
                worklist.push_back(operand);
    }

    // BFS order gives query seeds priority, then progressively older
    // definitions. Canonicalize only after applying the explicit policy cap.
    const std::size_t limit = Options::AERelationalMaxVars();
    relationalDroppedCount_ = closure.size() > limit
                              ? closure.size() - limit : 0;
    if (closure.size() > limit)
        closure.resize(limit);
    std::sort(closure.begin(), closure.end());
    closure.erase(std::unique(closure.begin(), closure.end()), closure.end());
    relationalVocabulary_ =
        std::make_shared<const std::vector<AD::Variable>>(std::move(closure));

    SVFUtil::outs()
            << "AE_RELATIONAL_POLICY policy=query-slice seeds="
            << relationalSeedCount_
            << " selected=" << relationalVocabulary_->size()
            << " dropped=" << relationalDroppedCount_
            << " max=" << Options::AERelationalMaxVars() << '\n';
}


void AbstractInterpretation::runOnModule()
{
    using PhaseClock = std::chrono::steady_clock;
    const auto secondsSince = [](PhaseClock::time_point start) {
        return std::chrono::duration<double>(PhaseClock::now() - start).count();
    };
    const bool phaseStats = std::getenv("SVF_AE_PHASE_STATS") != nullptr;
    const bool domainStats = std::getenv("SVF_AE_DOMAIN_STATS") != nullptr;
    double querySeconds = 0.0;

    stat->startClk();
    utils = new AbsExtAPI(this);
    const PhaseClock::time_point queryEnumerationStart = PhaseClock::now();
    enumerateQueries();
    querySeconds += secondsSince(queryEnumerationStart);
    /// collect checkpoint
    utils->collectCheckPoint();

    initializeRelationalPolicy();

    if (domainStats)
        AD::NumericalDomain::beginTelemetry();
    const PhaseClock::time_point solveStart = PhaseClock::now();
    analyse();
    const double solveSeconds = secondsSince(solveStart);
    if (domainStats)
    {
        const AD::NumericalTelemetry operations =
            AD::NumericalDomain::endTelemetry();
        std::size_t activeProperties = 0;
        std::size_t totalSupport = 0;
        std::size_t maximumSupport = 0;
        std::size_t components = 0;
        std::size_t maximumComponent = 0;
        const auto measure = [&](const State& property) {
            const AD::NumericalDomain& numerical = property.numerical();
            if (numerical.isBottom())
                return;
            ++activeProperties;
            const std::vector<AD::Variable> support =
                numerical.supportVariables();
            totalSupport += support.size();
            maximumSupport = std::max(maximumSupport, support.size());
            if (numerical.isDomain<AD::BoxDomain>())
            {
                components += support.size();
                if (!support.empty())
                    maximumComponent = std::max<std::size_t>(
                                           maximumComponent, 1);
                return;
            }
            std::set<AD::Variable> remaining(support.begin(), support.end());
            while (!remaining.empty())
            {
                const std::vector<AD::Variable> component =
                    numerical.relationalClosure({*remaining.begin()});
                ++components;
                maximumComponent = std::max(maximumComponent,
                                             component.size());
                for (AD::Variable variable : component)
                    remaining.erase(variable);
            }
        };
        for (const auto& [node, property] : stateTrace_)
        {
            (void)node;
            measure(property);
        }
        if (const AD::AbstractDomain* carrier = getScalarAbstractState())
            measure(static_cast<const State&>(*carrier));
        SVFUtil::outs()
                << "AE_DOMAIN_STATS flow_states=" << stateTrace_.size()
                << " active_properties=" << activeProperties
                << " total_support=" << totalSupport
                << " max_support=" << maximumSupport
                << " components=" << components
                << " max_component=" << maximumComponent
                << " closure_calls=" << operations.relationalClosureCalls
                << " join_calls=" << operations.joinCalls
                << " widen_calls=" << operations.wideningCalls
                << " narrow_calls=" << operations.narrowingCalls << '\n';
        if (Options::AERelationalPolicy() == QuerySliceRelational)
            SVFUtil::outs()
                    << "AE_PARTIAL_RELATIONAL_STATS inside_ops="
                    << operations.partialInsideOperations
                    << " fallback_ops="
                    << operations.partialFallbackOperations
                    << " projections=" << operations.partialProjections
                    << '\n';
    }
    if (unknownTargetTelemetryEnabled_)
    {
        const UnknownTargetTelemetry& telemetry = unknownTargetTelemetry_;
        SVFUtil::outs()
                << "AE_UNKNOWN_TARGET_STATS loads=" << telemetry.loads
                << " stores=" << telemetry.stores
                << " store_cells_visited=" << telemetry.storeCellsVisited
                << " sparse_definition_cells_visited="
                << telemetry.sparseDefinitionCellsVisited << '\n';
    }
    utils->checkPointAllSet();
    stat->endClk();
    stat->finializeStat();
    if (Options::PStat())
        stat->performStat();
    const PhaseClock::time_point queryReportStart = PhaseClock::now();
    for (auto& detector : detectors)
        detector->reportBug();
    writeQueryLedger();
    querySeconds += secondsSince(queryReportStart);
    const PhaseClock::time_point postStart = PhaseClock::now();
    verifyPostFixpoint();
    const double postSeconds = secondsSince(postStart);
    if (phaseStats)
        SVFUtil::outs() << std::defaultfloat << std::setprecision(9)
                        << "AE_PHASE_TIMES ai_s=" << solveSeconds
                        << " query_s=" << querySeconds
                        << " post_s=" << postSeconds << '\n';
}

bool AbstractInterpretation::queryLedgerEnabled() const
{
    return !Options::AEQueryLedgerFile().empty();
}

void AbstractInterpretation::enumerateQueries()
{
    if (!queryLedgerEnabled())
        return;
    if (Options::AEQueryInputID().empty())
        throw std::invalid_argument(
            "-ae-query-input-id is required with -ae-query-ledger");
    for (auto& detector : detectors)
        detector->enumerateQueries();
}

void AbstractInterpretation::registerQuery(
    AEDetector::DetectorKind detector, const ICFGNode* node,
    const SVFVar* operand, const std::string& queryKind)
{
    if (!queryLedgerEnabled())
        return;
    if (!node)
        throw std::invalid_argument("AE query has no ICFG node");

    QueryRecord record;
    record.detector = detector;
    record.icfgNode = node->getId();
    record.operand = operand ? operand->getId() : 0;
    record.queryKind = queryKind;
    record.function = node->getFun() ? node->getFun()->getName() : "<global>";
    record.sourceLocation = node->getSourceLoc();
    queryLedger_.emplace(queryKey(detector, node, operand, queryKind),
                         std::move(record));
}

void AbstractInterpretation::recordQuery(
    AEDetector::DetectorKind detector, const ICFGNode* node,
    const SVFVar* operand, const std::string& queryKind, QueryOutcome outcome,
    const std::string& reason)
{
    if (!queryLedgerEnabled())
        return;
    registerQuery(detector, node, operand, queryKind);
    QueryRecord& record = queryLedger_.at(
                              queryKey(detector, node, operand, queryKind));
    if (queryOutcomeRank(outcome) >= queryOutcomeRank(record.outcome))
    {
        record.outcome = outcome;
        record.reason = reason;
    }
}

void AbstractInterpretation::writeQueryLedger() const
{
    if (!queryLedgerEnabled())
        return;
    std::ofstream output(Options::AEQueryLedgerFile(),
                         std::ios::out | std::ios::trunc);
    if (!output)
        throw std::runtime_error("cannot open AE query ledger: " +
                                 Options::AEQueryLedgerFile());

    output << "query_id\tinput_id\tdetector\tfunction\tsource_location\t"
              "icfg_node\toperand\tquery_kind\toutcome\treason\n";
    const std::string input = escapeQueryField(Options::AEQueryInputID());
    std::vector<const QueryRecord*> records;
    records.reserve(queryLedger_.size());
    for (const auto& [key, record] : queryLedger_)
    {
        (void)key;
        records.push_back(&record);
    }
    // SVF NodeIDs are allocation-order diagnostics, not stable program
    // identities: parsing the same module in two processes can shift all IDs
    // in one external function.  Order equal semantic sites by their local
    // ICFG order, then name them by a source/function group occurrence.  The
    // raw IDs remain in dedicated diagnostic columns.
    std::sort(records.begin(), records.end(),
              [](const QueryRecord* left, const QueryRecord* right) {
                  return std::make_tuple(
                             left->detector, left->function,
                             left->sourceLocation, left->queryKind,
                             left->icfgNode, left->operand) <
                         std::make_tuple(
                             right->detector, right->function,
                             right->sourceLocation, right->queryKind,
                             right->icfgNode, right->operand);
              });
    std::tuple<AEDetector::DetectorKind, std::string, std::string,
               std::string> previousGroup;
    bool first = true;
    std::size_t occurrence = 0;
    for (const QueryRecord* recordPointer : records)
    {
        const QueryRecord& record = *recordPointer;
        const auto group = std::make_tuple(record.detector, record.function,
                                           record.sourceLocation,
                                           record.queryKind);
        if (first || group != previousGroup)
            occurrence = 0;
        else
            ++occurrence;
        previousGroup = group;
        first = false;
        const std::string detector = queryDetectorName(record.detector);
        const std::string identity = input + ':' + detector + ':' +
            record.function + ':' + record.sourceLocation + ':' +
            record.queryKind + ':' + std::to_string(occurrence);
        output << escapeQueryField(identity) << '\t' << input << '\t'
               << detector << '\t' << escapeQueryField(record.function)
               << '\t' << escapeQueryField(record.sourceLocation) << '\t'
               << record.icfgNode << '\t' << record.operand << '\t'
               << escapeQueryField(record.queryKind) << '\t'
               << queryOutcomeName(record.outcome) << '\t'
               << escapeQueryField(
                      record.outcome == QueryOutcome::Unreachable &&
                              record.reason.empty()
                      ? "not reached from configured analysis entries"
                      : record.reason) << '\n';
    }
    if (!output)
        throw std::runtime_error("failed to write AE query ledger: " +
                                 Options::AEQueryLedgerFile());
}

AbstractInterpretation::AbstractInterpretation()
    : adapter_(*PAG::getPAG()),
      unknownTargetTelemetryEnabled_(
          std::getenv("SVF_AE_UNKNOWN_TARGET_STATS") != nullptr)
{
    stat = new AEStat(this);
    // Run Andersen's pointer analysis and build WTO
    svfir = PAG::getPAG();
    icfg = svfir->getICFG();
    preAnalysis = new AEWTO(svfir, icfg);
    callGraph = preAnalysis->getCallGraph();
    icfg->updateCallGraph(callGraph);
    preAnalysis->initWTO();
}

/// Factory: first call allocates the concrete subclass based on
/// Options::AESparsity(); all subsequent calls return the same instance.
/// Must only be called after the option parser has populated AESparsity.
AbstractInterpretation& AbstractInterpretation::getAEInstance()
{
    // Keep the singleton alive until process exit. Several owned analysis
    // objects refer to process-global SVF state whose destruction order is
    // outside AE's control.
    static AbstractInterpretation* instance = []() -> AbstractInterpretation*
    {
        if (Options::AESparsity() == AESparsity::Sparse &&
                Options::AEDomain() != AENumericalDomain::Box)
            throw std::invalid_argument(
                "relational numerical domains currently support dense and "
                "semi-sparse AE only");
        if (Options::AERelationalPolicy() == QuerySliceRelational &&
                Options::AEDomain() == AENumericalDomain::Box)
            throw std::invalid_argument(
                "query-slice relational policy requires octagon or polyhedra");
        switch (Options::AESparsity())
        {
        case AESparsity::SemiSparse:
            return new SemiSparseAbstractInterpretation();
        case AESparsity::Sparse:
            return new FullSparseAbstractInterpretation();
        case AESparsity::Dense:
        default:
            return new AbstractInterpretation();
        }
    }();
    return *instance;
}

/// Destructor
AbstractInterpretation::~AbstractInterpretation()
{
    delete utils;
    delete stat;
    delete preAnalysis;
}

/// Collect entry point functions for analysis.
/// In main mode, entry is main/svf.main. In no-main mode,
/// entries are SCCs with no external caller in the Andersen-resolved CallGraph.
FIFOWorkList<const FunObjVar*> AbstractInterpretation::collectProgEntryFuns()
{
    FIFOWorkList<const FunObjVar*> entryFunctions;
    const bool mainEntry = Options::AEFunEntry() == AEFunEntryMode::MAIN;
    Set<NodeID> visitedEntrySCCs;
    auto* callGraphSCC = preAnalysis->getCallGraphSCC();

    for (auto it = callGraph->begin(); it != callGraph->end(); ++it)
    {
        const CallGraphNode* cgNode = it->second;
        const FunObjVar* fun = cgNode->getFunction();

        // Skip declarations
        if (fun->isDeclaration())
            continue;

        if (mainEntry)
        {
            if (SVFUtil::isProgEntryFunction(fun))
            {
                entryFunctions.push(fun);
                break;
            }
        }
        else
        {
            NodeID repNodeId = callGraphSCC->repNode(cgNode->getId());
            if (visitedEntrySCCs.count(repNodeId))
                continue;

            const NodeBS& cgSCCNodes = callGraphSCC->subNodes(repNodeId);
            bool hasExternalCaller = false;
            for (NodeID nodeId : cgSCCNodes)
            {
                const CallGraphNode* sccNode = callGraph->getGNode(nodeId);
                for (auto inEdge : sccNode->getInEdges())
                {
                    if (!cgSCCNodes.test(inEdge->getSrcID()))
                    {
                        hasExternalCaller = true;
                        break;
                    }
                }
                if (hasExternalCaller)
                    break;
            }

            if (hasExternalCaller)
                continue;

            visitedEntrySCCs.insert(repNodeId);
            const FunObjVar* entryFun = fun;
            for (NodeID nodeId : cgSCCNodes)
            {
                const FunObjVar* sccFun =
                    callGraph->getGNode(nodeId)->getFunction();
                if (SVFUtil::isProgEntryFunction(sccFun))
                {
                    entryFun = sccFun;
                    break;
                }
            }
            entryFunctions.push(entryFun);
        }
    }

    if (mainEntry && entryFunctions.empty())
    {
        SVFUtil::errs() << SVFUtil::errMsg(
                            "AE -ae-fun-entry=main requires a program entry function, but "
                            "main/svf.main was not found.\n");
        assert(false &&
               "No program entry function found for -ae-fun-entry=main");
        abort();
    }

    return entryFunctions;
}

/// Program entry - entry policy is selected by -ae-fun-entry.
void AbstractInterpretation::analyse()
{
    analyzeFromAllProgEntries();
}

/// Analyze the entry functions selected by collectProgEntryFuns().
/// Abstract state is shared across entry points so that functions analyzed from
/// earlier entries are not re-analyzed from scratch.
void AbstractInterpretation::analyzeFromAllProgEntries()
{
    // Collect all entry point functions
    FIFOWorkList<const FunObjVar*> entryFunctions = collectProgEntryFuns();

    if (entryFunctions.empty())
    {
        assert(false && "No entry functions found for analysis");
        return;
    }
    // handle Global ICFGNode of SVFModule
    handleGlobalNode();
    const ICFGNode* globalNode = icfg->getGlobalICFGNode();
    while (!entryFunctions.empty())
    {
        const FunObjVar* entryFun = entryFunctions.pop();
        const FunEntryICFGNode* funEntry = icfg->getFunEntryICFGNode(entryFun);
        // Selected roots have no caller transfer to define their arguments.
        // Inputs carry numerical Top, unlike uninitialized object contents.
        // Pointer inputs still have no modeled address targets, but their
        // numerical facet is preserved if subsequently written to memory.
        for (const SVFVar* argument : funEntry->getFormalParms())
        {
            updateInterval(argument, AD::Interval::top(), globalNode);
        }
        copyAbstractState(globalNode, funEntry);
        // A later call site can enlarge a context-insensitive callee summary.
        // Revisit the root until every caller and its downstream states cover
        // that enlarged summary.  Loop/recursion WTO handling remains
        // responsible for convergence; an arbitrary iteration cap would not
        // establish a post-fixpoint.
        while (handleFunction(funEntry, nullptr))
        {
        }
    }
}

/// Given a cmp operand, walk its SSA def edge to find the LoadStmt that
/// produced it. This lets us trace back to the ObjVar in memory so that
/// branch narrowing can refine the stored value.
///
/// Example: for `%cmp = icmp sgt %a, 5` where `%a = load i32, ptr %p`,
/// calling findBackingLoad(%a) returns the LoadStmt, and we can then
/// narrow the ObjVar behind %p.
///
/// Follows one level of CopyStmt (e.g., zext/sext) if the load is not
/// directly on the cmp operand. Returns nullptr if no load is found.
static const LoadStmt* findBackingLoad(const SVFVar* var)
{
    if (var->getInEdges().empty())
        return nullptr;
    SVFStmt* inStmt = *var->getInEdges().begin();
    if (const LoadStmt* ls = SVFUtil::dyn_cast<LoadStmt>(inStmt))
        return ls;
    if (const CopyStmt* cs = SVFUtil::dyn_cast<CopyStmt>(inStmt))
    {
        const SVFVar* src = cs->getRHSVar();
        if (!src->getInEdges().empty())
            return SVFUtil::dyn_cast<LoadStmt>(*src->getInEdges().begin());
    }
    return nullptr;
}

/// Compute the interval constraint on one cmp operand given the predicate,
/// branch direction (succ), which side it is on, and the other operand's
/// interval. Returns top if no useful narrowing is possible.
///
/// Called from collectBranchRefinement for each non-constant operand that has a
/// backing load. Given a branch condition like:
///
///   %cmp = icmp sgt %a, 5       ;  a > 5
///   br i1 %cmp, label %T, %F
///
/// On the true branch (succ=1), operand %a (isLHS=true) is constrained to
/// [6, +inf). On the false branch (succ=0), %a is constrained to (-inf, 5].
/// The result is used to narrow the ObjVar behind %a's load.
static AD::Interval computeCmpConstraint(s32_t predicate, s64_t succ,
        bool isLHS, const AD::Interval& self,
        const AD::Interval& other)
{
    // Normalize: always reason from the LHS perspective.
    // If we are the RHS operand, swap the predicate direction.
    if (!isLHS)
    {
        // a > b from b's perspective: b < a
        static const Map<s32_t, s32_t> swapPred =
        {
            {CmpStmt::ICMP_EQ, CmpStmt::ICMP_EQ},
            {CmpStmt::ICMP_NE, CmpStmt::ICMP_NE},
            {CmpStmt::ICMP_SGT, CmpStmt::ICMP_SLT},
            {CmpStmt::ICMP_SGE, CmpStmt::ICMP_SLE},
            {CmpStmt::ICMP_SLT, CmpStmt::ICMP_SGT},
            {CmpStmt::ICMP_SLE, CmpStmt::ICMP_SGE},
            {CmpStmt::ICMP_UGT, CmpStmt::ICMP_ULT},
            {CmpStmt::ICMP_UGE, CmpStmt::ICMP_ULE},
            {CmpStmt::ICMP_ULT, CmpStmt::ICMP_UGT},
            {CmpStmt::ICMP_ULE, CmpStmt::ICMP_UGE},
            {CmpStmt::FCMP_OEQ, CmpStmt::FCMP_OEQ},
            {CmpStmt::FCMP_UEQ, CmpStmt::FCMP_UEQ},
            {CmpStmt::FCMP_OGT, CmpStmt::FCMP_OLT},
            {CmpStmt::FCMP_OGE, CmpStmt::FCMP_OLE},
            {CmpStmt::FCMP_OLT, CmpStmt::FCMP_OGT},
            {CmpStmt::FCMP_OLE, CmpStmt::FCMP_OGE},
            {CmpStmt::FCMP_UGT, CmpStmt::FCMP_ULT},
            {CmpStmt::FCMP_UGE, CmpStmt::FCMP_ULE},
            {CmpStmt::FCMP_ULT, CmpStmt::FCMP_UGT},
            {CmpStmt::FCMP_ULE, CmpStmt::FCMP_UGE},
            {CmpStmt::FCMP_ONE, CmpStmt::FCMP_ONE},
            {CmpStmt::FCMP_UNE, CmpStmt::FCMP_UNE},
        };
        auto it = swapPred.find(predicate);
        if (it == swapPred.end())
            return AD::Interval::top();
        predicate = it->second;
    }

    // If false branch, negate the predicate.
    if (succ == 0)
    {
        static const Map<s32_t, s32_t> negPred =
        {
            {CmpStmt::ICMP_EQ, CmpStmt::ICMP_NE},
            {CmpStmt::ICMP_NE, CmpStmt::ICMP_EQ},
            {CmpStmt::ICMP_SGT, CmpStmt::ICMP_SLE},
            {CmpStmt::ICMP_SGE, CmpStmt::ICMP_SLT},
            {CmpStmt::ICMP_SLT, CmpStmt::ICMP_SGE},
            {CmpStmt::ICMP_SLE, CmpStmt::ICMP_SGT},
            {CmpStmt::ICMP_UGT, CmpStmt::ICMP_ULE},
            {CmpStmt::ICMP_UGE, CmpStmt::ICMP_ULT},
            {CmpStmt::ICMP_ULT, CmpStmt::ICMP_UGE},
            {CmpStmt::ICMP_ULE, CmpStmt::ICMP_UGT},
            {CmpStmt::FCMP_OEQ, CmpStmt::FCMP_ONE},
            {CmpStmt::FCMP_UEQ, CmpStmt::FCMP_UNE},
            {CmpStmt::FCMP_OGT, CmpStmt::FCMP_OLE},
            {CmpStmt::FCMP_OGE, CmpStmt::FCMP_OLT},
            {CmpStmt::FCMP_OLT, CmpStmt::FCMP_OGE},
            {CmpStmt::FCMP_OLE, CmpStmt::FCMP_OGT},
            {CmpStmt::FCMP_UGT, CmpStmt::FCMP_ULE},
            {CmpStmt::FCMP_UGE, CmpStmt::FCMP_ULT},
            {CmpStmt::FCMP_ULT, CmpStmt::FCMP_UGE},
            {CmpStmt::FCMP_ULE, CmpStmt::FCMP_UGT},
            {CmpStmt::FCMP_ONE, CmpStmt::FCMP_OEQ},
            {CmpStmt::FCMP_UNE, CmpStmt::FCMP_UEQ},
        };
        auto it = negPred.find(predicate);
        if (it == negPred.end())
            return AD::Interval::top();
        predicate = it->second;
    }

    // Now compute the constraint on LHS given: LHS <predicate> other
    AD::Interval result = self;
    switch (predicate)
    {
    case CmpStmt::ICMP_EQ:
    case CmpStmt::FCMP_OEQ:
    case CmpStmt::FCMP_UEQ:
        result.meetWith(other);
        break;
    case CmpStmt::ICMP_NE:
    case CmpStmt::FCMP_ONE:
    case CmpStmt::FCMP_UNE:
    case CmpStmt::FCMP_FALSE:
    case CmpStmt::FCMP_TRUE:
        // Original does not narrow memory on disequality, even when Box
        // could remove an interval endpoint. Keep that policy in the AE layer.
        return AD::Interval::top();
    case CmpStmt::ICMP_UGT:
    case CmpStmt::ICMP_SGT:
    case CmpStmt::FCMP_OGT:
    case CmpStmt::FCMP_UGT:
        if (!other.lower().isFinite())
            return result;
        result.meetWith(
            AD::Interval(AD::Bound::finite(other.lower().value(), true),
                         AD::Bound::plusInfinity()));
        break;
    case CmpStmt::ICMP_UGE:
    case CmpStmt::ICMP_SGE:
    case CmpStmt::FCMP_OGE:
    case CmpStmt::FCMP_UGE:
        if (!other.lower().isFinite())
            return result;
        result.meetWith(
            AD::Interval(AD::Bound::finite(other.lower().value(),
                                           other.lower().isStrict()),
                         AD::Bound::plusInfinity()));
        break;
    case CmpStmt::ICMP_ULT:
    case CmpStmt::ICMP_SLT:
    case CmpStmt::FCMP_OLT:
    case CmpStmt::FCMP_ULT:
        if (!other.upper().isFinite())
            return result;
        result.meetWith(
            AD::Interval(AD::Bound::minusInfinity(),
                         AD::Bound::finite(other.upper().value(), true)));
        break;
    case CmpStmt::ICMP_ULE:
    case CmpStmt::ICMP_SLE:
    case CmpStmt::FCMP_OLE:
    case CmpStmt::FCMP_ULE:
        if (!other.upper().isFinite())
            return result;
        result.meetWith(
            AD::Interval(AD::Bound::minusInfinity(),
                         AD::Bound::finite(other.upper().value(),
                                           other.upper().isStrict())));
        break;
    default:
        return AD::Interval::top();
    }
    return result;
}

void AbstractInterpretation::collectBranchRefinement(
    const IntraCFGEdge* edge, AbstractDomain::AbstractDomain& state)
{
    const SVFVar* cond = edge->getCondition();
    const ICFGNode* pred = edge->getSrcNode();
    const ICFGNode* succNode = edge->getDstNode();
    s64_t succ = edge->getSuccessorCondValue();

    // Some frontends leave synthetic branch conditions without a defining
    // SVF statement. Assertions are disabled in benchmark builds, so do not
    // dereference the empty edge set; there is simply no sound refinement to
    // collect in this case.
    if (cond->getInEdges().empty())
        return;
    const SVFStmt* condDef = *cond->getInEdges().begin();

    if (const CmpStmt* cmpStmt = SVFUtil::dyn_cast<CmpStmt>(condDef))
    {
        s32_t predicate = cmpStmt->getPredicate();

        if (cmpStmt->getOpVarID(0) == IRGraph::NullPtr ||
                cmpStmt->getOpVarID(1) == IRGraph::NullPtr)
        {
            // p == NULL / p != NULL: no interval obj to refine.
        }
        else
        {
            AD::Interval opVal[2] = {getInterval(cmpStmt->getOpVar(0), pred),
                                     getInterval(cmpStmt->getOpVar(1), pred)
                                    };
            AD::AddressSet opAddr[2] =
            {
                getAddressSet(cmpStmt->getOpVar(0), pred),
                getAddressSet(cmpStmt->getOpVar(1), pred)
            };

            if ((opVal[0].isBottom() || opVal[1].isBottom()) &&
                    (!opAddr[0].isBottom() || !opAddr[1].isBottom()))
            {
                // Pointer-valued cmp: branch feasibility only.
            }
            else
            {
                for (int i = 0; i < 2; i++)
                {
                    const int other = 1 - i;
                    const LoadStmt* load =
                        findBackingLoad(cmpStmt->getOpVar(i));

                    if (opVal[i].isSingleton())
                    {
                        // Example: in x < 5, operand 5 is not refined.
                    }
                    else if (!opVal[other].isSingleton())
                    {
                        // Match Original: refine a loaded value only against
                        // a fixed numerical bound, not another interval.
                    }
                    else if (!load)
                    {
                        // Example: cmp uses a computed temporary, not load p.
                    }
                    else
                    {
                        AD::Interval narrowed = computeCmpConstraint(
                                                    predicate, succ, i == 0, opVal[i], opVal[other]);

                        if (narrowed.isTop())
                        {
                            // != and unsupported predicates reach here.
                        }
                        else
                        {
                            const ICFGNode* loadIcfg = load->getICFGNode();
                            const AD::AddressSet ptrVal =
                                getAddressSet(load->getRHSVar(), loadIcfg);
                            if (ptrVal.isBottom() ||
                                    ptrVal.hasUnknownObject())
                            {
                                // Cannot map load p back to concrete ObjVars.
                            }
                            else
                            {
                                for (const AD::Location location : ptrVal)
                                {
                                    if (const ObjVar* object =
                                                objectAt(location))
                                        recordBranchRefinement(
                                            object->getId(), narrowed, state,
                                            loadIcfg, succNode);
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    else
    {
        const SVFVar* var = cond;

        AD::Interval switch_cond = getInterval(var, pred);
        switch_cond.meetWith(AD::Interval::singleton(AD::Rational(succ)));
        if (switch_cond.isBottom())
        {
            // This case label is not reachable from cond's interval.
        }
        else
        {
            FIFOWorkList<const SVFStmt*> stmtList;
            for (SVFStmt* stmt : var->getInEdges())
                stmtList.push(stmt);
            while (!stmtList.empty())
            {
                const SVFStmt* stmt = stmtList.pop();
                const LoadStmt* load = SVFUtil::dyn_cast<LoadStmt>(stmt);
                if (!load)
                {
                    // Skip non-load definitions of the switch condition.
                }
                else
                {
                    const ICFGNode* loadIcfg = load->getICFGNode();
                    const AD::AddressSet ptrVal =
                        getAddressSet(load->getRHSVar(), loadIcfg);
                    if (ptrVal.isBottom() || ptrVal.hasUnknownObject())
                    {
                        // Cannot map load p back to concrete ObjVars.
                    }
                    else
                    {
                        for (const AD::Location location : ptrVal)
                        {
                            if (const ObjVar* object = objectAt(location))
                                recordBranchRefinement(object->getId(),
                                                       switch_cond, state,
                                                       loadIcfg, succNode);
                        }
                    }
                }
            }
        }
    }
}

void AbstractInterpretation::recordBranchRefinement(
    NodeID objectId, const AD::Interval& narrowed,
    AD::AbstractDomain& abstractState, const ICFGNode*, const ICFGNode*)
{
    const auto* object = SVFUtil::dyn_cast<ObjVar>(svfir->getGNode(objectId));
    if (!object || object->isPointer())
        return;

    State& denseState = static_cast<State&>(abstractState);
    const AD::Variable content = adapter_.contentVariable(*object);
    AD::Interval refined = denseState.numerical().bound(content);
    refined.meetWith(narrowed);
    assignInterval(denseState, content, refined);
}

/**
 * Handle an ICFG node: execute statements on the current abstract state.
 * The node's pre-state must already be installed by
 * mergeStatesFromPredecessors, or by handleGlobalNode for the global node.
 * Returns true if the abstract state has changed, false if fixpoint reached or
 * unreachable.
 */
bool AbstractInterpretation::handleICFGNode(const ICFGNode* node)
{
    // Check reachability: pre-state must have been propagated by predecessors
    bool isFunEntry = SVFUtil::isa<FunEntryICFGNode>(node);
    if (!hasAbsState(node))
    {
        if (isFunEntry)
        {
            // Entry point with no callers: inherit from global node
            const ICFGNode* globalNode = icfg->getGlobalICFGNode();
            if (hasAbsState(globalNode))
            {
                copyAbstractState(globalNode, node);
            }
            else
            {
                resetAbstractState(node);
            }
        }
        else
        {
            return false; // unreachable node
        }
    }

    // Store the previous state for fixpoint detection
    std::unique_ptr<AbstractDomain::AbstractDomain> previousState =
        cloneAbstractState(node);

    stat->getBlockTrace()++;
    stat->getICFGNodeTrace()++;

    // Handle SVF statements
    for (const SVFStmt* stmt : node->getSVFStmts())
    {
        handleSVFStatement(stmt);
    }

    // Handle call sites
    if (const CallICFGNode* callNode = SVFUtil::dyn_cast<CallICFGNode>(node))
    {
        handleCallSite(callNode);
    }

    // Run detectors
    for (auto& detector : detectors)
        detector->detect(node);

    finalizeAbstractState(node);
    // Track this node as analyzed (for coverage statistics across all entry
    // points)
    allAnalyzedNodes.insert(node);

    if (isAbstractStateEquivalent(node, *previousState))
        return false;

    return true;
}

/**
 * Handle a function using worklist algorithm guided by WTO order.
 * All top-level WTO components are pushed into the worklist upfront,
 * so the traversal order is exactly the WTO order — each node is
 * visited once, and cycles are handled as whole components.
 */
bool AbstractInterpretation::handleFunction(const ICFGNode* funEntry,
                                            const CallICFGNode* caller)
{
    auto it = preAnalysis->getFuncToWTO().find(funEntry->getFun());
    assert(it != preAnalysis->getFuncToWTO().end() &&
           "Missing WTO for function");

    Map<const ICFGNode*, State> before;
    for (auto nodeIterator = icfg->begin(); nodeIterator != icfg->end();
         ++nodeIterator)
    {
        const ICFGNode* node = nodeIterator->second;
        if (node->getFun() == funEntry->getFun() && hasAbsState(node))
            before.emplace(node, state(node));
    }

    // Push all top-level WTO components into the worklist in WTO order
    FIFOWorkList<const ICFGWTOComp*> worklist(it->second->getWTOComponents());

    while (!worklist.empty())
    {
        const ICFGWTOComp* comp = worklist.pop();

        if (const ICFGSingletonWTO* singleton =
                    SVFUtil::dyn_cast<ICFGSingletonWTO>(comp))
        {
            const ICFGNode* node = singleton->getICFGNode();
            if (mergeStatesFromPredecessors(node))
                handleICFGNode(node);
        }
        else if (const ICFGCycleWTO* cycle =
                     SVFUtil::dyn_cast<ICFGCycleWTO>(comp))
        {
            if (mergeStatesFromPredecessors(cycle->head()->getICFGNode()))
                handleLoopOrRecursion(cycle, caller);
        }
    }
    for (auto nodeIterator = icfg->begin(); nodeIterator != icfg->end();
         ++nodeIterator)
    {
        const ICFGNode* node = nodeIterator->second;
        if (node->getFun() != funEntry->getFun() || !hasAbsState(node))
            continue;
        const auto previous = before.find(node);
        if (previous == before.end() ||
            state(node).isEquivalentTo(previous->second) !=
                AD::CheckResult::True)
            return true;
    }
    return false;
}

void AbstractInterpretation::handleCallSite(const ICFGNode* node)
{
    if (const CallICFGNode* callNode = SVFUtil::dyn_cast<CallICFGNode>(node))
    {
        if (isExtCall(callNode))
        {
            handleExtCall(callNode);
        }
        else
        {
            // Handle both direct and indirect calls uniformly
            handleFunCall(callNode);
        }
    }
    else
        assert(false && "it is not call node");
}

bool AbstractInterpretation::isExtCall(const CallICFGNode* callNode)
{
    return SVFUtil::isExtCall(callNode->getCalledFunction());
}

void AbstractInterpretation::handleExtCall(const CallICFGNode* callNode)
{
    utils->handleExtAPI(callNode);
    // Keep the external model's result, including an initialized Top. The
    // initialization guard carries that definition without an artificial range.
    for (auto& detector : detectors)
    {
        detector->handleStubFunctions(callNode);
    }
}

/// Get callee function: directly for direct calls, via pointer analysis for
/// indirect calls
const FunObjVar* AbstractInterpretation::getCallee(const CallICFGNode* callNode)
{
    // Direct call: get callee directly from call node
    if (const FunObjVar* callee = callNode->getCalledFunction())
        return callee;

    // Indirect call: resolve callee through pointer analysis
    const auto callsiteMaps = svfir->getIndirectCallsites();
    auto it = callsiteMaps.find(callNode);
    if (it == callsiteMaps.end())
        return nullptr;

    NodeID call_id = it->second;
    if (!hasAbsState(callNode))
        return nullptr;

    const AD::AddressSet addresses =
        getAddressSet(svfir->getSVFVar(call_id), callNode);
    if (!addresses.isFinite() || addresses.empty())
        return nullptr;

    const ObjVar* object = objectAt(*addresses.begin());
    return object ? SVFUtil::dyn_cast<FunObjVar>(object) : nullptr;
}

/// Handle direct or indirect call: get callee(s), process function body, set
/// return state.
///
/// For direct calls, the callee is known statically.
/// For indirect calls, the previous implementation resolved callees from the
/// abstract state's address domain, which only picked the first address and
/// missed other targets. Since the abstract state's address domain is not an
/// over-approximation for function pointers (it may be uninitialized or
/// incomplete), we now use Andersen's pointer analysis results from the
/// pre-computed call graph, which soundly resolves all possible indirect call
/// targets.
void AbstractInterpretation::handleFunCall(const CallICFGNode* callNode)
{
    if (skipRecursiveCall(callNode))
        return;

    // Direct call: callee is known
    if (const FunObjVar* callee = callNode->getCalledFunction())
    {
        const ICFGNode* calleeEntry = icfg->getFunEntryICFGNode(callee);
        handleFunction(calleeEntry, callNode);
        const RetICFGNode* retNode = callNode->getRetICFGNode();
        copyAbstractState(callNode, retNode);
        // The caller WTO can place a continuation before this return node.
        // Propagate the freshly enlarged context-insensitive callee summary
        // immediately; otherwise the continuation may retain a transiently
        // stronger state even though the return state changes later in the
        // same whole-function pass.
        if (mergeStatesFromPredecessors(retNode))
            handleICFGNode(retNode);
        return;
    }

    // Indirect call: use Andersen's call graph to get all resolved callees.
    const RetICFGNode* retNode = callNode->getRetICFGNode();
    bool analyzedCallee = false;
    if (callGraph->hasIndCSCallees(callNode))
    {
        const auto& callees = callGraph->getIndCSCallees(callNode);
        std::vector<const FunObjVar*> orderedCallees(callees.begin(),
                callees.end());
        std::sort(orderedCallees.begin(), orderedCallees.end(),
                  [](const FunObjVar* lhs, const FunObjVar* rhs)
        {
            return lhs->getId() < rhs->getId();
        });
        for (const FunObjVar* callee : orderedCallees)
        {
            if (callee->isDeclaration())
                continue;
            analyzedCallee = true;
            const ICFGNode* calleeEntry = icfg->getFunEntryICFGNode(callee);
            handleFunction(calleeEntry, callNode);
        }
    }
    if (!analyzedCallee)
    {
        // Original's missing SSA result reads as numerical Top. This is an
        // unresolved call, not an uninitialized object load (which stays Bottom).
        const SVFVar* result = retNode->getActualRet();
        if (result && getInterval(result, callNode).isBottom() &&
                getAddressSet(result, callNode).isBottom())
            updateInterval(result, AD::Interval::top(), callNode);
    }
    // Resume return node from caller's state (context-insensitive)
    copyAbstractState(callNode, retNode);
    if (analyzedCallee && mergeStatesFromPredecessors(retNode))
        handleICFGNode(retNode);
}

bool AbstractInterpretation::mergeStatesFromPredecessors(
    const ICFGNode* node)
{
    State merged = bottomState();
    bool hasFeasiblePredecessor = false;

    for (const ICFGEdge* edge : orderedIncomingEdges(node))
    {
        const ICFGNode* predecessor = edge->getSrcNode();
        if (stateTrace_.count(predecessor) == 0)
            continue;

        bool shouldMerge = false;
        const IntraCFGEdge* conditional = SVFUtil::dyn_cast<IntraCFGEdge>(edge);
        if (conditional)
            shouldMerge = true;
        else if (SVFUtil::isa<CallCFGEdge>(edge))
        {
            shouldMerge = true;
        }
        else if (SVFUtil::isa<RetCFGEdge>(edge))
        {
            const auto* returnSite = SVFUtil::dyn_cast<RetICFGNode>(node);
            // A shared callee exit can be reachable through another caller.
            // Preserve call/return matching even when recursive calls use a
            // TOP summary; the summary resumes through copyAbstractState().
            shouldMerge =
                returnSite &&
                stateTrace_.count(returnSite->getCallICFGNode()) != 0;
        }
        if (!shouldMerge)
            continue;

        State source = state(predecessor);
        if (conditional && conditional->getCondition())
        {
            assumeBranch(conditional, source);
            collectBranchRefinement(conditional, source);
        }
        if (source.isBottom())
            continue;

        merged.joinWith(source);
        hasFeasiblePredecessor = true;
    }

    if (!hasFeasiblePredecessor)
        return false;
    stateTrace_.insert_or_assign(node, std::move(merged));
    return true;
}

// Loop / recursion handling (handleLoopOrRecursion + cycle helpers +
// recursion utilities) lives in AELoopRecursion.cpp.

void AbstractInterpretation::handleSVFStatement(const SVFStmt* stmt)
{
    if (const AddrStmt* addr = SVFUtil::dyn_cast<AddrStmt>(stmt))
    {
        updateStateOnAddr(addr);
    }
    else if (const BinaryOPStmt* binary = SVFUtil::dyn_cast<BinaryOPStmt>(stmt))
    {
        updateStateOnBinary(binary);
    }
    else if (const CmpStmt* cmp = SVFUtil::dyn_cast<CmpStmt>(stmt))
    {
        updateStateOnCmp(cmp);
    }
    else if (SVFUtil::isa<UnaryOPStmt>(stmt))
    {
    }
    else if (SVFUtil::isa<BranchStmt>(stmt))
    {
        // branch stmt is handled in hasBranchES
    }
    else if (const LoadStmt* load = SVFUtil::dyn_cast<LoadStmt>(stmt))
    {
        updateStateOnLoad(load);
    }
    else if (const StoreStmt* store = SVFUtil::dyn_cast<StoreStmt>(stmt))
    {
        updateStateOnStore(store);
    }
    else if (const CopyStmt* copy = SVFUtil::dyn_cast<CopyStmt>(stmt))
    {
        updateStateOnCopy(copy);
    }
    else if (const GepStmt* gep = SVFUtil::dyn_cast<GepStmt>(stmt))
    {
        updateStateOnGep(gep);
    }
    else if (const SelectStmt* select = SVFUtil::dyn_cast<SelectStmt>(stmt))
    {
        updateStateOnSelect(select);
    }
    else if (const PhiStmt* phi = SVFUtil::dyn_cast<PhiStmt>(stmt))
    {
        updateStateOnPhi(phi);
    }
    else if (const CallPE* callPE = SVFUtil::dyn_cast<CallPE>(stmt))
    {
        // To handle Call Edge
        updateStateOnCall(callPE);
    }
    else if (const RetPE* retPE = SVFUtil::dyn_cast<RetPE>(stmt))
    {
        updateStateOnRet(retPE);
    }
    else
        assert(false && "implement this part");
    // NullPtr should not be changed by any statement. If the entry is missing
    // (not yet auto-inserted) we treat that as "unchanged" — only check the
    // entry if it actually exists.
}

void AbstractInterpretation::updateStateOnGep(const GepStmt* gep)
{
    const ICFGNode* node = gep->getICFGNode();
    const AD::Interval offset = getGepElementIndex(gep);
    updateAddressSet(
        gep->getLHSVar(),
        getGepObjAddrs(SVFUtil::cast<ValVar>(gep->getRHSVar()), offset, node),
        node);
}

void AbstractInterpretation::updateStateOnSelect(const SelectStmt* select)
{
    const ICFGNode* node = select->getICFGNode();
    const AD::Interval condition = getInterval(select->getCondition(), node);
    AD::Interval interval;
    AD::AddressSet addresses;
    std::vector<const SVFVar*> selectedValues;
    if (condition.isSingleton())
    {
        const SVFVar* selected = condition.isZero() ? select->getFalseValue()
                                 : select->getTrueValue();
        selectedValues.push_back(selected);
        interval = getInterval(selected, node);
        addresses = getAddressSet(selected, node);
    }
    else
    {
        selectedValues.push_back(select->getTrueValue());
        selectedValues.push_back(select->getFalseValue());
        interval = getInterval(select->getTrueValue(), node);
        interval.joinWith(getInterval(select->getFalseValue(), node));
        addresses = getAddressSet(select->getTrueValue(), node);
        addresses.joinWith(getAddressSet(select->getFalseValue(), node));
    }
    updateValue(select->getRes(), interval, addresses, node);

    if (Options::AEDomain() == AENumericalDomain::Box)
        return;
    const auto* target = SVFUtil::dyn_cast<ValVar>(select->getRes());
    if (!target || !adapter_.contains(*target))
        return;
    const AD::Variable targetVariable = adapter_.variable(*target);
    std::optional<State> relationalSelect;
    for (const SVFVar* selected : selectedValues)
    {
        const auto* source = SVFUtil::dyn_cast<ValVar>(selected);
        if (!source || !adapter_.contains(*source))
        {
            relationalSelect.reset();
            break;
        }
        State alternative = phiAlternativeState(node);
        const AD::Variable sourceVariable = adapter_.variable(*source);
        if (alternative.numericalMayBeUninitialized(sourceVariable))
        {
            relationalSelect.reset();
            break;
        }
        alternative.assignNumeric(targetVariable,
                                  AD::LinearExpression(sourceVariable));
        recordRelationalDependency(targetVariable, sourceVariable);
        alternative.setAddressSet(targetVariable,
                                  alternative.addressSet(sourceVariable));
        if (!relationalSelect)
            relationalSelect = std::move(alternative);
        else
            relationalSelect->joinWith(alternative);
    }
    if (relationalSelect)
    {
        relationalSelect->numerical().project(
            relationalSelect->numerical().relationalClosure({targetVariable}));
        scalarTransferState(node).numerical().meetWith(
            relationalSelect->numerical());
        recordRelationalSummary(targetVariable, *relationalSelect, node);
    }
}

void AbstractInterpretation::updateStateOnPhi(const PhiStmt* phi)
{
    const ICFGNode* icfgNode = phi->getICFGNode();
    const FunExitICFGNode* exit = icfgNode->getFun()
                                  ? icfg->getFunExitICFGNode(icfgNode->getFun())
                                  : nullptr;
    const bool isFormalReturn = exit && exit->getFormalRet() == phi->getRes();
    AD::Interval interval = AD::Interval::bottom();
    AD::AddressSet addresses = AD::AddressSet::bottom();
    std::optional<State> relationalPhi;
    for (u32_t i = 0; i < phi->getOpVarNum(); i++)
    {
        const ICFGNode* opICFGNode = phi->getOpICFGNode(i);
        if (hasAbsState(opICFGNode))
        {
            bool feasible = true;
            const ICFGEdge* edge =
                icfg->getICFGEdge(opICFGNode, icfgNode, ICFGEdge::IntraCF);
            if (edge)
            {
                const IntraCFGEdge* intraEdge =
                    SVFUtil::cast<IntraCFGEdge>(edge);
                if (intraEdge->getCondition())
                {
                    feasible = isBranchEdgeFeasibleAt(intraEdge, opICFGNode);
                }
            }
            if (feasible)
            {
                // A non-void source function that falls through has undefined
                // behavior. Clang lowers that path to an uninitialized return
                // slot; retain its guard for ordinary reads, but do not merge
                // the undefined alternative into the function's defined
                // return values.
                const AD::Interval operandInterval =
                    isFormalReturn
                        ? getDefinedInterval(phi->getOpVar(i), opICFGNode)
                        : getInterval(phi->getOpVar(i), opICFGNode);
                interval.joinWith(operandInterval);
                if (std::getenv("SVF_AE_TRACE_PHI_RELATIONS"))
                    SVFUtil::outs()
                        << "AE_PHI_OPERAND target=" << phi->getRes()->getId()
                        << " source=" << phi->getOpVar(i)->getId()
                        << " node=" << opICFGNode->getId()
                        << " interval=" << operandInterval.toString() << '\n';
                addresses.joinWith(getAddressSet(phi->getOpVar(i),
                                                 opICFGNode));

                if (Options::AEDomain() != AENumericalDomain::Box)
                {
                    const auto* target =
                        SVFUtil::dyn_cast<ValVar>(phi->getRes());
                    const auto* source =
                        SVFUtil::dyn_cast<ValVar>(phi->getOpVar(i));
                    if (target && source && adapter_.contains(*target) &&
                            adapter_.contains(*source))
                    {
                        State alternative = phiAlternativeState(opICFGNode);
                        const AD::Variable sourceVariable =
                            adapter_.variable(*source);
                        if (!alternative.numericalMayBeUninitialized(
                                    sourceVariable))
                        {
                            const AD::Variable targetVariable =
                                adapter_.variable(*target);
                            alternative.assignNumeric(
                                targetVariable,
                                AD::LinearExpression(sourceVariable));
                            recordRelationalDependency(targetVariable,
                                                       sourceVariable);
                            alternative.setAddressSet(
                                targetVariable,
                                alternative.addressSet(sourceVariable));
                            if (sourceVariable != targetVariable)
                                alternative.numerical().forget(sourceVariable);
                            if (std::getenv("SVF_AE_TRACE_PHI_RELATIONS"))
                                SVFUtil::outs()
                                    << "AE_PHI_ALTERNATIVE target="
                                    << targetVariable.id() << " source="
                                    << sourceVariable.id() << " state="
                                    << alternative.numerical().toString()
                                    << '\n';
                            if (!relationalPhi)
                                relationalPhi = std::move(alternative);
                            else
                                relationalPhi->joinWith(alternative);
                            if (std::getenv("SVF_AE_TRACE_PHI_RELATIONS"))
                                SVFUtil::outs()
                                    << "AE_PHI_JOIN target="
                                    << targetVariable.id() << " state="
                                    << relationalPhi->numerical().toString()
                                    << '\n';
                        }
                    }
                }
            }
        }
    }
    if (std::getenv("SVF_AE_TRACE_PHI_RELATIONS"))
        SVFUtil::outs()
            << "AE_PHI_INTERVAL target="
            << adapter_.variable(*SVFUtil::cast<ValVar>(phi->getRes())).id()
            << " interval=" << interval.toString() << '\n';
    // Phi operands are read from their predecessor program points rather than
    // solely from the immediate incoming state. Preserve the preceding
    // iterate when a later predecessor has grown; otherwise a phi split across
    // sequential ICFG nodes can remain one iteration behind the backedge and
    // violate its transfer equation. Widening here treats every split phi as
    // part of the enclosing loop header and guarantees termination even when
    // it is not the WTO component's first ICFG node.
    if (const auto* result = SVFUtil::dyn_cast<ValVar>(phi->getRes()))
    {
        AD::Interval previous = getDefinedInterval(result, icfgNode);
        previous.widenWith(interval);
        interval = std::move(previous);
        AD::AddressSet previousAddresses = getAddressSet(result, icfgNode);
        previousAddresses.joinWith(addresses);
        addresses = std::move(previousAddresses);
    }
    updateValue(phi->getRes(), interval, addresses, icfgNode);
    if (relationalPhi)
    {
        const auto* target = SVFUtil::dyn_cast<ValVar>(phi->getRes());
        if (target && adapter_.contains(*target))
        {
            const AD::Variable targetVariable = adapter_.variable(*target);
            relationalPhi->numerical().project(
                relationalPhi->numerical().relationalClosure({targetVariable}));
            scalarTransferState(icfgNode).numerical().meetWith(
                relationalPhi->numerical());
            recordRelationalSummary(targetVariable, *relationalPhi, icfgNode);
        }
    }
}

/// Handle CallPE: phi-like merging of actual parameters from all call sites
/// into the formal parameter at FunEntryICFGNode (e.g., formal =
/// join(actual1@cs1, actual2@cs2, ...))
void AbstractInterpretation::updateStateOnCall(const CallPE* callPE)
{
    const ICFGNode* node = callPE->getICFGNode();
    const SVFVar* res = callPE->getRes();
    AD::Interval interval = AD::Interval::bottom();
    AD::AddressSet addresses = AD::AddressSet::bottom();
    std::optional<State> relationalCall;
    for (u32_t i = 0; i < callPE->getOpVarNum(); i++)
    {
        const ICFGNode* opICFGNode = callPE->getOpCallICFGNode(i);
        if (hasAbsState(opICFGNode))
        {
            interval.joinWith(getInterval(callPE->getOpVar(i), opICFGNode));
            addresses.joinWith(getAddressSet(callPE->getOpVar(i),
                                             opICFGNode));
            if (Options::AEDomain() != AENumericalDomain::Box)
            {
                const auto* target = SVFUtil::dyn_cast<ValVar>(res);
                const auto* source =
                    SVFUtil::dyn_cast<ValVar>(callPE->getOpVar(i));
                if (target && source && adapter_.contains(*target) &&
                        adapter_.contains(*source))
                {
                    State alternative = phiAlternativeState(opICFGNode);
                    const AD::Variable sourceVariable =
                        adapter_.variable(*source);
                    if (!alternative.numericalMayBeUninitialized(
                                sourceVariable))
                    {
                        const AD::Variable targetVariable =
                            adapter_.variable(*target);
                        alternative.assignNumeric(
                            targetVariable,
                            AD::LinearExpression(sourceVariable));
                        recordRelationalDependency(targetVariable,
                                                   sourceVariable);
                        alternative.setAddressSet(
                            targetVariable,
                            alternative.addressSet(sourceVariable));
                        if (!relationalCall)
                            relationalCall = std::move(alternative);
                        else
                            relationalCall->joinWith(alternative);
                    }
                }
            }
        }
    }
    updateValue(res, interval, addresses, node);
    if (relationalCall)
    {
        const auto* target = SVFUtil::dyn_cast<ValVar>(res);
        if (target && adapter_.contains(*target))
        {
            const AD::Variable targetVariable = adapter_.variable(*target);
            relationalCall->numerical().project(
                relationalCall->numerical().relationalClosure({targetVariable}));
            scalarTransferState(node).numerical().meetWith(
                relationalCall->numerical());
            recordRelationalSummary(targetVariable, *relationalCall, node);
        }
    }
}

void AbstractInterpretation::updateStateOnRet(const RetPE* retPE)
{
    const ICFGNode* node = retPE->getICFGNode();
    updateValue(retPE->getLHSVar(),
                getDefinedInterval(retPE->getRHSVar(), node),
                getAddressSet(retPE->getRHSVar(), node), node);
    if (Options::AEDomain() == AENumericalDomain::Box)
        return;
    // A context-insensitive callee summary shared by multiple call sites has
    // one formal-return ghost. Relating every actual return to that same ghost
    // would spuriously relate distinct calls (for example, r1 == r2). Keep the
    // joined interval/address result, but do not export an affine equality
    // unless the callee has a single call site.
    bool sharedCallee = false;
    if (const auto* returnSite = SVFUtil::dyn_cast<RetICFGNode>(node))
    {
        for (const ICFGEdge* edge : returnSite->getInEdges())
        {
            if (!SVFUtil::isa<RetCFGEdge>(edge) ||
                    !edge->getSrcNode()->getFun())
                continue;
            const ICFGNode* entry = icfg->getFunEntryICFGNode(
                                        edge->getSrcNode()->getFun());
            const std::size_t callers = std::count_if(
                                            entry->getInEdges().begin(),
                                            entry->getInEdges().end(),
                                            [](const ICFGEdge* incoming) {
                return SVFUtil::isa<CallCFGEdge>(incoming);
            });
            sharedCallee |= callers > 1;
        }
    }
    if (sharedCallee)
        return;
    const auto* target = SVFUtil::dyn_cast<ValVar>(retPE->getLHSVar());
    const auto* source = SVFUtil::dyn_cast<ValVar>(retPE->getRHSVar());
    if (!target || !source || !adapter_.contains(*target) ||
            !adapter_.contains(*source))
        return;
    const AD::Variable sourceVariable = adapter_.variable(*source);
    if (!getDefinedInterval(source, node).isBottom())
        assignRelationalValue(target, AD::LinearExpression(sourceVariable),
                              getAddressSet(source, node), node);
}

void AbstractInterpretation::updateStateOnAddr(const AddrStmt* addr)
{
    const ICFGNode* node = addr->getICFGNode();
    const auto* object = SVFUtil::cast<ObjVar>(addr->getRHSVar());
    AD::Interval interval = AD::Interval::bottom();
    AD::AddressSet addresses = AD::AddressSet::bottom();
    initializeObjectValue(object, interval, addresses, node);
    if (addr->getRHSVar()->getType()->getKind() == SVFType::SVFIntegerTy)
        interval.meetWith(
            utils->getRangeLimitFromType(addr->getRHSVar()->getType()));
    updateValue(addr->getLHSVar(), interval, addresses, node);
}

void AbstractInterpretation::updateStateOnBinary(const BinaryOPStmt* binary)
{
    const ICFGNode* node = binary->getICFGNode();
    // Treat any unexpected bottom operand as unconstrained for soundness.
    AD::Interval lhs = getInterval(binary->getOpVar(0), node);
    AD::Interval rhs = getInterval(binary->getOpVar(1), node);
    if (lhs.isBottom())
        lhs = AD::Interval::top();
    if (rhs.isBottom())
        rhs = AD::Interval::top();
    AD::Interval result;
    switch (binary->getOpcode())
    {
    case BinaryOPStmt::Add:
    case BinaryOPStmt::FAdd:
        result = AD::add(lhs, rhs);
        break;
    case BinaryOPStmt::Sub:
    case BinaryOPStmt::FSub:
        result = AD::subtract(lhs, rhs);
        break;
    case BinaryOPStmt::Mul:
    case BinaryOPStmt::FMul:
        result = AD::multiply(lhs, rhs);
        break;
    case BinaryOPStmt::SDiv:
    case BinaryOPStmt::FDiv:
    case BinaryOPStmt::UDiv:
        result = AD::divide(lhs, rhs);
        break;
    case BinaryOPStmt::SRem:
    case BinaryOPStmt::FRem:
    case BinaryOPStmt::URem:
        result = AD::remainder(lhs, rhs);
        break;
    case BinaryOPStmt::Xor:
        result = AD::bitwiseXor(lhs, rhs);
        break;
    case BinaryOPStmt::And:
        result = AD::bitwiseAnd(lhs, rhs);
        break;
    case BinaryOPStmt::Or:
        result = AD::bitwiseOr(lhs, rhs);
        break;
    case BinaryOPStmt::AShr:
        result = AD::shiftRight(lhs, rhs);
        break;
    case BinaryOPStmt::Shl:
        result = AD::shiftLeft(lhs, rhs);
        break;
    case BinaryOPStmt::LShr:
        result = AD::shiftRight(lhs, rhs);
        break;
    default:
        assert(false && "undefined binary: ");
    }
    const auto* integerType =
        SVFUtil::dyn_cast<SVFIntegerType>(binary->getRes()->getType());
    const AD::Interval typeRange =
        integerType ? utils->getRangeLimitFromType(binary->getRes()->getType())
                    : AD::Interval::top();
    const bool noIntegerWrap =
        integerType && !result.isBottom() && result.isSubsetOf(typeRange);
    if (std::getenv("SVF_AE_TRACE_AFFINE_TRANSFER"))
        std::cerr << "AE affine candidate node=" << node->getId()
                  << " lhs=" << lhs.toString() << " rhs=" << rhs.toString()
                  << " result=" << result.toString() << " range="
                  << utils->getRangeLimitFromType(binary->getRes()->getType())
                         .toString()
                  << '\n';

    // Keep an affine equality when the LLVM integer operation cannot wrap
    // under the incoming bounds. This is the point where Octagon and
    // Polyhedra gain information beyond the interval baseline. Floating-point
    // operations retain interval semantics because rounding/NaN behavior is
    // not affine over rationals.
    if (Options::AEDomain() != AENumericalDomain::Box && noIntegerWrap)
    {
        const auto expressionFor = [&](const SVFVar* operand)
            -> std::optional<AD::LinearExpression>
        {
            if (const auto* value = SVFUtil::dyn_cast<ValVar>(operand))
            {
                if (adapter_.contains(*value))
                {
                    const AD::Variable variable = adapter_.variable(*value);
                    if (getDefinedInterval(value, node).isBottom())
                        return std::nullopt;
                    return AD::LinearExpression(variable);
                }
            }
            const AD::Interval constant = getInterval(operand, node);
            if (constant.isSingleton())
                return AD::LinearExpression(constant.singletonValue());
            return std::nullopt;
        };

        const auto lhsExpression = expressionFor(binary->getOpVar(0));
        const auto rhsExpression = expressionFor(binary->getOpVar(1));
        std::optional<AD::LinearExpression> affine;
        if (lhsExpression && rhsExpression)
        {
            if (binary->getOpcode() == BinaryOPStmt::Add)
                affine = *lhsExpression + *rhsExpression;
            else if (binary->getOpcode() == BinaryOPStmt::Sub)
                affine = *lhsExpression - *rhsExpression;
            else if (binary->getOpcode() == BinaryOPStmt::Mul)
            {
                if (lhs.isSingleton())
                    affine = lhs.singletonValue() * *rhsExpression;
                else if (rhs.isSingleton())
                    affine = rhs.singletonValue() * *lhsExpression;
            }
        }
        const auto* resultValue =
            SVFUtil::dyn_cast<ValVar>(binary->getRes());
        if (affine && resultValue && adapter_.contains(*resultValue))
        {
            assignRelationalValue(resultValue, *affine,
                                  AD::AddressSet::bottom(), node);
            return;
        }
    }
    if (integerType && !noIntegerWrap)
        result = AD::wrapIntegerInterval(
            result, binary->getRes()->getType()->getByteSize() * 8,
            integerType->isSigned());
    updateInterval(binary->getRes(), result, node);
}

void AbstractInterpretation::updateStateOnCmp(const CmpStmt* cmp)
{
    const ICFGNode* node = cmp->getICFGNode();
    AD::Interval lhsInterval = getInterval(cmp->getOpVar(0), node);
    AD::Interval rhsInterval = getInterval(cmp->getOpVar(1), node);
    const AD::AddressSet lhsAddresses = getAddressSet(cmp->getOpVar(0), node);
    const AD::AddressSet rhsAddresses = getAddressSet(cmp->getOpVar(1), node);
    const bool addressComparison = cmp->getOpVar(0)->isPointer();
    AD::Interval result =
        AD::Interval::closed(AD::Rational(0), AD::Rational(1));
    const auto boolean = [](bool value)
    {
        return AD::Interval::singleton(AD::Rational(value ? 1 : 0));
    };

    const auto predicate = cmp->getPredicate();
    if (!addressComparison &&
            (lhsInterval.isBottom() || rhsInterval.isBottom()))
    {
        // Original skips this transfer, then reads a never-defined SSA result
        // as Top. Preserve any existing result on subsequent loop iterations.
        if (getInterval(cmp->getRes(), node).isBottom())
            updateInterval(cmp->getRes(), AD::Interval::top(), node);
        return;
    }
    if (predicate == CmpStmt::FCMP_FALSE)
        result = boolean(false);
    else if (predicate == CmpStmt::FCMP_TRUE)
        result = boolean(true);
    else if (predicate == CmpStmt::FCMP_ORD || predicate == CmpStmt::FCMP_UNO)
    {
        // NaN is not tracked, so either outcome is possible.
    }
    else if (addressComparison)
    {
        const auto containsBlackHole = [&](const AD::AddressSet& addresses)
        {
            if (!addresses.isFinite())
                return true;
            return std::any_of(addresses.begin(), addresses.end(),
                               [&](AD::Location location)
            {
                const ObjVar* object = objectAt(location);
                const BaseObjVar* base = object
                                         ? svfir->getBaseObject(object->getId()) : nullptr;
                return base && base->isBlackHoleObj();
            });
        };
        const bool unknownAddress = containsBlackHole(lhsAddresses) ||
                                    containsBlackHole(rhsAddresses);
        const bool exact = lhsAddresses.isSingleton() &&
                           rhsAddresses.isSingleton() && !unknownAddress;
        const bool disjoint = lhsAddresses.isFinite() &&
                              rhsAddresses.isFinite() &&
                              !lhsAddresses.isBottom() &&
                              !rhsAddresses.isBottom() &&
                              !unknownAddress &&
                              !lhsAddresses.hasIntersection(rhsAddresses);
        switch (predicate)
        {
        case CmpStmt::ICMP_EQ:
        case CmpStmt::FCMP_OEQ:
        case CmpStmt::FCMP_UEQ:
            if (exact)
                result =
                    boolean(*lhsAddresses.begin() == *rhsAddresses.begin());
            else if (disjoint)
                result = boolean(false);
            break;
        case CmpStmt::ICMP_NE:
        case CmpStmt::FCMP_ONE:
        case CmpStmt::FCMP_UNE:
            if (exact)
                result =
                    boolean(*lhsAddresses.begin() != *rhsAddresses.begin());
            else if (disjoint)
                result = boolean(true);
            break;
        case CmpStmt::ICMP_UGT:
        case CmpStmt::ICMP_SGT:
        case CmpStmt::FCMP_OGT:
        case CmpStmt::FCMP_UGT:
        case CmpStmt::ICMP_UGE:
        case CmpStmt::ICMP_SGE:
        case CmpStmt::FCMP_OGE:
        case CmpStmt::FCMP_UGE:
        case CmpStmt::ICMP_ULT:
        case CmpStmt::ICMP_SLT:
        case CmpStmt::FCMP_OLT:
        case CmpStmt::FCMP_ULT:
        case CmpStmt::ICMP_ULE:
        case CmpStmt::ICMP_SLE:
        case CmpStmt::FCMP_OLE:
        case CmpStmt::FCMP_ULE:
            // Match Original's deliberate lack of pointer-order modelling,
            // including the same-known-singleton case. Equality is separate.
            break;
        default:
            assert(false && "undefined pointer compare");
        }
    }
    else
    {
        // Even when an operand is unconstrained, an LLVM comparison result is
        // still a Boolean.  Keeping [0, 1] here is both sound and important to
        // dense-equation replay: mathematical Top must not leak into the i1
        // result merely because a sparse operand summary is unavailable.
        switch (predicate)
        {
        case CmpStmt::ICMP_EQ:
        case CmpStmt::FCMP_OEQ:
        case CmpStmt::FCMP_UEQ:
            result = AD::equalTo(lhsInterval, rhsInterval);
            break;
        case CmpStmt::ICMP_NE:
        case CmpStmt::FCMP_ONE:
        case CmpStmt::FCMP_UNE:
            result = AD::notEqualTo(lhsInterval, rhsInterval);
            break;
        case CmpStmt::ICMP_UGT:
        case CmpStmt::ICMP_SGT:
        case CmpStmt::FCMP_OGT:
        case CmpStmt::FCMP_UGT:
            result = AD::greaterThan(lhsInterval, rhsInterval);
            break;
        case CmpStmt::ICMP_UGE:
        case CmpStmt::ICMP_SGE:
        case CmpStmt::FCMP_OGE:
        case CmpStmt::FCMP_UGE:
            result = AD::greaterEqual(lhsInterval, rhsInterval);
            break;
        case CmpStmt::ICMP_ULT:
        case CmpStmt::ICMP_SLT:
        case CmpStmt::FCMP_OLT:
        case CmpStmt::FCMP_ULT:
            result = AD::lessThan(lhsInterval, rhsInterval);
            break;
        case CmpStmt::ICMP_ULE:
        case CmpStmt::ICMP_SLE:
        case CmpStmt::FCMP_OLE:
        case CmpStmt::FCMP_ULE:
            result = AD::lessEqual(lhsInterval, rhsInterval);
            break;
        default:
            assert(false && "undefined numerical compare");
        }
    }
    updateInterval(cmp->getRes(), result, node);
}

void AbstractInterpretation::updateStateOnLoad(const LoadStmt* load)
{
    const ICFGNode* node = load->getICFGNode();
    AD::Interval interval;
    AD::AddressSet addresses;
    bool numericalMayBeUninitialized = false;
    loadValue(SVFUtil::cast<ValVar>(load->getRHSVar()), interval, addresses,
              numericalMayBeUninitialized, node);
    updateValue(load->getLHSVar(), interval, addresses, node);
    if (numericalMayBeUninitialized)
        addUninitializedNumericalAlternative(load->getLHSVar(), node);
    if (Options::AEDomain() != AENumericalDomain::Box)
    {
        const AD::AddressSet pointees = getAddressSet(load->getRHSVar(), node);
        if (pointees.isSingleton())
        {
            const AD::Location location = *pointees.begin();
            const ObjVar* object = location.isNull() ? nullptr
                                   : objectAt(location);
            if (object)
                assignRelationalLoad(
                    SVFUtil::dyn_cast<ValVar>(load->getLHSVar()),
                    memoryVariable(*object, ensureState(node)), node);
        }
    }
}

void AbstractInterpretation::updateStateOnStore(const StoreStmt* store)
{
    const ICFGNode* node = store->getICFGNode();
    storeValue(SVFUtil::cast<ValVar>(store->getLHSVar()),
               getInterval(store->getRHSVar(), node),
               getAddressSet(store->getRHSVar(), node), node);
    if (Options::AEDomain() != AENumericalDomain::Box)
    {
        const AD::AddressSet pointees = getAddressSet(store->getLHSVar(), node);
        if (pointees.isSingleton())
        {
            const AD::Location location = *pointees.begin();
            const ObjVar* object = location.isNull() ? nullptr
                                   : objectAt(location);
            if (object)
                assignRelationalStore(
                    SVFUtil::dyn_cast<ValVar>(store->getRHSVar()),
                    memoryVariable(*object, ensureState(node)), node);
        }
    }
}

void AbstractInterpretation::updateStateOnCopy(const CopyStmt* copy)
{
    const ICFGNode* node = copy->getICFGNode();
    const SVFVar* lhsVar = copy->getLHSVar();
    const SVFVar* rhsVar = copy->getRHSVar();

    const AD::Interval rhsInterval = getInterval(rhsVar, node);
    const AD::AddressSet rhsAddresses = getAddressSet(rhsVar, node);

    const bool exactAffineCopy =
        Options::AEDomain() != AENumericalDomain::Box &&
        (copy->getCopyKind() == CopyStmt::COPYVAL ||
         copy->getCopyKind() == CopyStmt::SEXT) &&
        SVFUtil::isa<SVFIntegerType>(lhsVar->getType()) &&
        SVFUtil::isa<SVFIntegerType>(rhsVar->getType()) &&
        !rhsInterval.isBottom() &&
        rhsInterval.isSubsetOf(utils->getRangeLimitFromType(lhsVar->getType()));
    if (exactAffineCopy)
    {
        const auto* lhsValue = SVFUtil::dyn_cast<ValVar>(lhsVar);
        const auto* rhsValue = SVFUtil::dyn_cast<ValVar>(rhsVar);
        if (lhsValue && rhsValue && adapter_.contains(*lhsValue) &&
                adapter_.contains(*rhsValue))
        {
            const AD::Variable source = adapter_.variable(*rhsValue);
            if (!getDefinedInterval(rhsValue, node).isBottom())
            {
                assignRelationalValue(lhsValue, AD::LinearExpression(source),
                                      rhsAddresses, node);
                return;
            }
        }
    }

    if (copy->getCopyKind() == CopyStmt::COPYVAL)
    {
        updateValue(lhsVar, rhsInterval, rhsAddresses, node);
    }
    else if (copy->getCopyKind() == CopyStmt::ZEXT)
    {
        const auto* sourceType =
            SVFUtil::dyn_cast<SVFIntegerType>(rhsVar->getType());
        updateInterval(lhsVar,
                       sourceType ? AD::zeroExtendIntegerInterval(
                                        rhsInterval,
                                        rhsVar->getType()->getByteSize() * 8,
                                        sourceType->isSigned())
                                  : AD::Interval::top(),
                       node);
    }
    else if (copy->getCopyKind() == CopyStmt::SEXT)
    {
        updateInterval(lhsVar, rhsInterval, node);
    }
    else if (copy->getCopyKind() == CopyStmt::FPTOSI)
    {
        updateInterval(
            lhsVar,
            AD::floatToInteger(rhsInterval,
                               lhsVar->getType()->getByteSize() * 8, true),
            node);
    }
    else if (copy->getCopyKind() == CopyStmt::FPTOUI)
    {
        updateInterval(
            lhsVar,
            AD::floatToInteger(rhsInterval,
                               lhsVar->getType()->getByteSize() * 8, false),
            node);
    }
    else if (copy->getCopyKind() == CopyStmt::SITOFP)
    {
        updateInterval(lhsVar, rhsInterval, node);
    }
    else if (copy->getCopyKind() == CopyStmt::UITOFP)
    {
        updateInterval(lhsVar, rhsInterval, node);
    }
    else if (copy->getCopyKind() == CopyStmt::TRUNC)
    {
        const auto* destinationType =
            SVFUtil::dyn_cast<SVFIntegerType>(lhsVar->getType());
        updateInterval(
            lhsVar,
            destinationType
                ? AD::wrapIntegerInterval(rhsInterval,
                                          lhsVar->getType()->getByteSize() * 8,
                                          destinationType->isSigned())
                : AD::Interval::top(),
            node);
    }
    else if (copy->getCopyKind() == CopyStmt::FPTRUNC)
    {
        updateInterval(lhsVar, rhsInterval, node);
    }
    else if (copy->getCopyKind() == CopyStmt::INTTOPTR)
    {
        // Match Original AE's transfer policy: integer-derived addresses do
        // not enter modeled pointer value-flow. AddressSet can express raw
        // addresses, but this interpreter deliberately leaves the address
        // component empty. Original's unmaterialized SSA result reads as
        // numerical Top, which must survive a later store of this value.
        updateValue(lhsVar, AD::Interval::top(),
                    AD::AddressSet::bottom(), node);
    }
    else if (copy->getCopyKind() == CopyStmt::PTRTOINT)
    {
        updateInterval(lhsVar, AD::Interval::top(), node);
    }
    else if (copy->getCopyKind() == CopyStmt::BITCAST)
    {
        if (!rhsAddresses.isBottom())
            updateValue(lhsVar, rhsInterval, rhsAddresses, node);
    }
    else
        assert(false && "undefined copy kind");
}
