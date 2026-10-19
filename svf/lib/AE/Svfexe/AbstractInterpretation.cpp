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
#include "AE/Svfexe/OhPackedAbstractInterpretation.h"
#include "AE/Svfexe/VersionedSparseAbstractInterpretation.h"
#include "AE/Svfexe/StatementRelationPacking.h"
#include "AE/Svfexe/ImpactRelationPacking.h"
#include "AE/Svfexe/AllocationSize.h"
#include "AE/Svfexe/SAS16FeatureExtraction.h"
#include "AE/Core/NumericalDomainFactory.h"
#include "AE/Core/PartialRelationalDomain.h"
#include "AE/Core/PackedRelationalDomain.h"
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

static std::vector<const ValVar*> definitionOperands(const SVFStmt* statement)
{
    std::vector<const ValVar*> result;
    const auto add = [&](const SVFVar* variable)
    {
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
              [](const ValVar* left, const ValVar* right)
    {
        return left->getId() < right->getId();
    });
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
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

void AbstractInterpretation::initializeRelationalPolicy()
{
    const u32_t policy = Options::AERelationalPolicy();
    if (policy == ClosurePackedRelational || policy == ImpactPackedRelational || policy == LearnedPackedRelational)
    {
        initializeFixedPacking();
        return;
    }
    if (policy == SyntaxPackedRelational)
    {
        initializeSyntaxPacking();
        return;
    }
    if (policy == WholeRelational)
        return;
    if (Options::AEDomain() == AENumericalDomain::Box)
        throw std::invalid_argument(
            "partial relational policies require octagon or polyhedra");

    std::vector<const ValVar*> seeds;
    const auto addSeed = [&](const SVFVar* variable)
    {
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
            // Branch guards are the numerical proof obligations that decide
            // whether detector checkpoints are reachable. Seed their operands
            // even when the eventual query operand is a pointer (for example
            // an infeasible null-dereference branch).
            if (const auto* compare = SVFUtil::dyn_cast<CmpStmt>(statement))
                for (const ValVar* operand : compare->getOpndVars())
                    addSeed(operand);
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

    const auto byVariable = [&](const ValVar* left, const ValVar* right)
    {
        return left->getId() < right->getId();
    };
    std::sort(seeds.begin(), seeds.end(), byVariable);
    seeds.erase(std::unique(seeds.begin(), seeds.end()), seeds.end());
    relationalSeedCount_ = seeds.size();

    // The query slice is deliberately computed independently from Eva-style
    // expression recognition. This keeps the two policy dimensions
    // measurable instead of silently changing the existing baseline.
    std::deque<const ValVar*> worklist(seeds.begin(), seeds.end());
    std::unordered_set<NodeID> seen;
    std::vector<SVF::Variable> queryClosure;
    while (!worklist.empty())
    {
        const ValVar* value = worklist.front();
        worklist.pop_front();
        if (!seen.insert(value->getId()).second)
            continue;
        if (adapter_.contains(*value))
            queryClosure.push_back(adapter_.variable(*value));

        std::vector<const SVFStmt*> definitions(
            value->getInEdges().begin(), value->getInEdges().end());
        std::sort(definitions.begin(), definitions.end(),
                  [](const SVFStmt* left, const SVFStmt* right)
        {
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

    // Eva's native Octagon creates relations only from recognizable affine
    // shapes. The current SVF carrier still stores a complete Octagon over the
    // selected vocabulary, so these pairs induce candidate variables rather
    // than claiming Eva's sparse-pair carrier or partial saturation.
    std::vector<SVF::Variable> evaCandidates;
    std::unordered_set<NodeID> evaSeen;
    std::size_t recognizedPairs = 0;
    // SVF formal-return ghosts can lose the source integer type and are then
    // conservatively represented as Real by the adapter. Octagons support
    // both Integer and Real coordinates; only pointer and IEEE-float values
    // are outside this policy's affine numerical vocabulary.
    const auto octagonalValue = [&](const ValVar* value)
    {
        return value && adapter_.contains(*value) && !value->isPointer() &&
               adapter_.variable(*value).type().kind !=
               SVF::NumericKind::IEEEFloat;
    };
    const auto addCandidate = [&](const ValVar* value)
    {
        if (octagonalValue(value) && evaSeen.insert(value->getId()).second)
            evaCandidates.push_back(adapter_.variable(*value));
    };
    const auto addPair = [&](const ValVar* left, const ValVar* right)
    {
        const bool leftOctagonal = octagonalValue(left);
        const bool rightOctagonal = octagonalValue(right);
        if (std::getenv("SVF_AE_TRACE_RELATIONAL_POLICY"))
        {
            const auto printValue = [&](const ValVar* value,
                                        bool accepted)
            {
                std::cerr << (value ? value->getId() : 0) << ':'
                          << accepted << ':'
                          << (value && adapter_.contains(*value)) << ':'
                          << (value && value->isPointer()) << ':';
                if (value && adapter_.contains(*value))
                    std::cerr << static_cast<unsigned>(
                                  adapter_.variable(*value).type().kind);
                else
                    std::cerr << '-';
            };
            std::cerr << "AE_RELATIONAL_PAIR=";
            printValue(left, leftOctagonal);
            std::cerr << ',';
            printValue(right, rightOctagonal);
            std::cerr << '\n';
        }
        if (!leftOctagonal || !rightOctagonal || left == right)
            return;
        ++recognizedPairs;
        addCandidate(left);
        addCandidate(right);
    };

    if (policy == EvaStyleRelational || policy == QueryEvaRelational)
    {
        const bool throughCalls =
            Options::AERelationalCallPolicy() == ThroughCalls;
        // Function-return phis and interprocedural edges are not guaranteed to
        // appear in an ICFG node's local statement list. A RetPE is also an
        // outgoing edge of the formal-return ghost, so walking only value
        // definitions misses one endpoint. Collect the canonical statement
        // sets for every recognized kind, then sort by stable edge identity
        // for a deterministic bounded vocabulary.
        std::vector<const SVFStmt*> statements;
        const auto addStatements = [&](SVFStmt::PEDGEK kind)
        {
            const SVFStmt::SVFStmtSetTy& current =
                svfir->getSVFStmtSet(kind);
            statements.insert(statements.end(), current.begin(),
                              current.end());
        };
        addStatements(SVFStmt::BinaryOp);
        addStatements(SVFStmt::Cmp);
        addStatements(SVFStmt::Phi);
        addStatements(SVFStmt::Select);
        addStatements(SVFStmt::Copy);
        if (throughCalls)
        {
            addStatements(SVFStmt::Call);
            addStatements(SVFStmt::Ret);
            addStatements(SVFStmt::ThreadFork);
            addStatements(SVFStmt::ThreadJoin);
        }
        std::sort(statements.begin(), statements.end(),
                  [](const SVFStmt* left, const SVFStmt* right)
        {
            return left->getEdgeID() < right->getEdgeID();
        });
        statements.erase(std::unique(statements.begin(), statements.end()),
                         statements.end());
        for (const SVFStmt* statement : statements)
        {
            if (const auto* binary =
                        SVFUtil::dyn_cast<BinaryOPStmt>(statement))
            {
                if (binary->getOpcode() == BinaryOPStmt::Add ||
                        binary->getOpcode() == BinaryOPStmt::Sub)
                    for (const ValVar* operand : binary->getOpndVars())
                        addPair(binary->getRes(), operand);
            }
            else if (const auto* compare =
                         SVFUtil::dyn_cast<CmpStmt>(statement))
            {
                if (compare->getOpVarNum() == 2)
                    addPair(compare->getOpVar(0), compare->getOpVar(1));
            }
            else if (const auto* phi =
                         SVFUtil::dyn_cast<PhiStmt>(statement))
            {
                for (const ValVar* operand : phi->getOpndVars())
                    addPair(phi->getRes(), operand);
            }
            else if (const auto* select =
                         SVFUtil::dyn_cast<SelectStmt>(statement))
            {
                for (const ValVar* operand : select->getOpndVars())
                    addPair(select->getRes(), operand);
            }
            else if (throughCalls)
            {
                if (const auto* call =
                            SVFUtil::dyn_cast<CallPE>(statement))
                    for (const ValVar* operand : call->getOpndVars())
                        addPair(call->getRes(), operand);
                else if (const auto* ret =
                             SVFUtil::dyn_cast<RetPE>(statement))
                    addPair(ret->getLHSVar(), ret->getRHSVar());
                else if (const auto* copy =
                             SVFUtil::dyn_cast<CopyStmt>(statement))
                    addPair(copy->getLHSVar(), copy->getRHSVar());
            }
            else if (const auto* copy =
                         SVFUtil::dyn_cast<CopyStmt>(statement))
                addPair(copy->getLHSVar(), copy->getRHSVar());
        }
    }

    std::vector<SVF::Variable> selected;
    if (policy == QuerySliceRelational)
        selected = queryClosure;
    else if (policy == EvaStyleRelational)
        selected = evaCandidates;
    else
    {
        const std::set<SVF::Variable> recognized(evaCandidates.begin(),
                evaCandidates.end());
        for (SVF::Variable variable : queryClosure)
            if (recognized.count(variable) != 0)
                selected.push_back(variable);
        // Query operands remain available even when the current IR has no
        // recognizable binary partner. This fallback can only add precision
        // to the global Box state and makes an empty candidate set explicit.
        for (const ValVar* seed : seeds)
            if (adapter_.contains(*seed))
                selected.push_back(adapter_.variable(*seed));
    }

    // Query-slice BFS order prioritizes query seeds. Eva-style order follows
    // deterministic ICFG/statement traversal. Canonicalize only after the cap.
    std::vector<SVF::Variable> uniqueSelected;
    std::set<SVF::Variable> selectedSeen;
    for (SVF::Variable variable : selected)
        if (selectedSeen.insert(variable).second)
            uniqueSelected.push_back(variable);
    const std::size_t limit = Options::AERelationalMaxVars();
    relationalDroppedCount_ = uniqueSelected.size() > limit
                              ? uniqueSelected.size() - limit : 0;
    if (uniqueSelected.size() > limit)
        uniqueSelected.resize(limit);
    std::sort(uniqueSelected.begin(), uniqueSelected.end());
    relationalVocabulary_ =
        std::make_shared<const std::vector<SVF::Variable>>(
            std::move(uniqueSelected));

    const char* policyName = policy == QuerySliceRelational ? "query-slice" :
                             policy == EvaStyleRelational ? "eva-style" :
                             "query-eva";
    const char* callName =
        Options::AERelationalCallPolicy() == ThroughCalls
        ? "through" : "intraprocedural";

    SVFUtil::outs()
            << "AE_RELATIONAL_POLICY policy=" << policyName
            << " calls=" << callName << " seeds="
            << relationalSeedCount_
            << " recognized_pairs=" << recognizedPairs
            << " candidates=" << evaCandidates.size()
            << " selected=" << relationalVocabulary_->size()
            << " dropped=" << relationalDroppedCount_
            << " max=" << Options::AERelationalMaxVars() << '\n';
    if (std::getenv("SVF_AE_TRACE_RELATIONAL_POLICY"))
    {
        const auto printVariables = [&](const char* label,
                                        const std::vector<SVF::Variable>& values)
        {
            std::cerr << label << '=';
            for (SVF::Variable variable : values)
            {
                const ValVar* value = adapter_.value(variable);
                std::cerr << variable.id() << ':'
                          << (value ? value->getId() : 0) << ',';
            }
            std::cerr << '\n';
        };
        printVariables("AE_RELATIONAL_QUERY_CLOSURE", queryClosure);
        printVariables("AE_RELATIONAL_EVA_CANDIDATES", evaCandidates);
        printVariables("AE_RELATIONAL_SELECTED", *relationalVocabulary_);
    }
}


void AbstractInterpretation::initializeSyntaxPacking()
{
    if (Options::AEDomain() != Octagon)
        throw std::invalid_argument("syntax-pack currently requires native Octagon");
    const std::size_t cap = Options::AEPackMaxVars();
    if (cap == 0) throw std::invalid_argument("ae-pack-max-vars must be positive");
    using Group = std::set<SVF::Variable>;
    std::vector<Group> candidates;
    std::map<const SVFBasicBlock*, Group> blocks;
    std::map<const ICFGNode*, Group> nodeGroups;
    const auto addValue = [&](Group& group, const SVFVar* var)
    {
        const auto* value = SVFUtil::dyn_cast<ValVar>(var);
        if (value && !value->isPointer() && adapter_.contains(*value))
        {
            const auto variable = adapter_.variable(*value);
            if (variable.type().kind != SVF::NumericKind::IEEEFloat) group.insert(variable);
        }
    };
    const auto addObjects = [&](Group& group, const ValVar* pointer)
    {
        for (NodeID id : preAnalysis->getPointerAnalysis()->getPts(pointer->getId()))
            if (const auto* object = SVFUtil::dyn_cast<ObjVar>(svfir->getGNode(id)))
            {
                const auto content = adapter_.contentVariable(*object);
                if (content.type().kind != SVF::NumericKind::IEEEFloat) group.insert(content);
            }
    };
    std::vector<const SVFStmt*> statements;
    for (auto kind :
            {
                SVFStmt::BinaryOp, SVFStmt::Cmp, SVFStmt::Copy,
                SVFStmt::Phi, SVFStmt::Select, SVFStmt::Call,
                SVFStmt::Ret, SVFStmt::Load, SVFStmt::Store
            })
    {
        const auto& current = svfir->getSVFStmtSet(kind);
        statements.insert(statements.end(), current.begin(), current.end());
    }
    std::sort(statements.begin(), statements.end(), [](const SVFStmt* a, const SVFStmt* b)
    {
        return a->getEdgeID() < b->getEdgeID();
    });
    for (const auto* statement : statements)
    {
        if (const auto* binary = SVFUtil::dyn_cast<BinaryOPStmt>(statement))
            if (binary->getOpcode() != BinaryOPStmt::Add && binary->getOpcode() != BinaryOPStmt::Sub &&
                    binary->getOpcode() != BinaryOPStmt::Mul) continue;
        Group group;
        for (const auto* value : definitionOperands(statement)) addValue(group, value);
        if (const auto* multi = SVFUtil::dyn_cast<MultiOpndStmt>(statement)) addValue(group, multi->getRes());
        if (const auto* assignment = SVFUtil::dyn_cast<AssignStmt>(statement)) addValue(group, assignment->getLHSVar());
        if (const auto* load = SVFUtil::dyn_cast<LoadStmt>(statement)) addObjects(group, load->getRHSVar());
        if (const auto* store = SVFUtil::dyn_cast<StoreStmt>(statement)) addObjects(group, store->getLHSVar());
        if (group.empty()) continue;
        candidates.push_back(group); // preserves each expression/call binding independently
        if (statement->getBB()) blocks[statement->getBB()].insert(group.begin(), group.end());
    }
    for (const auto& block : blocks) candidates.push_back(block.second);
    for (auto it=icfg->begin(); it!=icfg->end(); ++it)
        if (it->second->getBB()) nodeGroups[it->second]=blocks[it->second->getBB()];
    // Natural/WTO loop locality, including phi operands and nested cycles.
    std::function<Group(const ICFGWTOComp*)> collect = [&](const ICFGWTOComp* component)
    {
        if (const auto* singleton=SVFUtil::dyn_cast<ICFGSingletonWTO>(component))
            return nodeGroups[singleton->getICFGNode()];
        Group group;
        if (const auto* cycle=SVFUtil::dyn_cast<ICFGCycleWTO>(component))
        {
            group=nodeGroups[cycle->head()->getICFGNode()];
            for (const auto* child:cycle->getWTOComponents())
            {
                auto sub=collect(child);
                group.insert(sub.begin(),sub.end());
            }
            if (!group.empty()) candidates.push_back(group);
        }
        return group;
    };
    for (const auto& function:preAnalysis->getFuncToWTO())
        if (function.second) for (const auto* component:function.second->getWTOComponents()) collect(component);
    std::set<std::vector<SVF::Variable>> unique;
    Group all;
    for (const auto& group:candidates)
    {
        std::vector<SVF::Variable> chunk;
        for (auto v:group)
        {
            all.insert(v);
            chunk.push_back(v);
            if (chunk.size()==cap)
            {
                unique.insert(chunk);
                chunk.clear();
            }
        }
        if (!chunk.empty()) unique.insert(chunk);
    }
    // Remove exact subsets: a larger pack already carries that local relation.
    // This is deterministic and does not transitively merge overlapping packs.
    std::vector<std::vector<SVF::Variable>> result;
    for (const auto& group:unique)
    {
        bool subsumed=false;
        for (const auto& other:unique)
            if (other.size()>group.size() && std::includes(other.begin(),other.end(),group.begin(),group.end()))
            {
                subsumed=true;
                break;
            }
        if (!subsumed) result.push_back(group);
    }
    std::size_t memberships=0,maxSize=0;
    for (const auto& group:result)
    {
        memberships+=group.size();
        maxSize=std::max(maxSize,group.size());
    }
    relationalPacking_=std::make_shared<const std::vector<std::vector<SVF::Variable>>>(std::move(result));
    relationalVocabulary_=std::make_shared<const std::vector<SVF::Variable>>(all.begin(),all.end());
    SVFUtil::outs()<<"AE_SYNTAX_PACK packs="<<relationalPacking_->size()<<" variables="<<all.size()
                   <<" memberships="<<memberships<<" max="<<maxSize<<" cap="<<cap
                   <<" candidates="<<candidates.size()<<" locality=ir-basic-block-wto-loop\n";
    if (std::getenv("SVF_AE_TRACE_RELATIONAL_POLICY"))
        for (std::size_t i=0; i<relationalPacking_->size(); ++i)
        {
            std::cerr << "AE_SYNTAX_PACK_MEMBERS " << i;
            for (auto variable:relationalPacking_->at(i)) std::cerr << ' ' << variable.id();
            std::cerr << '\n';
        }
}

void AbstractInterpretation::initializeFixedPacking()
{
    const bool impact = Options::AERelationalPolicy() == ImpactPackedRelational;
    if (Options::AEDomain() != Octagon)
        throw std::invalid_argument(impact ? "impact-pack requires native Octagon" :
                                    "fixed packing requires native Octagon");
    std::set<SVF::Variable> vocabulary;
    for (auto it = svfir->begin(); it != svfir->end(); ++it)
    {
        if (const auto* value = SVFUtil::dyn_cast<ValVar>(it->second))
            if (!value->isPointer() && adapter_.contains(*value))
            {
                const auto variable = adapter_.variable(*value);
                if (variable.type().kind != SVF::NumericKind::IEEEFloat)
                    vocabulary.insert(variable);
            }
        // Andersen may create field objects after adapter construction. Bind
        // their content identities before freezing V and initializing states.
        if (const auto* object = SVFUtil::dyn_cast<ObjVar>(it->second))
            if (!object->isPointer())
            {
                const auto content = adapter_.contentVariable(*object);
                if (content.type().kind != SVF::NumericKind::IEEEFloat)
                    vocabulary.insert(content);
            }
    }
    // D-005: Sparrow's Size(a) coordinates belong to V for every policy.
    for (const auto& size : allocationSizeModel().all())
        vocabulary.insert(adapter_.sizeVariable(*size.object));
    relationalVocabulary_ = std::make_shared<const std::vector<SVF::Variable>>(
                                vocabulary.begin(), vocabulary.end());
    // B0 control: the same packed executor with every variable in its own
    // pack, separating executor effects from multi-variable relations.
    if (std::getenv("SVF_AE_SINGLETON_PACKS"))
    {
        if (Options::AERelationalPolicy() != ClosurePackedRelational)
            throw std::invalid_argument("SVF_AE_SINGLETON_PACKS requires closure-pack");
        SVF::RelationPacking packs;
        for (auto variable : *relationalVocabulary_)
            packs.push_back({variable});
        relationalPacking_ = std::make_shared<const SVF::RelationPacking>(std::move(packs));
        SVFUtil::outs() << "AE_SINGLETON_PACK packs=" << relationalPacking_->size()
                        << " variables=" << vocabulary.size()
                        << " max=1 edges=0 cap=none control=b0\n";
        return;
    }
    if (Options::AERelationalPolicy() == LearnedPackedRelational)
    {
        using PhaseClock = std::chrono::steady_clock;
        const auto secondsSince = [](PhaseClock::time_point start)
        {
            return std::chrono::duration<double>(PhaseClock::now() - start).count();
        };
        const auto* path = std::getenv("SVF_LEARNED_MODEL");
        if (!path) throw std::invalid_argument("learned-pack requires SVF_LEARNED_MODEL");
        std::ifstream input(path);
        auto model = SVF::PackingDecisionTree::read(input);
        const auto featureStart = PhaseClock::now();
        auto context = extractSAS16Features(*svfir, adapter_, *preAnalysis->getPointerAnalysis(), *relationalVocabulary_,std::getenv("SVF_SAS16_LEGACY_FEATURES")!=nullptr);
        const auto featureSeconds = secondsSince(featureStart);
        const auto schema=context.schema;
        SVF::LearnedRelationPacking learned(std::move(context),std::move(model));
        const auto classifierStart = PhaseClock::now();
        auto result = learned.pack(*relationalVocabulary_);
        const auto classifierSeconds = secondsSince(classifierStart);
        relationalPacking_ = std::make_shared<const SVF::RelationPacking>(std::move(result.packs));
        std::size_t maximum = 0;
        for (const auto& pack : *relationalPacking_) maximum = std::max(maximum, pack.size());
        SVFUtil::outs() << "AE_LEARNED_PACK packs=" << relationalPacking_->size()
                        << " variables=" << vocabulary.size() << " max=" << maximum
                        << " edges=" << result.edges.size() << " pairs=" << result.pairs
                        << " profiles="<<result.profiles<<" profile_evaluations="<<result.profileEvaluations
                        << " exceptional_pairs="<<result.exceptionalPairs
                        << " feature_s=" << featureSeconds << " classifier_s=" << classifierSeconds
                        << " features=30 cap=none impact_executed=0 schema="
                        <<(schema==SVF::PackingFeatureSchema::LegacyV1?"svf-sas16-adaptation-v1":"svf-sas16-paper-features-v2")<<'\n';
        if (std::getenv("SVF_AE_TRACE_RELATIONAL_POLICY"))
            for (std::size_t i = 0; i < relationalPacking_->size(); ++i)
            {
                std::cerr << "AE_LEARNED_PACK_MEMBERS " << i;
                for (auto v : relationalPacking_->at(i)) std::cerr << ' ' << v.id();
                std::cerr << '\n';
            }
        return;
    }
    if (impact)
    {
        std::vector<const ICFGNode*> entries;
        auto functions = collectProgEntryFuns();
        while (!functions.empty()) entries.push_back(icfg->getFunEntryICFGNode(functions.pop()));
        auto result = ImpactRelationPacking(*svfir, adapter_,
                                            *preAnalysis->getPointerAnalysis(), std::move(entries),
                                            &allocationSizeModel()).pack(*relationalVocabulary_);
        relationalPacking_ = std::make_shared<const SVF::RelationPacking>(std::move(result.packs));
        std::size_t maximum = 0, promising = 0, unsupported = 0;
        for (const auto& pack : *relationalPacking_) maximum = std::max(maximum, pack.size());
        for (const auto& query : result.queries)
        {
            promising += query.promising;
            unsupported += !query.supported;
        }
        SVFUtil::outs() << "AE_IMPACT_PACK packs=" << relationalPacking_->size()
                        << " variables=" << vocabulary.size() << " max=" << maximum
                        << " edges=" << result.edges.size() << " queries=" << result.queries.size()
                        << " promising=" << promising << " unsupported=" << unsupported
                        << " transfers=" << result.transfers << " commands=" << result.commands
                        << " phi_fallbacks=" << result.phiFallbacks
                        << " cap=none locality=promising-query-snapshots semantics=sparrow-octimpact-v2\n"
                        << "AE_IMPACT_PREANALYSIS_SECONDS " << result.preanalysisSeconds << '\n'
                        << "AE_IMPACT_EXTRACTION_SECONDS " << result.extractionSeconds << '\n';
        if (std::getenv("SVF_AE_TRACE_RELATIONAL_POLICY"))
        {
            for (auto variable : *relationalVocabulary_)
            {
                const auto* value = adapter_.value(variable);
                const auto* object = adapter_.contentObject(variable);
                const auto* sized = adapter_.sizeObject(variable);
                std::cerr << "AE_IMPACT_VARIABLE id=" << variable.id()
                          << " facet=" << (value ? "value" : object ? "content" : "size")
                          << " svf=" << (value ? value->getId() : object ? object->getId()
                                         : sized->getId()) << '\n';
            }
            for (const auto& query : result.queries)
            {
                std::cerr << "AE_IMPACT_QUERY node=" << query.node << " kind=" << query.kind
                          << " supported=" << query.supported << " promising=" << query.promising << '\n';
                for (const auto& edge : query.snapshot)
                    std::cerr << "AE_IMPACT_SNAPSHOT node=" << query.node
                              << " src=" << edge.first.id() << " dst=" << edge.second.id() << '\n';
            }
            for (const auto& edge : result.edges)
                std::cerr << "AE_IMPACT_EDGE kind=impact lhs=" << edge.first.id()
                          << " rhs=" << edge.second.id() << '\n';
            for (std::size_t i = 0; i < relationalPacking_->size(); ++i)
            {
                std::cerr << "AE_IMPACT_PACK_MEMBERS " << i;
                for (auto variable : relationalPacking_->at(i)) std::cerr << ' ' << variable.id();
                std::cerr << '\n';
            }
        }
        return;
    }
    auto result = StatementRelationPacking(
                      *svfir, adapter_, *preAnalysis->getPointerAnalysis(),
                      &allocationSizeModel()).pack(*relationalVocabulary_);
    relationalPacking_ = std::make_shared<const SVF::RelationPacking>(std::move(result.packs));
    std::size_t maximum = 0;
    for (const auto& pack : *relationalPacking_)
        maximum = std::max(maximum, pack.size());
    SVFUtil::outs() << "AE_CLOSURE_PACK packs=" << relationalPacking_->size()
                    << " variables=" << vocabulary.size() << " max=" << maximum
                    << " edges=" << result.edges.size()
                    << " cap=none locality=statement-points-to-components\n";
    if (std::getenv("SVF_AE_TRACE_RELATIONAL_POLICY"))
    {
        for (auto variable : *relationalVocabulary_)
        {
            const auto* value = adapter_.value(variable);
            const auto* object = adapter_.contentObject(variable);
            const auto* sized = adapter_.sizeObject(variable);
            std::cerr << "AE_CLOSURE_VARIABLE id=" << variable.id()
                      << " facet=" << (value ? "value" : object ? "content" : "size")
                      << " svf=" << (value ? value->getId() : object ? object->getId()
                                     : sized->getId()) << '\n';
        }
        for (const auto& edge : result.edges)
            std::cerr << "AE_CLOSURE_EDGE kind=" << edge.kind
                      << " stmt=" << edge.statement
                      << " lhs=" << edge.variables.first.id()
                      << " rhs=" << edge.variables.second.id() << '\n';
        for (std::size_t i = 0; i < relationalPacking_->size(); ++i)
        {
            std::cerr << "AE_CLOSURE_PACK_MEMBERS " << i;
            for (auto variable : relationalPacking_->at(i))
                std::cerr << ' ' << variable.id();
            std::cerr << '\n';
        }
    }
}

void AbstractInterpretation::runOnModule()
{
    using PhaseClock = std::chrono::steady_clock;
    const auto secondsSince = [](PhaseClock::time_point start)
    {
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

    const auto packingStart = PhaseClock::now();
    initializeRelationalPolicy();
    initializeExecutionPolicy();
    if (std::getenv("SVF_AE_VERIFY_PACKED_EXECUTOR"))
    {
        const auto probe = makeNumericalDomain(false);
        if (!dynamic_cast<const SVF::PackedRelationalDomain*>(probe.get()))
            throw std::logic_error("requested packed executor verification failed");
        SVFUtil::outs() << "AE_PACKING_EXECUTOR packed\n";
    }
    if (Options::AERelationalPolicy() == SyntaxPackedRelational ||
            Options::AERelationalPolicy() == ClosurePackedRelational ||
            Options::AERelationalPolicy() == ImpactPackedRelational ||
            Options::AERelationalPolicy() == LearnedPackedRelational)
        SVFUtil::outs() << "AE_PACKING_SECONDS " << secondsSince(packingStart) << '\n';
    if (std::getenv("SVF_AE_PACK_ONLY"))
    {
        SVFUtil::outs() << "AE_PACK_ONLY complete\n";
        SVFUtil::outs().flush();
        stat->endClk();
        return;
    }

    if (domainStats)
        SVF::NumericalDomain::beginTelemetry();
    const PhaseClock::time_point solveStart = PhaseClock::now();
    // Diagnostic for D-005 item 3: objects whose content coordinate is
    // created during the analysis (outside the frozen V).
    const SVF::Variable frozenFrontier = adapter_.syntheticVariable(SVF::NumericType::integer());
    // Whole Box has no frozen packing vocabulary. Retain upstream's distinct
    // field objects instead of merging runtime fields into a packing summary.
    if (Options::AEDomain() != AENumericalDomain::Box)
        adapter_.freezeRuntimeFields();
    analyse();
    const double solveSeconds = secondsSince(solveStart);
    if (phaseStats && Options::AESparsity() == AbstractInterpretation::Dense)
    {
        std::size_t edgeBlocks = 0;
        for (const auto& [block, edgeTransfer] : phiEdgeBlocks_)
        {
            (void)block;
            edgeBlocks += edgeTransfer;
        }
        SVFUtil::outs() << "AE_PHI_EDGE blocks=" << edgeBlocks
                        << " fallback_blocks=" << phiEdgeFallbackBlocks_ << '\n';
    }
    if (std::getenv("SVF_AE_TRACE_RUNTIME_FIELDS"))
    {
        std::size_t created = 0, fields = 0, stringBases = 0;
        std::map<const BaseObjVar*, std::size_t> bases;
        for (const ObjVar* object : adapter_.objectsRegisteredAfter(frozenFrontier))
        {
            ++created;
            fields += SVFUtil::isa<GepObjVar>(object);
            ++bases[svfir->getBaseObject(object->getId())];
        }
        for (const auto& [base, count] : bases)
        {
            (void)count;
            stringBases += base && base->isConstDataOrConstGlobal();
        }
        SVFUtil::outs() << "AE_RUNTIME_FIELDS summarized=" << adapter_.summarizedFieldCount()
                        << " summaries=" << adapter_.summaryCount()
                        << " created=" << created << " fields=" << fields
                        << " bases=" << bases.size() << " constant_bases=" << stringBases << '\n';
        std::size_t shown = 0;
        for (const auto& [base, count] : bases)
            if (base && shown++ < 8)
                SVFUtil::outs() << "AE_RUNTIME_FIELDS_BASE " << base->getName() << " count=" << count
                                << " const=" << base->isConstDataOrConstGlobal()
                                << " heap=" << base->isHeap() << '\n';
    }
    if (domainStats)
    {
        const SVF::NumericalTelemetry operations =
            SVF::NumericalDomain::endTelemetry();
        std::size_t activeProperties = 0;
        std::size_t totalSupport = 0;
        std::size_t maximumSupport = 0;
        std::size_t components = 0;
        std::size_t maximumComponent = 0;
        const auto measure = [&](const State& property)
        {
            const SVF::NumericalDomain& numerical = property.numerical();
            if (numerical.isBottom())
                return;
            ++activeProperties;
            const std::vector<SVF::Variable> support =
                numerical.supportVariables();
            totalSupport += support.size();
            maximumSupport = std::max(maximumSupport, support.size());
            if (numerical.isDomain<SVF::BoxDomain>())
            {
                components += support.size();
                if (!support.empty())
                    maximumComponent = std::max<std::size_t>(
                                           maximumComponent, 1);
                return;
            }
            std::set<SVF::Variable> remaining(support.begin(), support.end());
            while (!remaining.empty())
            {
                const std::vector<SVF::Variable> component =
                    numerical.relationalClosure({*remaining.begin()});
                ++components;
                maximumComponent = std::max(maximumComponent,
                                            component.size());
                for (SVF::Variable variable : component)
                    remaining.erase(variable);
            }
        };
        for (const auto& [node, property] : stateTrace_)
        {
            (void)property;
            measure(state(node));
        }
        if (const SVF::AbstractDomain* carrier = getScalarAbstractState())
            measure(static_cast<const State&>(*carrier));
        SVFUtil::outs()
                << "AE_DOMAIN_STATS flow_states=" << stateTrace_.size()
                << " active_properties=" << activeProperties
                << " total_support=" << totalSupport
                << " max_support=" << maximumSupport
                << " components=" << components
                << " max_component=" << maximumComponent
                << " closure_calls=" << operations.relationalClosureCalls
                << " closure_exports="
                << operations.relationalClosureConstraintExports
                << " closure_indexed="
                << operations.relationalClosureIndexedQueries
                << " join_calls=" << operations.joinCalls
                << " widen_calls=" << operations.wideningCalls
                << " narrow_calls=" << operations.narrowingCalls << '\n';
        if (Options::AERelationalPolicy() != WholeRelational)
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
    // L4 export runs only after every timed phase has been reported, so it
    // cannot change packing, analysis, query or Post timings.
    if (const char* invariantPath = std::getenv("SVF_AE_L4_STATES"))
    {
        SVFUtil::outs().flush();
        const PhaseClock::time_point invariantStart = PhaseClock::now();
        writeInvariantStates(invariantPath);
        SVFUtil::outs() << "AE_L4_SECONDS " << secondsSince(invariantStart) << '\n';
        SVFUtil::outs().flush();
    }
}

namespace
{
std::string invariantBoundText(const SVF::Bound& bound)
{
    if (bound.isMinusInfinity())
        return "-inf";
    if (bound.isPlusInfinity())
        return "+inf";
    return (bound.isStrict() ? "s" : "") + bound.value().toString();
}

/// Whether a two-variable constraint `e op 0` already follows from the
/// interval projection of the same state.
bool impliedByIntervals(const SVF::LinearConstraint& constraint,
                        const SVF::NumericalDomain& numerical)
{
    const SVF::LinearExpression& expression = constraint.expression();
    bool lowerFinite = true, upperFinite = true;
    SVF::Rational lower = expression.constant(), upper = expression.constant();
    for (const auto& [variable, coefficient] : expression.terms())
    {
        const SVF::Interval range = numerical.bound(variable);
        const SVF::Bound& forUpper = coefficient.sign() > 0 ? range.upper() : range.lower();
        const SVF::Bound& forLower = coefficient.sign() > 0 ? range.lower() : range.upper();
        if (forUpper.isFinite())
            upper += coefficient * forUpper.value();
        else
            upperFinite = false;
        if (forLower.isFinite())
            lower += coefficient * forLower.value();
        else
            lowerFinite = false;
    }
    const SVF::Rational zero(std::int64_t{0});
    switch (constraint.kind())
    {
    case SVF::ConstraintKind::LessEqual:
        return upperFinite && upper <= zero;
    case SVF::ConstraintKind::LessThan:
        return upperFinite && upper < zero;
    case SVF::ConstraintKind::GreaterEqual:
        return lowerFinite && zero <= lower;
    case SVF::ConstraintKind::GreaterThan:
        return lowerFinite && zero < lower;
    case SVF::ConstraintKind::Equal:
        return upperFinite && lowerFinite && upper <= zero && zero <= lower;
    case SVF::ConstraintKind::NotEqual:
        return true;
    }
    return true;
}
} // namespace

void AbstractInterpretation::writeInvariantStates(const std::string& path) const
{
    std::ofstream output(path, std::ios::out | std::ios::trunc);
    if (!output)
        throw std::runtime_error("cannot open AE invariant state file: " + path);
    std::set<NodeID> nodes;
    for (const auto& [key, record] : queryLedger_)
    {
        (void)key;
        nodes.insert(record.icfgNode);
    }
    const auto variableKey = [&](SVF::Variable variable) -> std::string
    {
        if (const ValVar* value = adapter_.value(variable))
            return "v\t" + std::to_string(value->getId()) + "\t-";
        if (const ObjVar* object = adapter_.contentObject(variable))
        {
            if (const auto* field = SVFUtil::dyn_cast<GepObjVar>(object))
                return "f\t" + std::to_string(field->getBaseNode()) + '\t' +
                std::to_string(field->getConstantFieldIdx());
            return "o\t" + std::to_string(object->getId()) + "\t-";
        }
        if (const ObjVar* object = adapter_.sizeObject(variable))
            return "s\t" + std::to_string(object->getId()) + "\t-";
        return "";
    };
    output << "#l4-v2\tN node status support exported unnamed finite_bounds nontrivial related"
           "\tV node facet svf field lower upper\tU facet svf field (fixed vocabulary)\n";
    // The fixed vocabulary V is frozen before analysis; fields created later
    // (for example modelled array bytes) are outside every pack.
    if (relationalVocabulary_)
        for (SVF::Variable variable : *relationalVocabulary_)
        {
            const std::string key = variableKey(variable);
            if (!key.empty())
                output << "U\t" << key << '\n';
        }
    for (NodeID id : nodes)
    {
        const auto found = stateTrace_.find(icfg->getICFGNode(id));
        if (found == stateTrace_.end())
        {
            output << "N\t" << id << "\tmissing\n";
            continue;
        }
        const SVF::NumericalDomain& numerical = found->second.numerical();
        if (numerical.isBottom())
        {
            output << "N\t" << id << "\tbottom\n";
            continue;
        }
        std::size_t support = 0, exported = 0, unnamed = 0, finiteBounds = 0;
        std::ostringstream rows;
        for (SVF::Variable variable : numerical.supportVariables())
        {
            if (variable.type().kind == SVF::NumericKind::IEEEFloat)
                continue;
            ++support;
            const std::string key = variableKey(variable);
            if (key.empty())
            {
                ++unnamed;
                continue;
            }
            const SVF::Interval range = numerical.bound(variable);
            if (range.isTop())
                continue;
            finiteBounds += range.lower().isFinite() + range.upper().isFinite();
            ++exported;
            rows << "V\t" << id << '\t' << key << '\t'
                 << invariantBoundText(range.lower()) << '\t'
                 << invariantBoundText(range.upper()) << '\n';
        }
        std::size_t nontrivial = 0;
        std::set<SVF::Variable> related;
        if (!numerical.isDomain<SVF::BoxDomain>())
            for (const SVF::LinearConstraint& constraint : numerical.toConstraints())
            {
                const auto& terms = constraint.expression().terms();
                if (terms.size() != 2 || impliedByIntervals(constraint, numerical))
                    continue;
                ++nontrivial;
                for (const auto& [variable, coefficient] : terms)
                {
                    (void)coefficient;
                    related.insert(variable);
                }
            }
        output << "N\t" << id << "\tstate\t" << support << '\t' << exported << '\t'
               << unnamed << '\t' << finiteBounds << '\t' << nontrivial << '\t'
               << related.size() << '\n' << rows.str();
    }
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

void AbstractInterpretation::recheckQueriesOnFinalStates(
    Map<const ICFGNode*, State>& finals, const std::string& path)
{
    if (!queryLedgerEnabled())
        return;
    const Map<std::string, QueryRecord> saved = queryLedger_;
    for (auto& [key, record] : queryLedger_)
    {
        (void)key;
        record.outcome = QueryOutcome::Unreachable;
        record.reason = "recheck: no Post final state";
    }
    std::vector<std::unique_ptr<AEDetector>> fresh;
    for (const auto& detector : detectors)
    {
        if (detector->getKind() == AEDetector::BUF_OVERFLOW)
            fresh.push_back(std::make_unique<BufOverflowDetector>());
        else if (detector->getKind() == AEDetector::NULL_DEREF)
            fresh.push_back(std::make_unique<NullptrDerefDetector>());
    }
    std::vector<const ICFGNode*> nodes;
    for (const auto& [node, state] : finals)
    {
        (void)state;
        nodes.push_back(node);
    }
    std::sort(nodes.begin(), nodes.end(),
              [](const ICFGNode* a, const ICFGNode* b)
    {
        return a->getId() < b->getId();
    });
    recheckStates_ = &finals;
    // Pass 1 rebuilds detector side tables (gep offsets) from the final
    // states; pass 2 records the outcomes.
    for (int pass = 0; pass < 2; ++pass)
    {
        if (pass == 1)
            for (auto& [key, record] : queryLedger_)
            {
                (void)key;
                record.outcome = QueryOutcome::Unreachable;
                record.reason = "recheck: no Post final state";
            }
        for (const ICFGNode* node : nodes)
        {
            if (finals.at(node).isBottom())
                continue;
            if (const auto* call = SVFUtil::dyn_cast<CallICFGNode>(node))
                recordReachedAssertion(call);
            for (auto& detector : fresh)
                detector->detect(node);
        }
    }
    recheckStates_ = nullptr;
    writeQueryLedgerTo(path);
    queryLedger_ = saved;
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
        if (Options::AESparsity() == AESparsity::VersionedSparse &&
                (Options::AEDomain() == AENumericalDomain::Box ||
                 (Options::AERelationalPolicy() != WholeRelational &&
                  Options::AERelationalPolicy() != SyntaxPackedRelational)))
            throw std::invalid_argument(
                "-ae-sparsity=d3 requires a relational domain with "
                "-ae-relational-policy=whole or syntax-pack");
        if (Options::AERelationalPolicy() != WholeRelational &&
                Options::AEDomain() == AENumericalDomain::Box)
            throw std::invalid_argument(
                "partial relational policies require octagon or polyhedra");
        switch (Options::AESparsity())
        {
        case AESparsity::OhPackedSparse:
            if (Options::AEDomain() != Octagon ||
                    Options::AERelationalPolicy() != SyntaxPackedRelational ||
                    Options::AEPostCheckFile().empty())
                throw std::invalid_argument("oh-packed requires native octagon, syntax-pack, Post checking");
            return new OhPackedAbstractInterpretation();
        case AESparsity::SemiSparse:
            return new SemiSparseAbstractInterpretation();
        case AESparsity::Sparse:
            return new FullSparseAbstractInterpretation();
        case AESparsity::VersionedSparse:
            return new VersionedSparseAbstractInterpretation();
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
        if (Options::AEDomain() == AENumericalDomain::Box)
            handleFunction(funEntry, nullptr);
        else
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
        if (!(Options::AEDomain() == Box && unsignedPredicate) &&
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
                                        if (canStrongStore(ptrVal,*object,static_cast<State&>(state)))
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
                                if (canStrongStore(ptrVal,*object,static_cast<State&>(state)))
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

    // Handle SVF statements.  Individual phi transfers establish each
    // target's product facets.  The grouped transfer below additionally
    // preserves relations between targets selected by the same predecessor.
    for (const SVFStmt* stmt : node->getSVFStmts())
    {
        if (returnBindingsApplied && SVFUtil::isa<RetPE>(stmt))
            continue;
        handleSVFStatement(stmt);
    }
    updateStateOnPhiGroupAtNode(node);
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
        applyRelationalCallBoundary(source, edge, node);
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

void AbstractInterpretation::applyRelationalCallBoundary(
    State& source, const ICFGEdge* edge, const ICFGNode* target) const
{
    if (Options::AEDomain() == AENumericalDomain::Box ||
            Options::AERelationalCallPolicy() != IntraproceduralCalls ||
            (!SVFUtil::isa<CallCFGEdge>(edge) &&
             !SVFUtil::isa<RetCFGEdge>(edge)) || source.isBottom())
        return;

    // Eva's intraprocedural mode keeps unary values and memory effects across
    // a call boundary, but not relations inferred in the other function.
    // Snapshot every unary hull before forgetting support so the operation is
    // independent of the concrete numerical-domain representation.
    SVF::NumericalDomain& numerical = source.numerical();
    std::vector<std::pair<SVF::Variable, SVF::Interval>> unary;
    for (SVF::Variable variable : numerical.supportVariables())
        unary.emplace_back(variable, numerical.bound(variable));
    for (const auto& [variable, value] : unary)
    {
        (void)value;
        numerical.forget(variable);
    }
    for (const auto& [variable, value] : unary)
        if (!value.isTop())
            numerical.assignBound(variable, value);

    if (!SVFUtil::isa<RetCFGEdge>(edge))
        return;
    const auto* returnSite = SVFUtil::dyn_cast<RetICFGNode>(target);
    if (!returnSite)
        return;
    const CallICFGNode* call = returnSite->getCallICFGNode();
    if (!call || stateTrace_.count(call) == 0)
        return;

    // Caller SSA values are immutable across the call. Restore their
    // caller-local relations after importing the callee's unary/effect state;
    // object-content coordinates remain excluded because the callee may have
    // modified them.
    State caller = state(call);
    const SVF::Variable contentBegin = adapter_.firstObjectContentVariable();
    std::vector<SVF::Variable> callerScalars;
    for (SVF::Variable variable : caller.numerical().supportVariables())
        if (variable < contentBegin)
            callerScalars.push_back(variable);
    caller.numerical().project(callerScalars);
    numerical.meetWith(caller.numerical());
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
    updateStateOnSelectValue(select);
    const IntraCFGEdge* guard = piGuard(select);
    if (!guard)
        return;
    const std::vector<SVF::LinearConstraint> constraints = piConstraints(select, guard);
    if (constraints.empty())
        return;
    const ICFGNode* node = select->getICFGNode();
    const SVF::Variable target = adapter_.variable(*SVFUtil::cast<ValVar>(select->getRes()));
    if (Options::AESparsity() == AbstractInterpretation::Dense)
    {
        State& denseState = ensureState(node);
        if (!denseState.isBottom())
            for (const auto& constraint : constraints)
                denseState.assume(constraint);
        return;
    }
    // Sparse executors keep SSA values in definition summaries: make the
    // guard part of x.pi's definition (research D-013 item 3).
    const auto* operand = SVFUtil::dyn_cast<ValVar>(select->getTrueValue());
    if (!operand || !adapter_.contains(*operand))
        return;
    State alternative = phiAlternativeState(node);
    if (alternative.numericalMayBeUninitialized(adapter_.variable(*operand)))
        return;
    alternative.assignNumeric(target, SVF::LinearExpression(adapter_.variable(*operand)));
    for (const auto& constraint : constraints)
        alternative.assume(constraint);
    alternative.numerical().project(alternative.numerical().relationalClosure({target}));
    scalarTransferState(node).numerical().meetWith(alternative.numerical());
    recordRelationalSummary(target, alternative, node);
}

const IntraCFGEdge* AbstractInterpretation::piGuard(const SelectStmt* select) const
{
    if (!Options::PiNodes() || select->getTrueValue() != select->getFalseValue())
        return nullptr;
    const SVFBasicBlock* block = select->getICFGNode()->getBB();
    if (!block || block->getICFGNodeList().empty())
        return nullptr;
    const ICFGNode* entry = block->getICFGNodeList().front();
    if (entry->getInEdges().size() != 1)
        return nullptr;
    const auto* edge = SVFUtil::dyn_cast<IntraCFGEdge>(*entry->getInEdges().begin());
    if (!edge || !edge->getCondition())
        return nullptr;
    if (edge->isSwitchEdge())
        return edge->getCondition() == select->getTrueValue() ? edge : nullptr;
    return edge->getCondition() == select->getCondition() ? edge : nullptr;
}

bool AbstractInterpretation::edgeHasPiNodes(const IntraCFGEdge* edge) const
{
    if (!Options::PiNodes() || Options::AESparsity() != AbstractInterpretation::Dense)
        return false;
    const SVFBasicBlock* block = edge->getDstNode()->getBB();
    if (!block)
        return false;
    for (const ICFGNode* node : block->getICFGNodeList())
        for (const SVFStmt* statement : node->getSVFStmts())
            if (const auto* select = SVFUtil::dyn_cast<SelectStmt>(statement))
                if (piGuard(select) == edge)
                    return true;
    return false;
}

std::vector<SVF::LinearConstraint> AbstractInterpretation::piConstraints(
    const SelectStmt* select, const IntraCFGEdge* guard)
{
    std::vector<SVF::LinearConstraint> constraints;
    const auto* target = SVFUtil::dyn_cast<ValVar>(select->getRes());
    if (!target || !adapter_.contains(*target) || target->isPointer())
        return constraints;
    const SVF::LinearExpression pi(adapter_.variable(*target));
    if (guard->isSwitchEdge())
    {
        // Same restrictions as the switch edge refinement in assumeBranch.
        const auto& cases = guard->getSwitchCases();
        if (guard->includesSwitchDefault() || guard->getSwitchWidth() < 32 ||
                guard->getSwitchWidth() > 64 || cases.empty())
            return constraints;
        const auto limits = std::minmax_element(cases.begin(), cases.end());
        constraints.emplace_back(pi - SVF::LinearExpression(SVF::Rational(*limits.first)),
                                 SVF::ConstraintKind::GreaterEqual);
        constraints.emplace_back(pi - SVF::LinearExpression(SVF::Rational(*limits.second)),
                                 SVF::ConstraintKind::LessEqual);
        return constraints;
    }
    const auto* comparison = guard->getCondition()->getInEdges().empty()
                             ? nullptr
                             : SVFUtil::dyn_cast<CmpStmt>(*guard->getCondition()->getInEdges().begin());
    if (!comparison)
        return constraints;
    const SVFVar* operand = select->getTrueValue();
    const bool left = comparison->getOpVar(0) == operand;
    if (!left && comparison->getOpVar(1) != operand)
        return constraints;
    const SVFVar* other = comparison->getOpVar(left ? 1 : 0);
    // D-014 item 6: the other side is a direct operand, a variable or a value.
    std::optional<SVF::LinearExpression> otherExpression;
    const ICFGNode* source = guard->getSrcNode();
    if (const auto* value = SVFUtil::dyn_cast<ValVar>(other);
            value && adapter_.contains(*value) && !value->isPointer())
        otherExpression = SVF::LinearExpression(adapter_.variable(*value));
    else if (const SVF::Interval constant = getInterval(other, source); constant.isSingleton())
        otherExpression = SVF::LinearExpression(constant.singletonValue());
    if (!otherExpression)
        return constraints;
    s32_t predicate = comparison->getPredicate();
    // D-006: an unsigned comparison is refined as the signed one only when
    // both sides are known non-negative.
    const auto unsignedToSigned = [](s32_t p) -> s32_t
    {
        switch (p)
    {
    case CmpStmt::ICMP_ULT:
        return CmpStmt::ICMP_SLT;
    case CmpStmt::ICMP_ULE:
        return CmpStmt::ICMP_SLE;
    case CmpStmt::ICMP_UGT:
        return CmpStmt::ICMP_SGT;
    case CmpStmt::ICMP_UGE:
        return CmpStmt::ICMP_SGE;
    default:
        return -1;
    }
};
if (const s32_t signedPredicate = unsignedToSigned(predicate); signedPredicate >= 0)
    {
        const SVF::Interval self = getInterval(operand, source);
        const SVF::Interval rest = getInterval(other, source);
        const auto nonNegative = [](const SVF::Interval& interval)
        {
            return !interval.isBottom() && interval.lower().isFinite() &&
                   interval.lower().value() >= SVF::Rational(0);
        };
        if (!nonNegative(self) || !nonNegative(rest))
            return constraints;
        predicate = signedPredicate;
    }
    const bool taken = guard->getSuccessorCondValue() != 0;
    std::optional<SVF::ConstraintKind> kind;
    switch (predicate)
    {
    case CmpStmt::ICMP_EQ:
        kind = taken ? SVF::ConstraintKind::Equal : SVF::ConstraintKind::NotEqual;
        break;
    case CmpStmt::ICMP_NE:
        kind = taken ? SVF::ConstraintKind::NotEqual : SVF::ConstraintKind::Equal;
        break;
    case CmpStmt::ICMP_SLT:
        kind = taken ? SVF::ConstraintKind::LessThan : SVF::ConstraintKind::GreaterEqual;
        break;
    case CmpStmt::ICMP_SLE:
        kind = taken ? SVF::ConstraintKind::LessEqual : SVF::ConstraintKind::GreaterThan;
        break;
    case CmpStmt::ICMP_SGT:
        kind = taken ? SVF::ConstraintKind::GreaterThan : SVF::ConstraintKind::LessEqual;
        break;
    case CmpStmt::ICMP_SGE:
        kind = taken ? SVF::ConstraintKind::GreaterEqual : SVF::ConstraintKind::LessThan;
        break;
    default:
        break;
    }
    if (!kind)
        return constraints;
    const SVF::LinearExpression lhs = left ? pi : *otherExpression;
    const SVF::LinearExpression rhs = left ? *otherExpression : pi;
    constraints.emplace_back(lhs - rhs, *kind);
    return constraints;
}

void AbstractInterpretation::updateStateOnSelectValue(const SelectStmt* select)
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

    if (Options::AEDomain() == AENumericalDomain::Box)
        return;
    const auto* target = SVFUtil::dyn_cast<ValVar>(select->getRes());
    if (!target || !adapter_.contains(*target))
        return;
    const SVF::Variable targetVariable = adapter_.variable(*target);
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
        const SVF::Variable sourceVariable = adapter_.variable(*source);
        if (alternative.numericalMayBeUninitialized(sourceVariable))
        {
            relationalSelect.reset();
            break;
        }
        alternative.assignNumeric(targetVariable,
                                  SVF::LinearExpression(sourceVariable));
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
    // SVF_AE_PHI_LEGACY=1 keeps the pre-D-009 per-phi transfer (ablation).
    static const bool legacy = std::getenv("SVF_AE_PHI_LEGACY") != nullptr;
    if (!block || legacy || Options::AESparsity() != AbstractInterpretation::Dense)
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
    if (!phis.empty() && !edgeTransfer)
        ++phiEdgeFallbackBlocks_;
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
    std::optional<State> relationalPhi;
    bool relationalPhiComplete = true;
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
                    if (!target || !source || !adapter_.contains(*target) ||
                            !adapter_.contains(*source))
                    {
                        // A phi is a union of every feasible predecessor.  A
                        // relational summary built from only the representable
                        // operands would under-approximate that union when it
                        // is met back into the flow state.
                        relationalPhiComplete = false;
                        relationalPhi.reset();
                    }
                    else if (relationalPhiComplete)
                    {
                        State alternative = phiAlternativeState(opICFGNode);
                        const SVF::Variable sourceVariable =
                            adapter_.variable(*source);
                        if (alternative.numericalMayBeUninitialized(
                                    sourceVariable))
                        {
                            relationalPhiComplete = false;
                            relationalPhi.reset();
                        }
                        else
                        {
                            const SVF::Variable targetVariable =
                                adapter_.variable(*target);
                            alternative.assignNumeric(
                                targetVariable,
                                SVF::LinearExpression(sourceVariable));
                            recordRelationalDependency(targetVariable,
                                                       sourceVariable);
                            alternative.setAddressSet(
                                targetVariable,
                                alternative.addressSet(sourceVariable));
                            // Keep the selected source until the joined phi
                            // summary is projected.  For a root SSA value
                            // there is no earlier dependency through which
                            // target==source can be reconstructed; forgetting
                            // it here erases max/min relations after the two
                            // guarded alternatives are joined.
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
    if (const auto* result = SVFUtil::dyn_cast<ValVar>(phi->getRes());
            Options::AEDomain() != Box && result)
    {
        const auto history = phiIntervalHistory_.find(phi);
        SVF::Interval previous = history == phiIntervalHistory_.end()
                                 ? SVF::Interval::bottom() : history->second;
        const auto previousBeforeWiden = previous;
        // A non-recursive return joins observations from distinct callers;
        // it is not a loop header. Widening 2 with 3 here spuriously gives
        // [2,+inf]. Keep widening for cyclic/recursive phi propagation.
        if (isFormalReturn && !isRecursiveFun(icfgNode->getFun()))
            previous.joinWith(interval);
        else
            previous.widenWith(interval);
        if (std::getenv("SVF_AE_TRACE_PHI_RELATIONS"))
            SVFUtil::outs() << "AE_PHI_WIDEN target=" << adapter_.variable(*result).id()
                            << " previous=" << previousBeforeWiden.toString()
                            << " incoming=" << interval.toString()
                            << " next=" << previous.toString() << '\n';
        interval = std::move(previous);
        phiIntervalHistory_.insert_or_assign(phi, interval);
        SVF::AddressSet previousAddresses = getAddressSet(result, icfgNode);
        previousAddresses.joinWith(addresses);
        addresses = std::move(previousAddresses);
    }
    updateValue(phi->getRes(), interval, addresses, icfgNode);
    if (relationalPhiComplete && relationalPhi)
    {
        const auto* target = SVFUtil::dyn_cast<ValVar>(phi->getRes());
        if (target && adapter_.contains(*target))
        {
            const SVF::Variable targetVariable = adapter_.variable(*target);
            relationalPhi->numerical().project(
                relationalPhi->numerical().relationalClosure({targetVariable}));
            scalarTransferState(icfgNode).numerical().meetWith(
                relationalPhi->numerical());
            recordRelationalSummary(targetVariable, *relationalPhi, icfgNode);
        }
    }
}

void AbstractInterpretation::updateStateOnPhiGroupAtNode(
    const ICFGNode* node)
{
    if (!node || !node->getBB())
        return;
    const bool containsPhi =
        std::any_of(node->getSVFStmts().begin(), node->getSVFStmts().end(),
                    [](const SVFStmt* statement)
    {
        return SVFUtil::isa<PhiStmt>(statement);
    });
    if (!containsPhi || phiEdgeBlock(node->getBB()))
        return;

    // SVF may split one LLVM phi tuple across several sequential ICFG nodes.
    // Reconstruct the complete basic-block group and apply it once, after the
    // last split phi's independent product transfer.
    std::vector<const PhiStmt*> blockPhis;
    const ICFGNode* lastPhiNode = nullptr;
    for (const ICFGNode* blockNode : node->getBB()->getICFGNodeList())
        for (const SVFStmt* statement : blockNode->getSVFStmts())
            if (const auto* phi = SVFUtil::dyn_cast<PhiStmt>(statement))
            {
                blockPhis.push_back(phi);
                lastPhiNode = blockNode;
            }
    if (lastPhiNode == node)
        updateStateOnPhiGroup(blockPhis);
}

void AbstractInterpretation::updateStateOnPhiGroup(
    const std::vector<const PhiStmt*>& phis)
{
    if (Options::AEDomain() == AENumericalDomain::Box || phis.size() < 2)
        return;

    // Ignore non-numerical members without disabling a valid numerical tuple.
    // Pointer phis keep their ordinary address-set transfer.
    std::vector<const PhiStmt*> numericalPhis;
    for (const PhiStmt* phi : phis)
    {
        const auto* target = phi ? SVFUtil::dyn_cast<ValVar>(phi->getRes())
                             : nullptr;
        bool numerical = target && adapter_.contains(*target) &&
                         !target->isPointer();
        for (u32_t i = 0; numerical && i < phi->getOpVarNum(); ++i)
        {
            const auto* source =
                SVFUtil::dyn_cast<ValVar>(phi->getOpVar(i));
            numerical = source && adapter_.contains(*source) &&
                        !source->isPointer();
        }
        if (numerical)
            numericalPhis.push_back(phi);
    }
    if (numericalPhis.size() < 2)
        return;

    const ICFGNode* node = numericalPhis.back()->getICFGNode();
    const SVFBasicBlock* block = node ? node->getBB() : nullptr;
    if (!node || !block)
        return;

    std::vector<SVF::Variable> targets;
    targets.reserve(numericalPhis.size());
    std::set<SVF::Variable> uniqueTargets;
    for (const PhiStmt* phi : numericalPhis)
    {
        const auto* target = phi && phi->getICFGNode()->getBB() == block
                             ? SVFUtil::dyn_cast<ValVar>(phi->getRes())
                             : nullptr;
        if (!target || !adapter_.contains(*target))
            return;
        const SVF::Variable variable = adapter_.variable(*target);
        if (!uniqueTargets.insert(variable).second)
            return;
        targets.push_back(variable);
    }

    // Every phi in one SSA group must map exactly one operand to each of the
    // same predecessor program points.  If the frontend presents an
    // incomplete/non-canonical group, retain the already-applied independent
    // transfers rather than guessing a correlation.
    std::vector<const ICFGNode*> predecessors;
    for (u32_t i = 0; i < numericalPhis.front()->getOpVarNum(); ++i)
    {
        const ICFGNode* predecessor =
            numericalPhis.front()->getOpICFGNode(i);
        if (!predecessor ||
                std::find(predecessors.begin(), predecessors.end(),
                          predecessor) != predecessors.end())
            return;
        predecessors.push_back(predecessor);
    }
    for (const PhiStmt* phi : numericalPhis)
    {
        if (phi->getOpVarNum() != predecessors.size())
            return;
        for (const ICFGNode* predecessor : predecessors)
        {
            unsigned matches = 0;
            for (u32_t i = 0; i < phi->getOpVarNum(); ++i)
                matches += phi->getOpICFGNode(i) == predecessor;
            if (matches != 1)
                return;
        }
    }

    std::optional<State> joined;
    for (const ICFGNode* predecessor : predecessors)
    {
        if (!hasAbsState(predecessor))
            continue;

        const ICFGEdge* edge = nullptr;
        for (const PhiStmt* phi : numericalPhis)
            if ((edge = icfg->getICFGEdge(
                            predecessor, phi->getICFGNode(), ICFGEdge::IntraCF)))
                break;
        if (edge)
        {
            const auto* intra = SVFUtil::cast<IntraCFGEdge>(edge);
            if (intra->getCondition() &&
                    !isBranchEdgeFeasibleAt(intra, predecessor))
                continue;
        }

        State alternative = phiAlternativeState(predecessor);
        SVF::LinearAssignmentList assignments;
        std::vector<SVF::AddressSet> addresses;
        assignments.reserve(numericalPhis.size());
        addresses.reserve(numericalPhis.size());
        for (std::size_t phiIndex = 0;
                phiIndex < numericalPhis.size(); ++phiIndex)
        {
            const PhiStmt* phi = numericalPhis[phiIndex];
            const ValVar* source = nullptr;
            for (u32_t i = 0; i < phi->getOpVarNum(); ++i)
                if (phi->getOpICFGNode(i) == predecessor)
                {
                    source = SVFUtil::dyn_cast<ValVar>(phi->getOpVar(i));
                    break;
                }
            if (!source || !adapter_.contains(*source))
                return;
            const SVF::Variable sourceVariable = adapter_.variable(*source);
            if (alternative.numericalMayBeUninitialized(sourceVariable))
                return;
            assignments.push_back(
            {targets[phiIndex], SVF::LinearExpression(sourceVariable)});
            addresses.push_back(alternative.addressSet(sourceVariable));
            recordRelationalDependency(targets[phiIndex], sourceVariable);
        }

        alternative.assignNumericParallel(assignments);
        for (std::size_t i = 0; i < targets.size(); ++i)
            alternative.setAddressSet(targets[i], addresses[i]);
        if (!joined)
            joined = std::move(alternative);
        else
            joined->joinWith(alternative);
    }
    if (!joined)
        return;

    const std::vector<SVF::Variable> retained =
        joined->numerical().relationalClosure(targets);
    joined->numerical().project(retained);
    scalarTransferState(node).numerical().meetWith(joined->numerical());

    // Make a grouped relation reconstructible in Semi-Sparse mode.  All
    // targets form one simultaneous SSA definition tuple.  Keeping the tuple
    // together is conservative even when the backend ultimately represents
    // no relation between a particular pair; its projection will simply be
    // Top for that pair.
    for (SVF::Variable target : targets)
    {
        for (SVF::Variable peer : targets)
            if (peer != target)
                recordRelationalTupleDependency(target, peer);
        recordRelationalSummary(target, *joined, node);
    }

    if (std::getenv("SVF_AE_TRACE_PHI_RELATIONS"))
        SVFUtil::outs() << "AE_PHI_GROUP node=" << node->getId()
                        << " targets=" << targets.size() << " state="
                        << joined->numerical().toString() << '\n';
}

/// Handle CallPE: phi-like merging of actual parameters from all call sites
/// into the formal parameter at FunEntryICFGNode (e.g., formal =
/// join(actual1@cs1, actual2@cs2, ...))
void AbstractInterpretation::updateStateOnCall(const CallPE* callPE)
{
    const ICFGNode* node = callPE->getICFGNode();
    const SVFVar* res = callPE->getRes();
    const std::size_t callerCount = std::count_if(
                                        node->getInEdges().begin(), node->getInEdges().end(),
                                        [](const ICFGEdge* incoming)
    {
        return SVFUtil::isa<CallCFGEdge>(incoming);
    });
    const bool sharedContextInsensitiveFormal = callerCount > 1;
    SVF::Interval interval = SVF::Interval::bottom();
    SVF::AddressSet addresses = SVF::AddressSet::bottom();
    std::optional<State> relationalCall;
    for (u32_t i = 0; i < callPE->getOpVarNum(); i++)
    {
        const ICFGNode* opICFGNode = callPE->getOpCallICFGNode(i);
        if (hasAbsState(opICFGNode))
        {
            interval.joinWith(getInterval(callPE->getOpVar(i), opICFGNode));
            addresses.joinWith(getAddressSet(callPE->getOpVar(i),
                                             opICFGNode));
            if (Options::AEDomain() != AENumericalDomain::Box &&
                    Options::AERelationalCallPolicy() == ThroughCalls &&
                    !sharedContextInsensitiveFormal)
            {
                // A single formal shared by multiple call sites denotes the
                // join of distinct calling contexts. Caller-local SSA
                // coordinates do not identify which alternative supplied the
                // current invocation, so retaining formal--caller relations
                // would correlate different calls. Keep only the joined
                // unary value until the analysis has explicit contexts.
                const auto* target = SVFUtil::dyn_cast<ValVar>(res);
                const auto* source =
                    SVFUtil::dyn_cast<ValVar>(callPE->getOpVar(i));
                if (target && source && adapter_.contains(*target) &&
                        adapter_.contains(*source))
                {
                    State alternative = phiAlternativeState(opICFGNode);
                    const SVF::Variable sourceVariable =
                        adapter_.variable(*source);
                    if (!alternative.numericalMayBeUninitialized(
                                sourceVariable))
                    {
                        const SVF::Variable targetVariable =
                            adapter_.variable(*target);
                        alternative.assignNumeric(
                            targetVariable,
                            SVF::LinearExpression(sourceVariable));
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
            const SVF::Variable targetVariable = adapter_.variable(*target);
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
    if (Options::AEDomain() == AENumericalDomain::Box ||
            Options::AERelationalCallPolicy() == IntraproceduralCalls)
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
                                            [](const ICFGEdge* incoming)
            {
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
    const SVF::Variable sourceVariable = adapter_.variable(*source);
    const SVF::Interval sourceInterval = getDefinedInterval(source, node);
    if (!sourceInterval.isBottom())
        assignRelationalValue(target, SVF::LinearExpression(sourceVariable),
                              sourceInterval, getAddressSet(source, node), node);
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
    const auto* integerType =
        SVFUtil::dyn_cast<SVFIntegerType>(binary->getRes()->getType());
    const SVF::Interval typeRange =
        integerType ? utils->getRangeLimitFromType(binary->getRes()->getType())
        : SVF::Interval::top();
    const bool intervalProvesNoIntegerWrap =
        integerType && !result.isBottom() && result.isSubsetOf(typeRange);
    std::optional<SVF::LinearExpression> affine;
    SVF::Interval relationalResult = SVF::Interval::top();

    // Build the affine expression before deciding whether the operation can
    // wrap.  A relational domain can bound x-y exactly even when the unary
    // intervals of x and y are both Top.  Requiring operand-wise interval
    // arithmetic to prove no-wrap first would prevent Octagon/Polyhedra from
    // answering precisely the expressions they are designed to represent.
    if (Options::AEDomain() != AENumericalDomain::Box && integerType)
    {
        const auto expressionFor = [&](const SVFVar* operand)
                                   -> std::optional<SVF::LinearExpression>
        {
            if (const auto* value = SVFUtil::dyn_cast<ValVar>(operand))
            {
                if (adapter_.contains(*value))
                {
                    const SVF::Variable variable = adapter_.variable(*value);
                    if (getDefinedInterval(value, node).isBottom())
                        return std::nullopt;
                    return SVF::LinearExpression(variable);
                }
            }
            const SVF::Interval constant = getInterval(operand, node);
            if (constant.isSingleton())
                return SVF::LinearExpression(constant.singletonValue());
            return std::nullopt;
        };

        const auto lhsExpression = expressionFor(binary->getOpVar(0));
        const auto rhsExpression = expressionFor(binary->getOpVar(1));
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

        // Relational evaluation is normally a precision fallback.  A genuine
        // two-variable octagonal expression is also queried when intervals
        // already prove no-wrap so dense Post replay and semi-sparse transfer
        // agree on exact singleton folding (for example, x-a == 2).
        const bool exactOctagonalCandidate =
            affine && affine->terms().size() == 2;
        if (affine &&
                (!intervalProvesNoIntegerWrap || exactOctagonalCandidate))
        {
            State relationalState = ensureState(node);
            std::vector<SVF::Variable> variables;
            variables.reserve(affine->terms().size());
            for (const auto& [variable, coefficient] : affine->terms())
            {
                (void)coefficient;
                variables.push_back(variable);
            }
            materializeRelations(relationalState, variables, node);
            relationalResult = relationalState.numerical().bound(*affine);
        }
    }

    const bool relationProvesNoIntegerWrap =
        integerType && relationalResult.isSingleton() &&
        relationalResult.isSubsetOf(typeRange);
    const bool noIntegerWrap =
        intervalProvesNoIntegerWrap || relationProvesNoIntegerWrap;
    if (std::getenv("SVF_AE_TRACE_AFFINE_TRANSFER"))
        std::cerr << "AE affine candidate node=" << node->getId()
                  << " lhs=" << lhs.toString() << " rhs=" << rhs.toString()
                  << " result=" << result.toString()
                  << " relational=" << relationalResult.toString()
                  << " range="
                  << utils->getRangeLimitFromType(binary->getRes()->getType())
                  .toString()
                  << '\n';

    // Keep an affine equality when either interval arithmetic or the active
    // relational domain proves that the LLVM integer operation cannot wrap.
    // Floating-point operations retain interval semantics because rounding and
    // NaN behavior are not affine over rationals.
    if (Options::AEDomain() != AENumericalDomain::Box && noIntegerWrap)
    {
        const auto* resultValue =
            SVFUtil::dyn_cast<ValVar>(binary->getRes());
        if (affine && resultValue && adapter_.contains(*resultValue))
        {
            // Octagon cannot directly encode target = x-y because that
            // equality contains three variables.  If the pre-state proves the
            // right-hand side is a singleton, fold it before assignment so the
            // exact result survives in the target coordinate.
            const SVF::LinearExpression assignment =
                relationalResult.isSingleton()
                ? SVF::LinearExpression(relationalResult.singletonValue())
                : *affine;
            const SVF::Interval assignmentInterval =
                relationalResult.isSingleton() ? relationalResult : result;
            assignRelationalValue(resultValue, assignment, assignmentInterval,
                                  SVF::AddressSet::bottom(), node);
            return;
        }
    }
    if (Options::AEDomain() != AENumericalDomain::Box &&
            integerType && !noIntegerWrap)
        result = SVF::wrapIntegerInterval(
                     result, binary->getRes()->getType()->getByteSize() * 8,
                     integerType->isSigned());
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
        // Even when an operand is unconstrained, an LLVM comparison result is
        // still a Boolean.  Keeping [0, 1] here is both sound and important to
        // dense-equation replay: mathematical Top must not leak into the i1
        // result merely because a sparse operand summary is unavailable.
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
                if (std::getenv("SVF_AE_TRACE_PHI_RELATIONS"))
                    SVFUtil::outs()
                            << "AE_CMP_QUERY node=" << node->getId()
                            << " lhs=" << lhs->toString()
                            << " rhs=" << rhs->toString()
                            << " state="
                            << relationalQuery.numerical().toString() << '\n';
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
    if (std::getenv("SVF_AE_TRACE_ADDRESS_TRANSFER") && addressComparison)
        SVFUtil::outs()
                << "AE_ADDRESS_CMP node=" << node->getId()
                << " function="
                << (node->getFun() ? node->getFun()->getName() : "<global>")
                << " predicate=" << predicate
                << " lhs_var=" << cmp->getOpVar(0)->getId()
                << " lhs=" << lhsAddresses.toString()
                << " rhs_var=" << cmp->getOpVar(1)->getId()
                << " rhs=" << rhsAddresses.toString()
                << " result=" << result.toString() << '\n';
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
    if (std::getenv("SVF_AE_TRACE_ADDRESS_TRANSFER") &&
            load->getLHSVar()->isPointer())
        SVFUtil::outs()
                << "AE_ADDRESS_LOAD node=" << node->getId()
                << " function="
                << (node->getFun() ? node->getFun()->getName() : "<global>")
                << " target=" << load->getLHSVar()->getId()
                << " pointer=" << load->getRHSVar()->getId()
                << " pointees="
                << getAddressSet(load->getRHSVar(), node).toString()
                << " result=" << addresses.toString() << '\n';
    updateValue(load->getLHSVar(), interval, addresses, node);
    if (numericalMayBeUninitialized)
        addUninitializedNumericalAlternative(load->getLHSVar(), node);
    if (Options::AEDomain() != AENumericalDomain::Box)
    {
        const SVF::AddressSet pointees = getAddressSet(load->getRHSVar(), node);
        if (pointees.isSingleton())
        {
            const SVF::Location location = *pointees.begin();
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
    if (std::getenv("SVF_AE_TRACE_ADDRESS_TRANSFER") &&
            store->getRHSVar()->isPointer())
        SVFUtil::outs()
                << "AE_ADDRESS_STORE node=" << node->getId()
                << " function="
                << (node->getFun() ? node->getFun()->getName() : "<global>")
                << " pointer=" << store->getLHSVar()->getId()
                << " pointees="
                << getAddressSet(store->getLHSVar(), node).toString()
                << " source=" << store->getRHSVar()->getId()
                << " value="
                << getAddressSet(store->getRHSVar(), node).toString() << '\n';
    storeValue(SVFUtil::cast<ValVar>(store->getLHSVar()),
               getInterval(store->getRHSVar(), node),
               getAddressSet(store->getRHSVar(), node), node);
    if (Options::AEDomain() != AENumericalDomain::Box)
    {
        const SVF::AddressSet pointees = getAddressSet(store->getLHSVar(), node);
        if (pointees.isSingleton())
        {
            const SVF::Location location = *pointees.begin();
            const ObjVar* object = location.isNull() ? nullptr
                                   : objectAt(location);
            if (object && canStrongStore(pointees, *object, state(node)))
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

    const SVF::Interval rhsInterval = getInterval(rhsVar, node);
    const SVF::AddressSet rhsAddresses = getAddressSet(rhsVar, node);

    // D-005: the source is a machine value of its own type, so clip it to
    // that range before deciding whether sign extension is value-preserving.
    SVF::Interval rhsMachine = rhsInterval;
    if (!rhsMachine.isBottom() && SVFUtil::isa<SVFIntegerType>(rhsVar->getType()))
        rhsMachine.meetWith(utils->getRangeLimitFromType(rhsVar->getType()));
    const bool exactAffineCopy =
        Options::AEDomain() != AENumericalDomain::Box &&
        (copy->getCopyKind() == CopyStmt::COPYVAL ||
         copy->getCopyKind() == CopyStmt::SEXT ||
         (copy->getCopyKind() == CopyStmt::ZEXT && rhsInterval.lower().isFinite() &&
          rhsInterval.lower().value() >= SVF::Rational(0))) &&
        SVFUtil::isa<SVFIntegerType>(lhsVar->getType()) &&
        SVFUtil::isa<SVFIntegerType>(rhsVar->getType()) &&
        !rhsMachine.isBottom() &&
        rhsMachine.isSubsetOf(utils->getRangeLimitFromType(lhsVar->getType()));
    if (exactAffineCopy)
    {
        const auto* lhsValue = SVFUtil::dyn_cast<ValVar>(lhsVar);
        const auto* rhsValue = SVFUtil::dyn_cast<ValVar>(rhsVar);
        if (lhsValue && rhsValue && adapter_.contains(*lhsValue) &&
                adapter_.contains(*rhsValue))
        {
            const SVF::Variable source = adapter_.variable(*rhsValue);
            if (!getDefinedInterval(rhsValue, node).isBottom())
            {
                assignRelationalValue(lhsValue, SVF::LinearExpression(source),
                                      rhsInterval,
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
