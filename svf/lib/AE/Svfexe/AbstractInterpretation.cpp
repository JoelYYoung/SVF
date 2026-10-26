//===- AbstractInterpretation.cpp -- Abstract interpretation -------------===//
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
#include "AE/Svfexe/AllocationSize.h"
#include "Graphs/CallGraph.h"
#include "SVFIR/SVFIR.h"
#include "Util/Options.h"
#include "Util/WorkList.h"
#include "WPA/Andersen.h"
#include <cmath>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <iomanip>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <tuple>
#include <unordered_set>

using namespace SVF;
using namespace SVFUtil;

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
    case AEDetector::ASSERTION:
        return "assertion";
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

static std::string queryKey(AEDetector::DetectorKind detector,
                            const ICFGNode* node, const SVFVar* operand,
                            const std::string& queryKind, const SVFStmt* site = nullptr)
{
    if (site)
    {
        if (site->getQuerySite().empty())
            throw std::invalid_argument("detector site lacks LLVM semantic identity");
        return std::to_string(static_cast<unsigned>(detector)) + ":v2:" +
               site->getQuerySite() + ':' + queryKind;
    }
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

SVF::AddressSet AbstractInterpretation::blackHoleAddressSet() const
{
    const auto* object = SVFUtil::dyn_cast<ObjVar>(
                             svfir->getGNode(IRGraph::BlackHole));
    if (!object)
        throw std::runtime_error("SVFIR has no BlackHole object");
    return SVF::AddressSet::singleton(adapter_.location(*object));
}

void AbstractInterpretation::handleGlobalNode()
{
    const ICFGNode* node = icfg->getGlobalICFGNode();
    stateTrace_.insert_or_assign(node, topState());
    for (const SVFStmt* statement : node->getSVFStmts())
        handleSVFStatement(statement);

    if (const auto* variable = SVFUtil::dyn_cast<ValVar>(
                                   svfir->getGNode(PAG::getPAG()->getBlkPtr())))
        updateValue(variable, SVF::Interval::top(),
                    blackHoleAddressSet(), node);
    // Global initialization executes once. Its GEP properties must not be
    // left as Unreachable merely because no ordinary ICFG visitor runs here.
    for (auto& detector : detectors)
        detector->detect(node);
}

void AbstractInterpretation::initializeObjectValue(
    const ObjVar* object, SVF::Interval& interval, SVF::AddressSet& addresses,
    const ICFGNode* node)
{
    interval = SVF::Interval::bottom();
    addresses = SVF::AddressSet::bottom();
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
                SVF::Interval::singleton(SVF::Rational(integer->getSExtValue()));
        else if (const auto* floating =
                     SVFUtil::dyn_cast<ConstFPObjVar>(object))
            interval = SVFIRAdapter::floatingConstant(floating->getFPValue());
        else if (SVFUtil::isa<ConstNullPtrObjVar>(object))
            addresses = SVF::AddressSet::singleton(SVF::Location::null());
        else if (!SVFUtil::isa<GlobalObjVar>(object))
            interval = SVF::Interval::top();
        if (!interval.isBottom() || !addresses.isBottom())
            return;
    }
    addresses = SVF::AddressSet::singleton(adapter_.location(*object));
}

void AbstractInterpretation::runOnModule()
{
    stat->startClk();
    utils = new AbsExtAPI(this);
    enumerateQueries();
    utils->collectCheckPoint();
    analyse();
    utils->checkPointAllSet();
    stat->endClk();
    stat->finializeStat();
    if (Options::PStat())
        stat->performStat();
    for (auto& detector : detectors)
        detector->reportBug();
    writeQueryLedger();
}

// namespace

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
    enumerateStandardAssertionQueries();
}

namespace
{
bool isStandardAssertionFailure(const CallICFGNode* call)
{
    const FunObjVar* function = call ? call->getCalledFunction() : nullptr;
    if (!function)
        return false;
    const std::string name = function->getName();
    return name == "__assert_fail" || name == "__assert_rtn" ||
           name == "__assert_func" || name == "__assert2";
}

bool isDirectSVFAssertion(const CallICFGNode* call)
{
    const FunObjVar* function = call ? call->getCalledFunction() : nullptr;
    return function && function->getName() == "svf_assert" &&
           call->arg_size() >= 1;
}
} // namespace

void AbstractInterpretation::enumerateStandardAssertionQueries()
{
    for (auto iterator = icfg->begin(); iterator != icfg->end(); ++iterator)
    {
        const auto* call = SVFUtil::dyn_cast<CallICFGNode>(iterator->second);
        if (isStandardAssertionFailure(call))
            registerQuery(AEDetector::ASSERTION, call, nullptr,
                          "standard-assert-failure");
        else if (isDirectSVFAssertion(call))
            registerQuery(AEDetector::ASSERTION, call, call->getArgument(0),
                          "assert-condition");
    }
}

bool AbstractInterpretation::isAssertionTrue(const ValVar* condition,
        const ICFGNode* node)
{
    const SVF::Interval value = getInterval(condition, node);
    if (value == SVF::Interval::singleton(SVF::Rational(1)))
        return true;
    // SVF sign-extends literal i1 true to -1, while comparison transfers use
    // 1. Both encode the set bit. Do not accept -1 for wider integer helpers.
    const auto* type = SVFUtil::dyn_cast<SVFIntegerType>(condition->getType());
    return type && type->isBoolean() &&
           value == SVF::Interval::singleton(SVF::Rational(-1));
}

void AbstractInterpretation::recordReachedAssertion(
    const CallICFGNode* call)
{
    if (isStandardAssertionFailure(call))
    {
        recordQuery(AEDetector::ASSERTION, call, nullptr,
                    "standard-assert-failure", QueryOutcome::May,
                    "assertion failure block is abstractly reachable");
    }
    else if (isDirectSVFAssertion(call))
    {
        const ValVar* condition = call->getArgument(0);
        const bool safe = isAssertionTrue(condition, call);
        recordQuery(AEDetector::ASSERTION, call, condition,
                    "assert-condition", safe ? QueryOutcome::Safe
                    : QueryOutcome::May,
                    safe ? "assertion condition is definitely true"
                    : "assertion condition may be false");
    }
}

