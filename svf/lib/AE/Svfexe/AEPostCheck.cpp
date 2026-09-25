//===- AEPostCheck.cpp -- Dense transfer-equation validation -----------===//

#include "AE/Svfexe/AbstractInterpretation.h"

#include "AE/Core/NumericalOperationTrace.h"
#include "AE/Svfexe/AbsExtAPI.h"
#include "Util/Options.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <tuple>

namespace SVF
{

namespace AD = AbstractDomain;

namespace
{

enum class EquationStatus
{
    Pass,
    Unreachable,
    Infeasible,
    Unsupported,
    Fail
};

struct EquationRecord
{
    std::string id;
    std::string kind;
    NodeID source = 0;
    NodeID target = 0;
    EquationStatus status = EquationStatus::Pass;
    std::string reason;
};

const char* statusName(EquationStatus status)
{
    switch (status)
    {
    case EquationStatus::Pass:
        return "Pass";
    case EquationStatus::Unreachable:
        return "Unreachable";
    case EquationStatus::Infeasible:
        return "Infeasible";
    case EquationStatus::Unsupported:
        return "Unsupported";
    case EquationStatus::Fail:
        return "Fail";
    }
    return "Fail";
}

std::string escapeField(const std::string& field)
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

const char* edgeKind(const ICFGEdge* edge)
{
    if (SVFUtil::isa<CallCFGEdge>(edge))
        return "call";
    if (SVFUtil::isa<RetCFGEdge>(edge))
        return "return";
    return "intra";
}

bool isDiagnosticOnlyExternal(const FunObjVar* function)
{
    if (!function)
        return false;
    const std::string name = function->getName();
    return name == "svf_assert" || name == "svf_assert_eq" ||
           name == "svf_print" || name == "SAFE_LOAD" ||
           name == "UNSAFE_LOAD" || name == "SAFE_BUFACCESS" ||
           name == "UNSAFE_BUFACCESS";
}

bool isEquationEdge(
    const ICFGEdge* edge,
    const Map<const ICFGNode*, AbstractInterpretation::State>& reachable)
{
    if (SVFUtil::isa<IntraCFGEdge>(edge) || SVFUtil::isa<CallCFGEdge>(edge))
        return true;
    if (!SVFUtil::isa<RetCFGEdge>(edge))
        return false;
    // A context-insensitive callee exit may be reachable through a different
    // call site.  Its other return edges are not feasible unless their own
    // call sites are reachable; treating every return edge as an equation
    // under the TOP recursion policy fabricates caller continuations.
    const auto* returnSite = SVFUtil::dyn_cast<RetICFGNode>(edge->getDstNode());
    return returnSite && reachable.count(returnSite->getCallICFGNode()) != 0;
}

std::set<AD::Variable> definedScalars(const ICFGNode* node,
                                      const SVFIRAdapter& adapter)
{
    std::set<AD::Variable> result;
    const auto add = [&](const SVFVar* variable) {
        if (!variable)
            return;
        const auto* value = SVFUtil::dyn_cast<ValVar>(variable);
        if (value && adapter.contains(*value))
            result.insert(adapter.variable(*value));
    };
    for (const SVFStmt* statement : node->getSVFStmts())
    {
        if (const auto* address = SVFUtil::dyn_cast<AddrStmt>(statement))
            add(address->getLHSVar());
        else if (const auto* binary =
                     SVFUtil::dyn_cast<BinaryOPStmt>(statement))
            add(binary->getRes());
        else if (const auto* compare = SVFUtil::dyn_cast<CmpStmt>(statement))
            add(compare->getRes());
        else if (const auto* load = SVFUtil::dyn_cast<LoadStmt>(statement))
            add(load->getLHSVar());
        else if (const auto* copy = SVFUtil::dyn_cast<CopyStmt>(statement))
            add(copy->getLHSVar());
        else if (const auto* gep = SVFUtil::dyn_cast<GepStmt>(statement))
            add(gep->getLHSVar());
        else if (const auto* select = SVFUtil::dyn_cast<SelectStmt>(statement))
            add(select->getRes());
        else if (const auto* phi = SVFUtil::dyn_cast<PhiStmt>(statement))
            add(phi->getRes());
        else if (const auto* call = SVFUtil::dyn_cast<CallPE>(statement))
            add(call->getRes());
        else if (const auto* ret = SVFUtil::dyn_cast<RetPE>(statement))
            add(ret->getLHSVar());
    }
    // An external model binds the actual return in the call node.  An internal
    // call's result belongs to the RetICFGNode equation; treating it as already
    // available at the call boundary would use a future definition.
    if (const auto* call = SVFUtil::dyn_cast<CallICFGNode>(node))
        if (call->getCalledFunction() &&
            SVFUtil::isExtCall(call->getCalledFunction()))
            add(call->getRetICFGNode()->getActualRet());
    if (const auto* returnSite = SVFUtil::dyn_cast<RetICFGNode>(node))
        add(returnSite->getActualRet());
    return result;
}

Map<const ICFGNode*, std::set<AD::Variable>> computeAvailability(
    ICFG* graph, const SVFIR& svfir, const SVFIRAdapter& adapter,
    const Map<const ICFGNode*, AbstractInterpretation::State>& reachable,
    const std::vector<const FunObjVar*>& roots)
{
    std::set<AD::Variable> universe;
    for (auto iterator = svfir.begin(); iterator != svfir.end(); ++iterator)
    {
        const auto* value = SVFUtil::dyn_cast<ValVar>(iterator->second);
        if (value && adapter.contains(*value))
            universe.insert(adapter.variable(*value));
    }

    std::vector<const ICFGNode*> nodes;
    Map<const ICFGNode*, std::set<AD::Variable>> available;
    for (const auto& [node, state] : reachable)
    {
        (void)state;
        nodes.push_back(node);
        available.emplace(node, universe);
    }
    std::sort(nodes.begin(), nodes.end(),
              [](const ICFGNode* left, const ICFGNode* right) {
                  return left->getId() < right->getId();
              });

    const ICFGNode* global = graph->getGlobalICFGNode();
    std::set<AD::Variable> globalOut = definedScalars(global, adapter);
    for (const FunObjVar* root : roots)
    {
        const FunEntryICFGNode* entry = graph->getFunEntryICFGNode(root);
        for (const SVFVar* argument : entry->getFormalParms())
        {
            const auto* value = SVFUtil::dyn_cast<ValVar>(argument);
            if (value && adapter.contains(*value))
                globalOut.insert(adapter.variable(*value));
        }
    }
    if (available.count(global))
        available[global] = globalOut;

    bool changed = true;
    while (changed)
    {
        changed = false;
        for (const ICFGNode* node : nodes)
        {
            if (node == global)
                continue;
            bool first = true;
            std::set<AD::Variable> incoming;
            for (const ICFGEdge* edge : node->getInEdges())
            {
                const ICFGNode* predecessor = edge->getSrcNode();
                if (!isEquationEdge(edge, reachable) ||
                    reachable.count(predecessor) == 0)
                    continue;
                std::set<AD::Variable> edgeAvailable =
                    available.at(predecessor);
                // RetPE consumes its callee's formal-return ghost on the
                // return edge.  After that edge only the caller frame remains
                // live; the node definitions below add the actual return.
                // Keeping every alternative callee's formal ghost at a shared
                // return site would make each single-edge Post obligation
                // prove facts about the other alternatives.
                if (const auto* ret = SVFUtil::dyn_cast<RetCFGEdge>(edge))
                {
                    edgeAvailable.clear();
                    const auto caller = available.find(ret->getCallSite());
                    if (caller != available.end())
                        edgeAvailable = caller->second;
                }
                if (first)
                {
                    incoming = std::move(edgeAvailable);
                    first = false;
                }
                else
                {
                    std::set<AD::Variable> intersection;
                    std::set_intersection(
                        incoming.begin(), incoming.end(),
                        edgeAvailable.begin(), edgeAvailable.end(),
                        std::inserter(intersection, intersection.end()));
                    incoming = std::move(intersection);
                }
            }
            if (first)
                incoming.clear();
            const std::set<AD::Variable> definitions =
                definedScalars(node, adapter);
            incoming.insert(definitions.begin(), definitions.end());
            if (incoming != available.at(node))
            {
                available[node] = std::move(incoming);
                changed = true;
            }
        }
    }
    return available;
}

void writeReport(const std::vector<EquationRecord>& records)
{
    std::ofstream output(Options::AEPostCheckFile(),
                         std::ios::out | std::ios::trunc);
    if (!output)
        throw std::runtime_error("cannot open AE Post report: " +
                                 Options::AEPostCheckFile());
    output << "equation_id\tinput_id\tequation_kind\tsource\ttarget\t"
              "status\treason\n";
    const std::string input = escapeField(Options::AEQueryInputID());
    for (const EquationRecord& record : records)
    {
        output << escapeField(record.id) << '\t' << input << '\t'
               << record.kind << '\t' << record.source << '\t'
               << record.target << '\t' << statusName(record.status) << '\t'
               << escapeField(record.reason) << '\n';
    }
    if (!output)
        throw std::runtime_error("failed to write AE Post report: " +
                                 Options::AEPostCheckFile());
}

} // namespace

bool AbstractInterpretation::postCheckEnabled() const
{
    return !Options::AEPostCheckFile().empty();
}

AbstractInterpretation::State AbstractInterpretation::reconstructPostState(
    const ICFGNode* node, const std::set<AD::Variable>&)
{
    return state(node);
}

void AbstractInterpretation::normalizePostReplayState(
    State&, const std::set<AD::Variable>&) const
{
}

void AbstractInterpretation::preparePostReplayState(
    State&, const ICFGNode*, const std::set<AD::Variable>&) const
{
}

void AbstractInterpretation::restorePostReplayCallerFrame(
    State&, const RetICFGNode*, const State&,
    const std::set<AD::Variable>&) const
{
}

void AbstractInterpretation::verifyPostFixpoint()
{
    if (!postCheckEnabled())
        return;
    if (Options::AEQueryInputID().empty())
        throw std::invalid_argument(
            "-ae-query-input-id is required with -ae-post-check");

    const std::string input = escapeField(Options::AEQueryInputID());
    std::vector<EquationRecord> records;
    bool failed = false;
    const auto addRecord = [&](const std::string& kind, NodeID source,
                               NodeID target, EquationStatus status,
                               const std::string& reason,
                               const std::string& discriminator)
    {
        EquationRecord record;
        record.kind = kind;
        record.source = source;
        record.target = target;
        record.status = status;
        record.reason = reason;
        record.id = input + ':' + kind + ':' + std::to_string(source) + ':' +
                    std::to_string(target) + discriminator;
        records.push_back(std::move(record));
        failed |= status == EquationStatus::Fail ||
                  status == EquationStatus::Unsupported;
    };
    const auto traceFailure = [&](const std::string& label,
                                  const State& replayed,
                                  const State& finalState) {
        if (!std::getenv("SVF_AE_TRACE_POST_FAILURE"))
            return;
        std::cerr << "AE Post failure " << label << '\n'
                  << "  replayed: " << replayed.toString() << '\n'
                  << "  final:    " << finalState.toString() << '\n'
                  << "  facets: numerical="
                  << AD::toString(replayed.numerical().isSubsetOf(
                                      finalState.numerical()))
                  << " addresses="
                  << AD::toString(replayed.addresses().isSubsetOf(
                                      finalState.addresses()))
                  << " lifetimes="
                  << AD::toString(replayed.lifetimes().isSubsetOf(
                                      finalState.lifetimes()))
                  << " numeric-init="
                  << AD::toString(
                         replayed.numericalInitialization().isSubsetOf(
                             finalState.numericalInitialization()))
                  << " address-init="
                  << AD::toString(replayed.addressInitialization().isSubsetOf(
                                      finalState.addressInitialization()))
                  << '\n';
        for (AD::Variable variable :
             finalState.numerical().supportVariables())
        {
            const AD::Interval replayedBound =
                replayed.numerical().bound(variable);
            const AD::Interval finalBound =
                finalState.numerical().bound(variable);
            if (!replayedBound.isSubsetOf(finalBound))
            {
                std::cerr << "  numerical difference v" << variable.id()
                          << " replayed=" << replayedBound.toString()
                          << " final=" << finalBound.toString();
                if (const ValVar* value = adapter_.value(variable))
                    std::cerr << " svfir=" << value->getId() << ' '
                              << value->toString();
                std::cerr << '\n';
            }
        }
        std::set<AD::Variable> initialized;
        const std::vector<AD::Variable> replayedInitialized =
            replayed.initializedVariables();
        const std::vector<AD::Variable> finalInitialized =
            finalState.initializedVariables();
        initialized.insert(replayedInitialized.begin(),
                           replayedInitialized.end());
        initialized.insert(finalInitialized.begin(), finalInitialized.end());
        for (AD::Variable variable : initialized)
        {
            const auto replayedNumeric =
                replayed.numericalInitialization().value(variable);
            const auto finalNumeric =
                finalState.numericalInitialization().value(variable);
            const auto replayedAddress =
                replayed.addressInitialization().value(variable);
            const auto finalAddress =
                finalState.addressInitialization().value(variable);
            if (replayedNumeric == finalNumeric &&
                    replayedAddress == finalAddress)
                continue;
            std::cerr << "  initialization difference v" << variable.id()
                      << " numerical="
                      << static_cast<unsigned>(replayedNumeric) << "->"
                      << static_cast<unsigned>(finalNumeric) << " address="
                      << static_cast<unsigned>(replayedAddress) << "->"
                      << static_cast<unsigned>(finalAddress);
            if (const ValVar* value = adapter_.value(variable))
                std::cerr << " svfir=" << value->getId() << ' '
                          << value->toString();
            std::cerr << '\n';
        }
    };

    std::vector<const FunObjVar*> roots;
    FIFOWorkList<const FunObjVar*> rootWorklist = collectProgEntryFuns();
    while (!rootWorklist.empty())
        roots.push_back(rootWorklist.pop());
    if (roots.size() != 1)
    {
        addRecord("initial", 0, 0, EquationStatus::Unsupported,
                  "Post replay currently requires exactly one analysis entry",
                  "");
        writeReport(records);
        throw std::runtime_error(
            "AE Post check currently requires exactly one analysis entry");
    }

    const Map<const ICFGNode*, State> storedStates = stateTrace_;
    auto availability =
        computeAvailability(icfg, *svfir, adapter_, storedStates, roots);
    // Phi transfer reads each operand at its annotated predecessor program
    // point. Keep those coordinates observable in the predecessor's Post
    // state even when the ordinary CFG availability recurrence does not carry
    // them into that node's own statements.
    for (auto nodeIterator = icfg->begin();
            Options::AEDomain() == AENumericalDomain::Box &&
            nodeIterator != icfg->end(); ++nodeIterator)
    {
        for (const SVFStmt* statement : nodeIterator->second->getSVFStmts())
        {
            const auto* phi = SVFUtil::dyn_cast<PhiStmt>(statement);
            if (!phi)
                continue;
            for (u32_t index = 0; index < phi->getOpVarNum(); ++index)
            {
                const ICFGNode* operandNode = phi->getOpICFGNode(index);
                const auto point = availability.find(operandNode);
                const auto* operand =
                    SVFUtil::dyn_cast<ValVar>(phi->getOpVar(index));
                if (point != availability.end() && operand &&
                        adapter_.contains(*operand))
                    point->second.insert(adapter_.variable(*operand));
            }
        }
    }
    Map<const ICFGNode*, State> finalStates;
    for (const auto& [node, stored] : storedStates)
    {
        (void)stored;
        finalStates.emplace(node,
                            reconstructPostState(node, availability.at(node)));
    }

    // Replay through a separate dense interpreter. This prevents virtual
    // sparse transfer hooks from mutating the analyzed SSA carrier while the
    // equations are checked. Reuse the exact symbol mapping so reconstructed
    // states and replay states remain lattice-compatible.
    // Post validation replays equations through a separate interpreter, but it
    // is not part of the measured production-domain workload.  Share the live
    // writer so replay states keep the same decorator type as stored states,
    // but suppress its rows and never reopen/truncate the trace path.
    struct TraceResume final
    {
        std::shared_ptr<AD::NumericalOperationTraceWriter> writer;
        ~TraceResume()
        {
            if (writer)
                writer->resume();
        }
    };
    if (numericalOperationTrace_)
        numericalOperationTrace_->suspend();
    TraceResume traceResume{numericalOperationTrace_};
    AbstractInterpretation replay(false);
    replay.numericalOperationTrace_ = numericalOperationTrace_;
    replay.adapter_ = adapter_;
    replay.relationalVocabulary_ = relationalVocabulary_;
    replay.relationalSeedCount_ = relationalSeedCount_;
    replay.relationalDroppedCount_ = relationalDroppedCount_;
    replay.utils = new AbsExtAPI(&replay);
    const ICFGNode* global = icfg->getGlobalICFGNode();

    const auto replayNode = [&](const ICFGNode* node,
                                const State& incoming) -> State {
        replay.stateTrace_ = finalStates;
        replay.stateTrace_.insert_or_assign(node, incoming);
        for (const SVFStmt* statement : node->getSVFStmts())
        {
            if (SVFUtil::isa<RetICFGNode>(node) &&
                    SVFUtil::isa<RetPE>(statement))
                continue;
            replay.handleSVFStatement(statement);
        }
        replay.updateStateOnPhiGroupAtNode(node);
        if (const auto* call = SVFUtil::dyn_cast<CallICFGNode>(node))
        {
            if (replay.isExtCall(call))
            {
                if (!isDiagnosticOnlyExternal(call->getCalledFunction()))
                    replay.utils->handleExtAPI(call);
            }
            else if (Options::HandleRecur() == TOP &&
                     call->getCalledFunction() &&
                     replay.isRecursiveFun(call->getCalledFunction()))
            {
                replay.skipRecursionWithTop(call);
            }
            else if (!call->getCalledFunction())
            {
                bool hasConcreteCallee = false;
                if (replay.callGraph->hasIndCSCallees(call))
                {
                    for (const FunObjVar* callee :
                         replay.callGraph->getIndCSCallees(call))
                        hasConcreteCallee |= !callee->isDeclaration();
                }
                if (!hasConcreteCallee)
                {
                    const SVFVar* result =
                        call->getRetICFGNode()->getActualRet();
                    if (result && replay.getInterval(result, call).isBottom() &&
                        replay.getAddressSet(result, call).isBottom())
                        replay.updateInterval(result, AD::Interval::top(),
                                              call);
                }
            }
        }
        replay.finalizeAbstractState(node);
        State result = replay.state(node);
        normalizePostReplayState(result, availability.at(node));
        return result;
    };

    const auto globalFinal = finalStates.find(global);
    if (globalFinal == finalStates.end())
    {
        addRecord("initial", 0, global->getId(), EquationStatus::Fail,
                  "global node has no final abstract state", "");
    }
    else
    {
        replay.stateTrace_ = finalStates;
        replay.stateTrace_.insert_or_assign(global, replay.topState());
        for (const SVFStmt* statement : global->getSVFStmts())
            replay.handleSVFStatement(statement);
        if (const auto* blackHole = SVFUtil::dyn_cast<ValVar>(
                replay.svfir->getGNode(PAG::getPAG()->getBlkPtr())))
            replay.updateValue(blackHole, AD::Interval::top(),
                               replay.blackHoleAddressSet(), global);
        const FunObjVar* root = roots.front();
        const FunEntryICFGNode* rootEntry =
            replay.icfg->getFunEntryICFGNode(root);
        for (const SVFVar* argument : rootEntry->getFormalParms())
            replay.updateInterval(argument, AD::Interval::top(), global);
        State replayedGlobal = replay.state(global);
        normalizePostReplayState(replayedGlobal, availability.at(global));
        const bool included = replayedGlobal.isSubsetOf(globalFinal->second) ==
                              AD::CheckResult::True;
        if (!included)
            traceFailure("initial", replayedGlobal, globalFinal->second);
        addRecord("initial", 0, global->getId(),
                  included ? EquationStatus::Pass : EquationStatus::Fail,
                  included ? "initial state is covered"
                           : "final global state does not cover initialization",
                  "");

        if (included)
        {
            const State replayedEntry = replayNode(rootEntry, replayedGlobal);
            const auto entryFinal = finalStates.find(rootEntry);
            const bool entryIncluded =
                entryFinal != finalStates.end() &&
                replayedEntry.isSubsetOf(entryFinal->second) ==
                    AD::CheckResult::True;
            if (!entryIncluded && entryFinal != finalStates.end())
                traceFailure("entry", replayedEntry, entryFinal->second);
            addRecord(
                "entry", global->getId(), rootEntry->getId(),
                entryIncluded ? EquationStatus::Pass : EquationStatus::Fail,
                entryIncluded
                    ? "entry transfer is covered"
                    : "final entry state does not cover root initialization",
                "");
        }
    }

    std::vector<const ICFGEdge*> edges;
    for (auto nodeIterator = icfg->begin(); nodeIterator != icfg->end();
         ++nodeIterator)
    {
        for (const ICFGEdge* edge : nodeIterator->second->getOutEdges())
            edges.push_back(edge);
    }
    std::sort(edges.begin(), edges.end(),
              [](const ICFGEdge* left, const ICFGEdge* right) {
                  return std::make_tuple(left->getSrcNode()->getId(),
                                         left->getDstNode()->getId(),
                                         left->getEdgeKind()) <
                         std::make_tuple(right->getSrcNode()->getId(),
                                         right->getDstNode()->getId(),
                                         right->getEdgeKind());
              });

    for (const ICFGEdge* edge : edges)
    {
        if (!isEquationEdge(edge, finalStates))
            continue;
        const ICFGNode* source = edge->getSrcNode();
        const ICFGNode* target = edge->getDstNode();
        const auto* conditional = SVFUtil::dyn_cast<IntraCFGEdge>(edge);
        const std::string kind = edgeKind(edge);
        std::string discriminator =
            ":edge=" + std::to_string(edge->getEdgeKind());
        if (conditional && conditional->getCondition())
            discriminator +=
                ":condition=" +
                std::to_string(conditional->getCondition()->getId()) +
                ":value=" +
                std::to_string(conditional->getSuccessorCondValue());
        // TOP recursion is represented by a conservative summary at the
        // caller.  Its body equations are intentionally absent from this
        // configured analysis and must not be mistaken for missing states.
        const bool summarizedRecursiveBody =
            Options::HandleRecur() == TOP &&
            ((source->getFun() && replay.isRecursiveFun(source->getFun())) ||
             (target->getFun() && replay.isRecursiveFun(target->getFun())));
        if (summarizedRecursiveBody)
        {
            addRecord(
                kind, source->getId(), target->getId(),
                EquationStatus::Unreachable,
                "recursive body is replaced by the configured top summary",
                discriminator);
            continue;
        }
        const auto sourceFinal = finalStates.find(source);
        if (sourceFinal == finalStates.end())
        {
            addRecord(kind, source->getId(), target->getId(),
                      EquationStatus::Unreachable,
                      "source has no final abstract state", discriminator);
            continue;
        }
        State incoming = sourceFinal->second;
        // Dense replay states have the same physical shape as dense solver
        // states. Semi-sparse finalStates are reconstructed with scalar
        // summaries that are absent from the propagated flow carrier; their
        // virtual caller-frame hook below already mirrors the sparse merge.
        if (Options::AESparsity() == AESparsity::Dense)
            replay.applyRelationalCallBoundary(incoming, edge, target);
        const auto targetAvailability = availability.find(target);
        if (targetAvailability != availability.end())
        {
            std::set<AD::Variable> targetInputs =
                targetAvailability->second;
            for (AD::Variable definition : definedScalars(target, adapter_))
                targetInputs.erase(definition);
            preparePostReplayState(incoming, target, targetInputs);
        }
        if (const auto* ret = SVFUtil::dyn_cast<RetCFGEdge>(edge))
        {
            const CallICFGNode* call = ret->getCallSite();
            const auto callerFinal = finalStates.find(call);
            const auto callerAvailability = availability.find(call);
            if (callerFinal != finalStates.end() &&
                    callerAvailability != availability.end())
                restorePostReplayCallerFrame(
                    incoming, SVFUtil::cast<RetICFGNode>(target),
                    callerFinal->second, callerAvailability->second);
            replay.applyReturnEdgeTransfer(incoming, ret);
        }
        if (conditional && conditional->getCondition())
        {
            replay.assumeBranch(conditional, incoming);
            replay.collectBranchRefinement(conditional, incoming);
        }
        if (incoming.isBottom())
        {
            addRecord(kind, source->getId(), target->getId(),
                      EquationStatus::Infeasible, "edge assumption is bottom",
                      discriminator);
            continue;
        }
        const auto targetFinal = finalStates.find(target);
        if (targetFinal == finalStates.end())
        {
            if (std::getenv("SVF_AE_TRACE_POST_FAILURE"))
                std::cerr << "AE Post failure missing target "
                          << source->getId() << ':' << target->getId()
                          << " incoming: " << incoming.toString() << '\n';
            addRecord(kind, source->getId(), target->getId(),
                      EquationStatus::Fail,
                      "feasible edge target has no final abstract state",
                      discriminator);
            continue;
        }
        const State replayed = replayNode(target, incoming);
        const bool included =
            replayed.isSubsetOf(targetFinal->second) == AD::CheckResult::True;
        if (!included)
        {
            if (std::getenv("SVF_AE_TRACE_POST_FAILURE"))
            {
                std::cerr << "AE Post source node: " << source->toString()
                          << "\nAE Post target node: " << target->toString()
                          << '\n';
                for (const SVFStmt* statement : target->getSVFStmts())
                    std::cerr << "AE Post target statement: "
                              << statement->toString() << '\n';
            }
            traceFailure(kind + ":" + std::to_string(source->getId()) + ":" +
                             std::to_string(target->getId()),
                         replayed, targetFinal->second);
        }
        addRecord(kind, source->getId(), target->getId(),
                  included ? EquationStatus::Pass : EquationStatus::Fail,
                  included ? "replayed transfer is covered"
                           : "replayed transfer is not included in final state",
                  discriminator);
    }
    std::sort(records.begin(), records.end(),
              [](const EquationRecord& left, const EquationRecord& right)
    {
        return left.id < right.id;
    });
    writeReport(records);
    if (failed)
        throw std::runtime_error("AE Post check failed; see " +
                                 Options::AEPostCheckFile());
}

} // namespace SVF
