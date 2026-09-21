//===- AEPostCheck.cpp -- Dense transfer-equation validation -----------===//

#include "AE/Svfexe/AbstractInterpretation.h"

#include "AE/Svfexe/AbsExtAPI.h"
#include "Util/Options.h"

#include <algorithm>
#include <fstream>
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

    if (Options::AESparsity() != AESparsity::Dense)
    {
        addRecord("solver", 0, 0, EquationStatus::Unsupported,
                  "semi-sparse dense-state reconstruction is not implemented",
                  "");
        writeReport(records);
        throw std::runtime_error(
            "AE Post check does not yet support semi-sparse reconstruction");
    }

    const Map<const ICFGNode*, State> finalStates = stateTrace_;
    const Set<const CallICFGNode*> savedCheckpoints = utils->checkpoints;
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

    const ICFGNode* global = icfg->getGlobalICFGNode();
    auto restore = [&]()
    {
        stateTrace_ = finalStates;
        utils->checkpoints = savedCheckpoints;
    };

    const auto replayNode = [&](const ICFGNode* node, const State& incoming)
        -> State
    {
        stateTrace_ = finalStates;
        stateTrace_.insert_or_assign(node, incoming);
        for (const SVFStmt* statement : node->getSVFStmts())
            handleSVFStatement(statement);
        if (const auto* call = SVFUtil::dyn_cast<CallICFGNode>(node))
        {
            if (isExtCall(call))
            {
                if (!isDiagnosticOnlyExternal(call->getCalledFunction()))
                    utils->handleExtAPI(call);
            }
            else if (!call->getCalledFunction())
            {
                bool hasConcreteCallee = false;
                if (callGraph->hasIndCSCallees(call))
                {
                    for (const FunObjVar* callee :
                            callGraph->getIndCSCallees(call))
                        hasConcreteCallee |= !callee->isDeclaration();
                }
                if (!hasConcreteCallee)
                {
                    const SVFVar* result =
                        call->getRetICFGNode()->getActualRet();
                    if (result && getInterval(result, call).isBottom() &&
                            getAddressSet(result, call).isBottom())
                        updateInterval(result, AD::Interval::top(), call);
                }
            }
        }
        finalizeAbstractState(node);
        return state(node);
    };

    try
    {
        const auto globalFinal = finalStates.find(global);
        if (globalFinal == finalStates.end())
        {
            addRecord("initial", 0, global->getId(), EquationStatus::Fail,
                      "global node has no final abstract state", "");
        }
        else
        {
            stateTrace_ = finalStates;
            stateTrace_.insert_or_assign(global, topState());
            for (const SVFStmt* statement : global->getSVFStmts())
                handleSVFStatement(statement);
            if (const auto* blackHole = SVFUtil::dyn_cast<ValVar>(
                    svfir->getGNode(PAG::getPAG()->getBlkPtr())))
                updateValue(blackHole, AD::Interval::top(),
                            blackHoleAddressSet(), global);
            const FunObjVar* root = roots.front();
            const FunEntryICFGNode* rootEntry =
                icfg->getFunEntryICFGNode(root);
            for (const SVFVar* argument : rootEntry->getFormalParms())
                updateInterval(argument, AD::Interval::top(), global);
            const State replayedGlobal = state(global);
            const bool included = replayedGlobal.isSubsetOf(
                                      globalFinal->second) == AD::CheckResult::True;
            addRecord("initial", 0, global->getId(),
                      included ? EquationStatus::Pass : EquationStatus::Fail,
                      included ? "initial state is covered"
                               : "final global state does not cover initialization",
                      "");

            if (included)
            {
                const State replayedEntry = replayNode(rootEntry,
                                                       replayedGlobal);
                const auto entryFinal = finalStates.find(rootEntry);
                const bool entryIncluded = entryFinal != finalStates.end() &&
                    replayedEntry.isSubsetOf(entryFinal->second) ==
                    AD::CheckResult::True;
                addRecord("entry", global->getId(), rootEntry->getId(),
                          entryIncluded ? EquationStatus::Pass
                                        : EquationStatus::Fail,
                          entryIncluded ? "entry transfer is covered"
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
                  [](const ICFGEdge* left, const ICFGEdge* right)
        {
            return std::make_tuple(left->getSrcNode()->getId(),
                                   left->getDstNode()->getId(),
                                   left->getEdgeKind()) <
                   std::make_tuple(right->getSrcNode()->getId(),
                                   right->getDstNode()->getId(),
                                   right->getEdgeKind());
        });

        for (const ICFGEdge* edge : edges)
        {
            const ICFGNode* source = edge->getSrcNode();
            const ICFGNode* target = edge->getDstNode();
            const auto* conditional = SVFUtil::dyn_cast<IntraCFGEdge>(edge);
            bool equation = conditional || SVFUtil::isa<CallCFGEdge>(edge);
            if (SVFUtil::isa<RetCFGEdge>(edge))
            {
                equation = Options::HandleRecur() == TOP;
                if (!equation)
                {
                    const auto* returnSite =
                        SVFUtil::dyn_cast<RetICFGNode>(target);
                    equation = returnSite &&
                        finalStates.count(returnSite->getCallICFGNode()) != 0;
                }
            }
            if (!equation)
                continue;

            const std::string kind = edgeKind(edge);
            std::string discriminator =
                ":edge=" + std::to_string(edge->getEdgeKind());
            if (conditional && conditional->getCondition())
                discriminator += ":condition=" +
                    std::to_string(conditional->getCondition()->getId()) +
                    ":value=" +
                    std::to_string(conditional->getSuccessorCondValue());
            const auto sourceFinal = finalStates.find(source);
            if (sourceFinal == finalStates.end())
            {
                addRecord(kind, source->getId(), target->getId(),
                          EquationStatus::Unreachable,
                          "source has no final abstract state", discriminator);
                continue;
            }
            State incoming = sourceFinal->second;
            if (conditional && conditional->getCondition())
            {
                assumeBranch(conditional, incoming);
                collectBranchRefinement(conditional, incoming);
            }
            if (incoming.isBottom())
            {
                addRecord(kind, source->getId(), target->getId(),
                          EquationStatus::Infeasible,
                          "edge assumption is bottom", discriminator);
                continue;
            }
            const auto targetFinal = finalStates.find(target);
            if (targetFinal == finalStates.end())
            {
                addRecord(kind, source->getId(), target->getId(),
                          EquationStatus::Fail,
                          "feasible edge target has no final abstract state",
                          discriminator);
                continue;
            }
            const State replayed = replayNode(target, incoming);
            const bool included = replayed.isSubsetOf(targetFinal->second) ==
                                  AD::CheckResult::True;
            addRecord(kind, source->getId(), target->getId(),
                      included ? EquationStatus::Pass : EquationStatus::Fail,
                      included ? "replayed transfer is covered"
                               : "replayed transfer is not included in final state",
                      discriminator);
        }
    }
    catch (...)
    {
        restore();
        throw;
    }
    restore();
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