void AbstractInterpretation::registerQuery(
    AEDetector::DetectorKind detector, const ICFGNode* node,
    const SVFVar* operand, const std::string& queryKind, const SVFStmt* site)
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
    record.semanticSite = site ? site->getQuerySite() : "";
    record.statement = site;
    auto inserted = queryLedger_.emplace(queryKey(detector, node, operand, queryKind, site),
                                         std::move(record));
    if (!inserted.second && site && inserted.first->second.statement != site)
        throw std::invalid_argument("duplicate detector semantic site: " + site->getQuerySite());
}

void AbstractInterpretation::recordQuery(
    AEDetector::DetectorKind detector, const ICFGNode* node,
    const SVFVar* operand, const std::string& queryKind, QueryOutcome outcome,
    const std::string& reason, const SVFStmt* site)
{
    if (!queryLedgerEnabled())
        return;
    registerQuery(detector, node, operand, queryKind, site);
    QueryRecord& record = queryLedger_.at(
                              queryKey(detector, node, operand, queryKind, site));
    // A Safe query keeps the relational-size tag if any visit needed it.
    const bool keepRelational = outcome == QueryOutcome::Safe &&
                                record.outcome == QueryOutcome::Safe &&
                                record.reason.find("[relational-size]") != std::string::npos;
    if (queryOutcomeRank(outcome) >= queryOutcomeRank(record.outcome) && !keepRelational)
    {
        record.outcome = outcome;
        record.reason = reason;
    }
}

void AbstractInterpretation::writeQueryLedger() const
{
    if (!queryLedgerEnabled())
        return;
    writeQueryLedgerTo(Options::AEQueryLedgerFile());
}

void AbstractInterpretation::writeQueryLedgerTo(const std::string& path) const
{
    std::ofstream output(path, std::ios::out | std::ios::trunc);
    if (!output)
        throw std::runtime_error("cannot open AE query ledger: " + path);

    output << "query_id\tinput_id\tdetector\tfunction\tsource_location\t"
              "icfg_node\toperand\tquery_kind\toutcome\treason\tsemantic_site\n";
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
              [](const QueryRecord* left, const QueryRecord* right)
    {
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
        const std::string identity = !record.semanticSite.empty()
                                     ? input + ":v2:" + detector + ':' + record.semanticSite + ':' + record.queryKind
                                     : input + ':' + detector + ':' +
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
                   : record.reason) << '\t' << escapeQueryField(record.semanticSite) << '\n';
    }
    if (!output)
        throw std::runtime_error("failed to write AE query ledger: " +
                                 Options::AEQueryLedgerFile());
}

AbstractInterpretation::AbstractInterpretation()
    : adapter_(*PAG::getPAG())
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
    static AbstractInterpretation* instance = []() -> AbstractInterpretation*
    {
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
        // Inputs are unknown values, unlike uninitialized object contents.
        // Match upstream's type-directed policy for external pointer inputs.
        for (const SVFVar* argument : funEntry->getFormalParms())
        {
            if (argument->getType()->isPointerTy())
                updateAddressSet(argument, blackHoleAddressSet(), globalNode);
            else
                updateInterval(argument, unknownInterval(argument), globalNode);
        }
        copyAbstractState(globalNode, funEntry);
        // A later call site can enlarge a context-insensitive callee summary.
        // Revisit the root until every caller and its downstream states cover
        // that enlarged summary.  Loop/recursion WTO handling remains
        // responsible for convergence; an arbitrary iteration cap would not
        // establish a post-fixpoint.
        handleFunction(funEntry, nullptr);
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
/// Follows an identity copy or signed extension, whose interval inverse
/// is intersection with the original memory interval. Other casts are excluded.
static const LoadStmt* findBackingLoad(const SVFVar* var)
{
    if (var->getInEdges().size() != 1)
        return nullptr;
    SVFStmt* inStmt = *var->getInEdges().begin();
    if (const LoadStmt* ls = SVFUtil::dyn_cast<LoadStmt>(inStmt))
        return ls;
    if (const CopyStmt* cs = SVFUtil::dyn_cast<CopyStmt>(inStmt))
    {
        const bool identity = cs->getCopyKind() == CopyStmt::COPYVAL &&
                              cs->getLHSVar()->getType() == cs->getRHSVar()->getType();
        const bool signedExtension = cs->getCopyKind() == CopyStmt::SEXT &&
                                     SVFUtil::isa<SVFIntegerType>(cs->getRHSVar()->getType()) &&
                                     cs->getLHSVar()->getType()->getByteSize() >= cs->getRHSVar()->getType()->getByteSize();
        if (!identity && !signedExtension)
            return nullptr;
        const SVFVar* src = cs->getRHSVar();
        if (src->getInEdges().size() == 1)
            return SVFUtil::dyn_cast<LoadStmt>(*src->getInEdges().begin());
    }
    return nullptr;
}

// Sufficient freshness proof, deliberately alias-independent: the load and
// branch are on one acyclic, unconditional, intra-block predecessor chain,
// with no intervening store or call. An SSA load result survives a write;
// the mutable object's content does not. Check through the branch (not merely
// through the comparison), because a write may also occur after the compare.
static bool loadedMemoryStillCurrent(const LoadStmt* load, const ICFGNode* branch)
{
    const ICFGNode* origin=load->getICFGNode();
    if (!origin->getBB() || origin->getBB()!=branch->getBB()) return false;
    std::set<const ICFGNode*> seen;
    for (const ICFGNode* cursor=branch;;)
    {
        if (!seen.insert(cursor).second || !SVFUtil::isa<IntraICFGNode>(cursor) ||
                cursor->getBB()!=origin->getBB()) return false;
        for (const SVFStmt* stmt:cursor->getSVFStmts())
            if (SVFUtil::isa<StoreStmt>(stmt)) return false;
        if (cursor==origin) return true;
        if (cursor->getInEdges().size()!=1) return false;
        const auto* edge=SVFUtil::dyn_cast<IntraCFGEdge>(*cursor->getInEdges().begin());
        if (!edge || edge->getCondition()) return false;
        cursor=edge->getSrcNode();
    }
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
static SVF::Interval computeCmpConstraint(s32_t predicate, s64_t succ,
        bool isLHS, const SVF::Interval& self,
        const SVF::Interval& other)
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
            return SVF::Interval::top();
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
            return SVF::Interval::top();
        predicate = it->second;
    }

    // Now compute the constraint on LHS given: LHS <predicate> other
    SVF::Interval result = self;
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
        return SVF::Interval::top();
    case CmpStmt::ICMP_UGT:
    case CmpStmt::ICMP_SGT:
    case CmpStmt::FCMP_OGT:
    case CmpStmt::FCMP_UGT:
        if (!other.lower().isFinite())
            return result;
        result.meetWith(
            SVF::Interval(SVF::Bound::finite(other.lower().value(), true),
                          SVF::Bound::plusInfinity()));
        break;
    case CmpStmt::ICMP_UGE:
    case CmpStmt::ICMP_SGE:
    case CmpStmt::FCMP_OGE:
    case CmpStmt::FCMP_UGE:
        if (!other.lower().isFinite())
            return result;
        result.meetWith(
            SVF::Interval(SVF::Bound::finite(other.lower().value(),
                                             other.lower().isStrict()),
                          SVF::Bound::plusInfinity()));
        break;
    case CmpStmt::ICMP_ULT:
    case CmpStmt::ICMP_SLT:
    case CmpStmt::FCMP_OLT:
    case CmpStmt::FCMP_ULT:
        if (!other.upper().isFinite())
            return result;
        result.meetWith(
            SVF::Interval(SVF::Bound::minusInfinity(),
                          SVF::Bound::finite(other.upper().value(), true)));
        break;
    case CmpStmt::ICMP_ULE:
    case CmpStmt::ICMP_SLE:
    case CmpStmt::FCMP_OLE:
    case CmpStmt::FCMP_ULE:
        if (!other.upper().isFinite())
            return result;
        result.meetWith(
            SVF::Interval(SVF::Bound::minusInfinity(),
                          SVF::Bound::finite(other.upper().value(),
                                             other.upper().isStrict())));
        break;
    default:
        return SVF::Interval::top();
    }
    return result;
}

void AbstractInterpretation::collectBranchRefinement(
    const IntraCFGEdge* edge, SVF::AbstractDomain& state)
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

    const CmpStmt* cmpStmt = edge->isSwitchEdge() ? nullptr : SVFUtil::dyn_cast<CmpStmt>(condDef);
    if (cmpStmt)
    {
        s32_t predicate = cmpStmt->getPredicate();
        const bool unsignedPredicate = predicate == CmpStmt::ICMP_ULT ||
                                       predicate == CmpStmt::ICMP_ULE ||
                                       predicate == CmpStmt::ICMP_UGT ||
                                       predicate == CmpStmt::ICMP_UGE;
        if (!unsignedPredicate &&
                predicate!=CmpStmt::ICMP_EQ && predicate!=CmpStmt::ICMP_NE &&
                predicate!=CmpStmt::ICMP_SLT && predicate!=CmpStmt::ICMP_SLE &&
                predicate!=CmpStmt::ICMP_SGT && predicate!=CmpStmt::ICMP_SGE)
            return;

        if (cmpStmt->getOpVarID(0) == IRGraph::NullPtr ||
                cmpStmt->getOpVarID(1) == IRGraph::NullPtr)
        {
            // p == NULL / p != NULL: no interval obj to refine.
        }
        else
        {
            SVF::Interval opVal[2] = {getInterval(cmpStmt->getOpVar(0), pred),
                                      getInterval(cmpStmt->getOpVar(1), pred)
                                     };
            // Unsigned and mathematical order agree on non-negative operands.
            if (unsignedPredicate)
                for (const auto& interval : opVal)
                    if (interval.isBottom() || !interval.lower().isFinite() ||
                            interval.lower().value() < SVF::Rational(0))
                        return;
            SVF::AddressSet opAddr[2] =
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
                    else if (!load || !loadedMemoryStillCurrent(load,pred))
                    {
                        // Example: cmp uses a computed temporary, not load p.
                    }
                    else
                    {
                        SVF::Interval narrowed = computeCmpConstraint(
                                                     predicate, succ, i == 0, opVal[i], opVal[other]);

                        if (narrowed.isTop())
                        {
                            // != and unsupported predicates reach here.
                        }
                        else
                        {
                            const ICFGNode* loadIcfg = load->getICFGNode();
                            const SVF::AddressSet ptrVal =
                                getAddressSet(load->getRHSVar(), loadIcfg);
                            if (!ptrVal.isSingleton() ||
                                    ptrVal.hasUnknownObject())
                            {
                                // Cannot map load p back to concrete ObjVars.
                            }
                            else
                            {
                                for (const SVF::Location location : ptrVal)
                                {
                                    if (const ObjVar* object = objectAt(location))
                                        if (canRefineMemory(ptrVal,*object,static_cast<State&>(state)))
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

        SVF::Interval switch_cond = getInterval(var, pred);
        if (edge->isSwitchEdge())
        {
            const auto& cases = edge->getSwitchCases();
            if (edge->includesSwitchDefault() || edge->getSwitchWidth() < 32 ||
                    edge->getSwitchWidth() > 64 || cases.empty())
                return;
            const auto limits = std::minmax_element(cases.begin(), cases.end());
            switch_cond.meetWith(SVF::Interval(SVF::Bound::finite(SVF::Rational(*limits.first)),
                                               SVF::Bound::finite(SVF::Rational(*limits.second))));
        }
        else
            switch_cond.meetWith(SVF::Interval::singleton(SVF::Rational(succ)));
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
                if (!load || !loadedMemoryStillCurrent(load,pred))
                {
                    // Skip non-load definitions of the switch condition.
                }
                else
                {
                    const ICFGNode* loadIcfg = load->getICFGNode();
                    const SVF::AddressSet ptrVal =
                        getAddressSet(load->getRHSVar(), loadIcfg);
                    if (!ptrVal.isSingleton() || ptrVal.hasUnknownObject())
                    {
                        // Cannot map load p back to concrete ObjVars.
                    }
                    else
                    {
                        for (const SVF::Location location : ptrVal)
                        {
                            if (const ObjVar* object = objectAt(location))
                                if (canRefineMemory(ptrVal,*object,static_cast<State&>(state)))
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
    NodeID objectId, const SVF::Interval& narrowed,
    SVF::AbstractDomain& abstractState, const ICFGNode*, const ICFGNode*)
{
    const auto* object = SVFUtil::dyn_cast<ObjVar>(svfir->getGNode(objectId));
    if (!object || object->isPointer())
        return;

    State& denseState = static_cast<State&>(abstractState);
    const SVF::Variable content = adapter_.contentVariable(*object);
    // A guard intersects the current content, preserving every relation and
    // initialization facet. Assignment would forget those relations.
    if (narrowed.isBottom())
        denseState.numerical().assume(SVF::equal(SVF::LinearExpression(SVF::Rational(0)),
                SVF::LinearExpression(SVF::Rational(1))));
    else
    {
        if (narrowed.lower().isFinite())
            denseState.numerical().assume(SVF::LinearConstraint(
                                              SVF::LinearExpression(content)-SVF::LinearExpression(narrowed.lower().value()),
                                              narrowed.lower().isStrict()?SVF::ConstraintKind::GreaterThan:SVF::ConstraintKind::GreaterEqual));
        if (narrowed.upper().isFinite())
            denseState.numerical().assume(SVF::LinearConstraint(
                                              SVF::LinearExpression(content)-SVF::LinearExpression(narrowed.upper().value()),
                                              narrowed.upper().isStrict()?SVF::ConstraintKind::LessThan:SVF::ConstraintKind::LessEqual));
    }
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

    beginAbstractState(node);
    // Store the previous state for fixpoint detection
    std::unique_ptr<SVF::AbstractDomain> previousState =
        cloneAbstractState(node);

    stat->getBlockTrace()++;
    stat->getICFGNodeTrace()++;

    // A RetPE is owned by one RetCFGEdge and was executed before incoming
    // alternatives were joined. Executing every RetPE here would turn
    // alternative callees into sequential strong assignments.
    const bool returnBindingsApplied =
        SVFUtil::isa<RetICFGNode>(node) &&
        std::any_of(node->getInEdges().begin(), node->getInEdges().end(),
                    [](const ICFGEdge* edge)
    {
        const auto* ret = SVFUtil::dyn_cast<RetCFGEdge>(edge);
        return ret && ret->getRetPE();
    });

    // Handle SVF statements.
    for (const SVFStmt* stmt : node->getSVFStmts())
    {
        if (returnBindingsApplied && SVFUtil::isa<RetPE>(stmt))
            continue;
        handleSVFStatement(stmt);
    }
    if (returnBindingsApplied)
        recordMergedReturnSummary(SVFUtil::cast<RetICFGNode>(node));

    // Handle call sites
    if (const CallICFGNode* callNode = SVFUtil::dyn_cast<CallICFGNode>(node))
    {
        recordReachedAssertion(callNode);
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
                SVF::CheckResult::True)
            return true;
    }
    return false;
}

void AbstractInterpretation::refreshSiblingReturnSites(
    const FunObjVar* callee, const RetICFGNode* currentReturn)
{
    if (!callee)
        return;
    const ICFGNode* entry = icfg->getFunEntryICFGNode(callee);
    std::vector<const CallICFGNode*> callers;
    for (const ICFGEdge* incoming : entry->getInEdges())
    {
        const auto* callEdge = SVFUtil::dyn_cast<CallCFGEdge>(incoming);
        if (callEdge)
            callers.push_back(callEdge->getCallSite());
    }
    if (callers.size() <= 1)
        return;
    std::sort(callers.begin(), callers.end(),
              [](const CallICFGNode* left, const CallICFGNode* right)
    {
        return left->getId() < right->getId();
    });
    callers.erase(std::unique(callers.begin(), callers.end()), callers.end());
    for (const CallICFGNode* call : callers)
    {
        const RetICFGNode* returnSite = call->getRetICFGNode();
        if (!returnSite || returnSite == currentReturn || !hasAbsState(call))
            continue;
        copyAbstractState(call, returnSite);
        if (mergeStatesFromPredecessors(returnSite))
            handleICFGNode(returnSite);
    }
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

    const SVF::AddressSet addresses =
        getAddressSet(svfir->getSVFVar(call_id), callNode);
    if (!addresses.isFinite() || addresses.empty())
        return nullptr;

    const ObjVar* object = objectAt(*addresses.begin());
    return object ? SVFUtil::dyn_cast<FunObjVar>(object) : nullptr;
}

/// Process each callee and join its return state. Indirect targets come from
/// Andersen's call graph because AE address values may be incomplete.
void AbstractInterpretation::handleFunCall(const CallICFGNode* callNode)
{
    // The configured TOP summary has one synthetic call->return equation.
    // A mixed indirect target set needs a per-target summary/return join;
    // until that is implemented, do not silently use the direct-call rule.
    if (!callNode->getCalledFunction() && recursiveTopSummary(callNode))
        throw std::runtime_error(
            "AE_RECURSIVE_TOP_FAIL_CLOSED: indirect recursive target");
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
        refreshSiblingReturnSites(callee, retNode);
        return;
    }

    // Indirect call: use Andersen's call graph to get all resolved callees.
    const RetICFGNode* retNode = callNode->getRetICFGNode();
    bool analyzedCallee = false;
    std::vector<const FunObjVar*> analyzedCallees;
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
            analyzedCallees.push_back(callee);
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
            updateInterval(result, unknownInterval(result), callNode);
    }
    // Resume return node from caller's state (context-insensitive)
    copyAbstractState(callNode, retNode);
    if (analyzedCallee && mergeStatesFromPredecessors(retNode))
        handleICFGNode(retNode);
    for (const FunObjVar* callee : analyzedCallees)
        refreshSiblingReturnSites(callee, retNode);
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
        if (const auto* ret = SVFUtil::dyn_cast<RetCFGEdge>(edge))
            applyReturnEdgeTransfer(source, ret);
        if (conditional && conditional->getCondition())
        {
            assumeBranch(conditional, source);
            collectBranchRefinement(conditional, source);
        }
        applyPhiEdgeTransfer(source, edge, node);
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

void AbstractInterpretation::applyReturnEdgeTransfer(
    State& source, const RetCFGEdge* edge) const
{
    if (!edge || !edge->getRetPE() || source.isBottom())
        return;
    const RetPE* binding = edge->getRetPE();
    const auto* target = SVFUtil::dyn_cast<ValVar>(binding->getLHSVar());
    const auto* value = SVFUtil::dyn_cast<ValVar>(binding->getRHSVar());
    if (!target || !value || !adapter_.contains(*target) ||
            !adapter_.contains(*value))
        return;

    const SVF::Variable targetVariable = adapter_.variable(*target);
    const SVF::Variable sourceVariable = adapter_.variable(*value);
    const bool numericalMayBeUninitialized =
        source.numericalMayBeUninitialized(sourceVariable);
    source.assignValueFrom(targetVariable, source, sourceVariable);
    if (!numericalMayBeUninitialized)
        source.numerical().assign(
                  targetVariable, SVF::LinearExpression(sourceVariable));
}

void AbstractInterpretation::recordMergedReturnSummary(
    const RetICFGNode*)
{
}

// Loop / recursion handling (handleLoopOrRecursion + cycle helpers +
// recursion utilities) lives in AELoopRecursion.cpp.

void AbstractInterpretation::handleSVFStatement(const SVFStmt* stmt)
{
    // Memory-copy annotations synthesize pointer-analysis loads/stores. The
    // byte-aware external model owns their numerical effects; applying both
    // models can havoc memory even for a zero-length copy.
    if (SVFUtil::isa<LoadStmt>(stmt) || SVFUtil::isa<StoreStmt>(stmt))
    {
        const auto* call = SVFUtil::dyn_cast<CallICFGNode>(stmt->getICFGNode());
        const FunObjVar* callee = call ? call->getCalledFunction() : nullptr;
        if (callee && SVFUtil::isExtCall(callee) &&
                ExtAPI::getExtAPI()->hasExtFuncAnnotation(callee, "MEMCPY"))
            return;
    }
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
    const SVF::Interval offset = getGepElementIndex(gep);
    updateAddressSet(
        gep->getLHSVar(),
        getGepObjAddrs(SVFUtil::cast<ValVar>(gep->getRHSVar()), offset, node),
        node);
}

void AbstractInterpretation::updateStateOnSelect(const SelectStmt* select)
{
    const ICFGNode* node = select->getICFGNode();
    const SVF::Interval condition = getInterval(select->getCondition(), node);
    SVF::Interval interval;
    SVF::AddressSet addresses;
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

    return;
}

/// Phis of one basic block, possibly split across sequential ICFG nodes.
static std::vector<const PhiStmt*> blockPhiStatements(const SVFBasicBlock* block)
{
    std::vector<const PhiStmt*> phis;
    for (const ICFGNode* node : block->getICFGNodeList())
        for (const SVFStmt* statement : node->getSVFStmts())
            if (const auto* phi = SVFUtil::dyn_cast<PhiStmt>(statement))
                phis.push_back(phi);
    return phis;
}

/// Integer constants have no adapter coordinate (SVFIRAdapter skips constant
/// data); as phi operands they contribute their value.
static bool isNumericConstantOperand(const ValVar* value)
{
    return value && !value->isPointer() &&
           value->isConstDataOrAggDataButNotNullPtr();
}

bool AbstractInterpretation::phiEdgeBlock(const SVFBasicBlock* block)
{
    if (!block || Options::AESparsity() != AbstractInterpretation::Dense)
        return false;
    const auto cached = phiEdgeBlocks_.find(block);
    if (cached != phiEdgeBlocks_.end())
        return cached->second;
    const std::vector<const PhiStmt*> phis = blockPhiStatements(block);
    bool edgeTransfer = !phis.empty() && !block->getICFGNodeList().empty();
    const ICFGNode* first =
        edgeTransfer ? block->getICFGNodeList().front() : nullptr;
    std::set<const ICFGNode*> predecessors;
    if (edgeTransfer)
        for (const ICFGEdge* edge : first->getInEdges())
        {
            edgeTransfer = edgeTransfer && SVFUtil::isa<IntraCFGEdge>(edge);
            predecessors.insert(edge->getSrcNode());
        }
    for (const PhiStmt* phi : phis)
    {
        if (!edgeTransfer)
            break;
        // The function-exit return phi keeps its undefined-return handling.
        const auto* target = SVFUtil::dyn_cast<ValVar>(phi->getRes());
        edgeTransfer = target && adapter_.contains(*target) &&
                       !phi->isFunctionRetPhi();
        for (u32_t i = 0; edgeTransfer && i < phi->getOpVarNum(); ++i)
        {
            const auto* source = SVFUtil::dyn_cast<ValVar>(phi->getOpVar(i));
            edgeTransfer = source && predecessors.count(phi->getOpICFGNode(i)) &&
                           (adapter_.contains(*source) ||
                            isNumericConstantOperand(source));
        }
        for (const ICFGNode* predecessor : predecessors)
        {
            unsigned matches = 0;
            for (u32_t i = 0; i < phi->getOpVarNum(); ++i)
                matches += phi->getOpICFGNode(i) == predecessor;
            edgeTransfer = edgeTransfer && matches == 1;
        }
    }
    phiEdgeBlocks_.emplace(block, edgeTransfer);
    return edgeTransfer;
}

void AbstractInterpretation::applyPhiEdgeTransfer(
    State& state, const ICFGEdge* edge, const ICFGNode* target)
{
    const SVFBasicBlock* block = target ? target->getBB() : nullptr;
    if (!block || block->getICFGNodeList().empty() ||
            block->getICFGNodeList().front() != target || state.isBottom() ||
            !phiEdgeBlock(block))
        return;
    const ICFGNode* predecessor = edge->getSrcNode();
    // Every operand is read from the pre-edge state, so a phi that uses
    // another phi of the same block sees its previous value (a, b = b, a).
    const State before = state;
    SVF::LinearAssignmentList assignments;
    std::vector<std::pair<SVF::Variable, SVF::Interval>> unary;
    std::vector<SVF::Variable> mayBeUninitialized;
    std::vector<std::pair<SVF::Variable, SVF::Variable>> copies;
    for (const PhiStmt* phi : blockPhiStatements(block))
    {
        const ValVar* source = nullptr;
        for (u32_t i = 0; i < phi->getOpVarNum(); ++i)
            if (phi->getOpICFGNode(i) == predecessor)
                source = SVFUtil::dyn_cast<ValVar>(phi->getOpVar(i));
        const SVF::Variable targetVariable =
            adapter_.variable(*SVFUtil::cast<ValVar>(phi->getRes()));
        if (!adapter_.contains(*source))
        {
            const SVF::Interval value = getInterval(source, predecessor);
            if (value.isSingleton())
                assignments.push_back(
            {targetVariable, SVF::LinearExpression(value.singletonValue())});
            else
                unary.emplace_back(targetVariable, value);
            continue;
        }
        const SVF::Variable sourceVariable = adapter_.variable(*source);
        // Addresses, absent payloads and uninitialized-only values are copied
        // as whole coordinates; numerical values take part in the parallel
        // relational assignment.
        if (phi->getRes()->isPointer() || source->isPointer() ||
                before.interval(sourceVariable).isBottom() ||
                !before.addressSet(sourceVariable).isBottom())
        {
            copies.emplace_back(targetVariable, sourceVariable);
            continue;
        }
        assignments.push_back({targetVariable, SVF::LinearExpression(sourceVariable)});
        if (before.numericalMayBeUninitialized(sourceVariable))
            mayBeUninitialized.push_back(targetVariable);
    }
    if (!assignments.empty())
        state.assignNumericParallel(assignments);
    for (const auto& [variable, value] : unary)
        state.setInterval(variable, value);
    for (SVF::Variable variable : mayBeUninitialized)
        state.addUninitializedNumericalAlternative(variable);
    for (const auto& [variable, source] : copies)
        state.assignValueFrom(variable, before, source);
}

void AbstractInterpretation::updateStateOnPhi(const PhiStmt* phi)
{
    const ICFGNode* icfgNode = phi->getICFGNode();
    // D-009: the incoming edges already executed this block's phis.
    if (phiEdgeBlock(icfgNode->getBB()))
        return;
    const FunExitICFGNode* exit = icfgNode->getFun()
                                  ? icfg->getFunExitICFGNode(icfgNode->getFun())
                                  : nullptr;
    const bool isFormalReturn = exit && exit->getFormalRet() == phi->getRes();
    SVF::Interval interval = SVF::Interval::bottom();
    SVF::AddressSet addresses = SVF::AddressSet::bottom();
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
                const SVF::Interval operandInterval =
                    isFormalReturn
                    ? getDefinedInterval(phi->getOpVar(i), opICFGNode)
                    : getInterval(phi->getOpVar(i), opICFGNode);
                interval.joinWith(operandInterval);

                addresses.joinWith(getAddressSet(phi->getOpVar(i),
                                                 opICFGNode));

            }
        }
    }

    // Phi operands are read from their predecessor program points rather than
    // solely from the immediate incoming state. Preserve the preceding
    // iterate when a later predecessor has grown; otherwise a phi split across
    // sequential ICFG nodes can remain one iteration behind the backedge and
    // violate its transfer equation. Widening here treats every split phi as
    // part of the enclosing loop header and guarantees termination even when
    // it is not the WTO component's first ICFG node.

    updateValue(phi->getRes(), interval, addresses, icfgNode);

}

/// Handle CallPE: phi-like merging of actual parameters from all call sites
/// into the formal parameter at FunEntryICFGNode (e.g., formal =
/// join(actual1@cs1, actual2@cs2, ...))
void AbstractInterpretation::updateStateOnCall(const CallPE* callPE)
{
    const ICFGNode* node = callPE->getICFGNode();
    const SVFVar* res = callPE->getRes();
    SVF::Interval interval = SVF::Interval::bottom();
    SVF::AddressSet addresses = SVF::AddressSet::bottom();
    for (u32_t i = 0; i < callPE->getOpVarNum(); i++)
    {
        const ICFGNode* opICFGNode = callPE->getOpCallICFGNode(i);
        if (hasAbsState(opICFGNode))
        {
            interval.joinWith(getInterval(callPE->getOpVar(i), opICFGNode));
            addresses.joinWith(getAddressSet(callPE->getOpVar(i),
                                             opICFGNode));

        }
    }
    updateValue(res, interval, addresses, node);

}

void AbstractInterpretation::updateStateOnRet(const RetPE* retPE)
{
    const ICFGNode* node = retPE->getICFGNode();
    updateValue(retPE->getLHSVar(),
                getDefinedInterval(retPE->getRHSVar(), node),
                getAddressSet(retPE->getRHSVar(), node), node);
    return;
}

void AbstractInterpretation::updateStateOnAddr(const AddrStmt* addr)
{
    const ICFGNode* node = addr->getICFGNode();
    const auto* object = SVFUtil::cast<ObjVar>(addr->getRHSVar());
    SVF::Interval interval = SVF::Interval::bottom();
    SVF::AddressSet addresses = SVF::AddressSet::bottom();
    initializeObjectValue(object, interval, addresses, node);
    if (addr->getRHSVar()->getType()->getKind() == SVFType::SVFIntegerTy)
        interval.meetWith(
            utils->getRangeLimitFromType(addr->getRHSVar()->getType()));
    updateValue(addr->getLHSVar(), interval, addresses, node);
    assignAllocationSize(addr);
}

void AbstractInterpretation::updateStateOnBinary(const BinaryOPStmt* binary)
{
    const ICFGNode* node = binary->getICFGNode();
    // Treat any unexpected bottom operand as unconstrained for soundness.
    SVF::Interval lhs = getInterval(binary->getOpVar(0), node);
    SVF::Interval rhs = getInterval(binary->getOpVar(1), node);
    if (lhs.isBottom())
        lhs = SVF::Interval::top();
    if (rhs.isBottom())
        rhs = SVF::Interval::top();
    SVF::Interval result;
    switch (binary->getOpcode())
    {
    case BinaryOPStmt::Add:
    case BinaryOPStmt::FAdd:
        result = SVF::add(lhs, rhs);
        break;
    case BinaryOPStmt::Sub:
    case BinaryOPStmt::FSub:
        result = SVF::subtract(lhs, rhs);
        break;
    case BinaryOPStmt::Mul:
    case BinaryOPStmt::FMul:
        result = SVF::multiply(lhs, rhs);
        break;
    case BinaryOPStmt::SDiv:
    case BinaryOPStmt::FDiv:
    case BinaryOPStmt::UDiv:
        result = SVF::divide(lhs, rhs);
        break;
    case BinaryOPStmt::SRem:
    case BinaryOPStmt::FRem:
    case BinaryOPStmt::URem:
        result = SVF::remainder(lhs, rhs);
        break;
    case BinaryOPStmt::Xor:
        result = SVF::bitwiseXor(lhs, rhs);
        break;
    case BinaryOPStmt::And:
        result = SVF::bitwiseAnd(lhs, rhs);
        break;
    case BinaryOPStmt::Or:
        result = SVF::bitwiseOr(lhs, rhs);
        break;
    case BinaryOPStmt::AShr:
        result = SVF::shiftRight(lhs, rhs);
        break;
    case BinaryOPStmt::Shl:
        result = SVF::shiftLeft(lhs, rhs);
        break;
    case BinaryOPStmt::LShr:
        result = SVF::shiftRight(lhs, rhs);
        break;
    default:
        assert(false && "undefined binary: ");
    }
    updateInterval(binary->getRes(), result, node);
}

void AbstractInterpretation::updateStateOnCmp(const CmpStmt* cmp)
{
    const ICFGNode* node = cmp->getICFGNode();
    SVF::Interval lhsInterval = getInterval(cmp->getOpVar(0), node);
    SVF::Interval rhsInterval = getInterval(cmp->getOpVar(1), node);
    const SVF::AddressSet lhsAddresses = getAddressSet(cmp->getOpVar(0), node);
    const SVF::AddressSet rhsAddresses = getAddressSet(cmp->getOpVar(1), node);
    const bool addressComparison = cmp->getOpVar(0)->isPointer();
    SVF::Interval result =
        SVF::Interval::closed(SVF::Rational(0), SVF::Rational(1));
    const auto boolean = [](bool value)
    {
        return SVF::Interval::singleton(SVF::Rational(value ? 1 : 0));
    };

    const auto predicate = cmp->getPredicate();
    if (!addressComparison &&
            (lhsInterval.isBottom() || rhsInterval.isBottom()))
    {
        // Original skips this transfer, then reads a never-defined SSA result
        // as Top. Preserve any existing result on subsequent loop iterations.
        if (getInterval(cmp->getRes(), node).isBottom())
            updateInterval(cmp->getRes(), SVF::Interval::top(), node);
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
        const auto containsBlackHole = [&](const SVF::AddressSet& addresses)
        {
            if (!addresses.isFinite())
                return true;
            return std::any_of(addresses.begin(), addresses.end(),
                               [&](SVF::Location location)
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
        // An LLVM comparison remains Boolean even if a sparse operand
        // summary is unavailable.
        switch (predicate)
        {
        case CmpStmt::ICMP_EQ:
        case CmpStmt::FCMP_OEQ:
        case CmpStmt::FCMP_UEQ:
            result = SVF::equalTo(lhsInterval, rhsInterval);
            break;
        case CmpStmt::ICMP_NE:
        case CmpStmt::FCMP_ONE:
        case CmpStmt::FCMP_UNE:
            result = SVF::notEqualTo(lhsInterval, rhsInterval);
            break;
        case CmpStmt::ICMP_UGT:
        case CmpStmt::ICMP_SGT:
        case CmpStmt::FCMP_OGT:
        case CmpStmt::FCMP_UGT:
            result = SVF::greaterThan(lhsInterval, rhsInterval);
            break;
        case CmpStmt::ICMP_UGE:
        case CmpStmt::ICMP_SGE:
        case CmpStmt::FCMP_OGE:
        case CmpStmt::FCMP_UGE:
            result = SVF::greaterEqual(lhsInterval, rhsInterval);
            break;
        case CmpStmt::ICMP_ULT:
        case CmpStmt::ICMP_SLT:
        case CmpStmt::FCMP_OLT:
        case CmpStmt::FCMP_ULT:
            result = SVF::lessThan(lhsInterval, rhsInterval);
            break;
        case CmpStmt::ICMP_ULE:
        case CmpStmt::ICMP_SLE:
        case CmpStmt::FCMP_OLE:
        case CmpStmt::FCMP_ULE:
            result = SVF::lessEqual(lhsInterval, rhsInterval);
            break;
        default:
            assert(false && "undefined numerical compare");
        }

        // Interval projection cannot prove comparisons such as x - y <= c
        // even when the selected relational domain entails them.  Ask the
        // numerical state directly before leaving a Boolean result at [0, 1].
        // Unsigned order predicates are excluded because their modular order
        // is not mathematical integer order.
        if (result == SVF::Interval::closed(SVF::Rational(0), SVF::Rational(1)))
        {
            const State pointLocal = ensureState(node);
            const bool separateScalarCarrier =
                getScalarAbstractState() != nullptr;
            State relationalQuery =
                separateScalarCarrier ? topState() : pointLocal;
            std::vector<SVF::Variable> relationVariables;
            std::set<NodeID> visiting;
            const auto lhs = reconstructLinearExpression(
                                 cmp->getOpVar(0), relationalQuery, node,
                                 relationVariables, kComparisonUnfoldDepth, visiting);
            const auto rhs = reconstructLinearExpression(
                                 cmp->getOpVar(1), relationalQuery, node,
                                 relationVariables, kComparisonUnfoldDepth, visiting);
            if (lhs && rhs)
            {
                materializeRelations(relationalQuery, relationVariables,
                                     node);
                // Semi-sparse coordinate materialization assigns each leaf
                // and would forget a relation already held in the same state.
                // Build definitions on a neutral carrier, then conjoin the
                // point-local branch refinement after materialization.
                if (separateScalarCarrier)
                    relationalQuery.numerical().meetWith(
                                       pointLocal.numerical());
                std::optional<SVF::LinearConstraint> truth;
                std::optional<SVF::LinearConstraint> falsity;
                switch (predicate)
                {
                case CmpStmt::ICMP_EQ:
                    truth = SVF::equal(*lhs, *rhs);
                    falsity = SVF::notEqual(*lhs, *rhs);
                    break;
                case CmpStmt::ICMP_NE:
                    truth = SVF::notEqual(*lhs, *rhs);
                    falsity = SVF::equal(*lhs, *rhs);
                    break;
                case CmpStmt::ICMP_SGT:
                    truth = SVF::greaterThan(*lhs, *rhs);
                    falsity = SVF::lessEqual(*lhs, *rhs);
                    break;
                case CmpStmt::ICMP_SGE:
                    truth = SVF::greaterEqual(*lhs, *rhs);
                    falsity = SVF::lessThan(*lhs, *rhs);
                    break;
                case CmpStmt::ICMP_SLT:
                    truth = SVF::lessThan(*lhs, *rhs);
                    falsity = SVF::greaterEqual(*lhs, *rhs);
                    break;
                case CmpStmt::ICMP_SLE:
                    truth = SVF::lessEqual(*lhs, *rhs);
                    falsity = SVF::greaterThan(*lhs, *rhs);
                    break;
                default:
                    break;
                }

                if (truth && relationalQuery.numerical().entails(*truth) ==
                        SVF::CheckResult::True)
                    result = boolean(true);
                else if (falsity &&
                         relationalQuery.numerical().entails(*falsity) ==
                         SVF::CheckResult::True)
                    result = boolean(false);
            }
        }
    }

    updateInterval(cmp->getRes(), result, node);
}

void AbstractInterpretation::updateStateOnLoad(const LoadStmt* load)
{
    const ICFGNode* node = load->getICFGNode();
    SVF::Interval interval;
    SVF::AddressSet addresses;
    bool numericalMayBeUninitialized = false;
    loadValue(SVFUtil::cast<ValVar>(load->getRHSVar()), interval, addresses,
              numericalMayBeUninitialized, node);

    updateValue(load->getLHSVar(), interval, addresses, node);
    if (numericalMayBeUninitialized)
        addUninitializedNumericalAlternative(load->getLHSVar(), node);

}

void AbstractInterpretation::updateStateOnStore(const StoreStmt* store)
{
    const ICFGNode* node = store->getICFGNode();

    storeValue(SVFUtil::cast<ValVar>(store->getLHSVar()),
               getInterval(store->getRHSVar(), node),
               getAddressSet(store->getRHSVar(), node), node);

}

void AbstractInterpretation::updateStateOnCopy(const CopyStmt* copy)
{
    const ICFGNode* node = copy->getICFGNode();
    const SVFVar* lhsVar = copy->getLHSVar();
    const SVFVar* rhsVar = copy->getRHSVar();

    const SVF::Interval rhsInterval = getInterval(rhsVar, node);
    const SVF::AddressSet rhsAddresses = getAddressSet(rhsVar, node);

    // D-005: the source is a machine value of its own type, so clip it to
    // that range before deciding whether sign extension is value-preserving.
    if (copy->getCopyKind() == CopyStmt::COPYVAL)
    {
        updateValue(lhsVar, rhsInterval, rhsAddresses, node);
    }
    else if (copy->getCopyKind() == CopyStmt::ZEXT)
    {
        const auto* sourceType =
            SVFUtil::dyn_cast<SVFIntegerType>(rhsVar->getType());
        updateInterval(lhsVar,
                       sourceType ? SVF::zeroExtendIntegerInterval(
                           rhsInterval,
                           rhsVar->getType()->getByteSize() * 8,
                           sourceType->isSigned())
                       : SVF::Interval::top(),
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
            SVF::floatToInteger(rhsInterval,
                                lhsVar->getType()->getByteSize() * 8, true),
            node);
    }
    else if (copy->getCopyKind() == CopyStmt::FPTOUI)
    {
        updateInterval(
            lhsVar,
            SVF::floatToInteger(rhsInterval,
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
            ? SVF::wrapIntegerInterval(rhsInterval,
                                       lhsVar->getType()->getByteSize() * 8,
                                       destinationType->isSigned())
            : SVF::Interval::top(),
            node);
    }
    else if (copy->getCopyKind() == CopyStmt::FPTRUNC)
    {
        updateInterval(lhsVar, rhsInterval, node);
    }
    else if (copy->getCopyKind() == CopyStmt::INTTOPTR)
    {
        // Without a provenance-preserving integer/pointer representation,
        // the cast can designate any modeled object (including a pointer
        // round trip). Empty would silently discard a subsequent store.
        updateValue(lhsVar, SVF::Interval::top(),
                    rhsInterval.isZero()
                    ? SVF::AddressSet::singleton(SVF::Location::null())
                    : SVF::AddressSet::top(), node);
    }
    else if (copy->getCopyKind() == CopyStmt::PTRTOINT)
    {
        updateInterval(lhsVar, SVF::Interval::top(), node);
    }
    else if (copy->getCopyKind() == CopyStmt::BITCAST)
    {
        if (!rhsAddresses.isBottom())
            updateValue(lhsVar, rhsInterval, rhsAddresses, node);
    }
    else
        assert(false && "undefined copy kind");
}
