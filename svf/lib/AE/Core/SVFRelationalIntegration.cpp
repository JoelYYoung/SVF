//===- SVFRelationalIntegration.cpp -- AE reduced-product lifecycle -----===//

#include "AE/Svfexe/AbstractInterpretation.h"

#include "SVFIR/SVFIR.h"
#include "Util/Options.h"

#include <memory>
#include <string>
#include <utility>

using namespace SVF;

bool AbstractInterpretation::hasRelationalState(const ICFGNode* node) const
{
    const auto it = relationalTrace.find(node);
    return it != relationalTrace.end() && it->second != nullptr;
}

const SVFRelationalBridge*
AbstractInterpretation::getRelationalState(const ICFGNode* node) const
{
    const auto it = relationalTrace.find(node);
    return it == relationalTrace.end() ? nullptr : it->second.get();
}

void AbstractInterpretation::initializeRelationalEnvironments()
{
    if (!Options::AERelational())
        return;

    relationalDomain = relational::makeOctagonDomain();
    const std::size_t maximum = Options::AERelationalMaxVars();
    for (SVFIR::iterator it = svfir->begin(); it != svfir->end(); ++it)
    {
        const ValVar* value = SVFUtil::dyn_cast<ValVar>(it->second);
        if (!value || value->isPointer() ||
                value->isConstDataOrAggDataButNotNullPtr() ||
                !SVFUtil::isa<SVFIntegerType>(value->getType()))
            continue;

        const FunObjVar* function = value->getFunction();
        std::vector<TrackedRelationalVariable>& variables =
            function ? relationalVariablesByFunction[function]
                     : globalRelationalVariables;
        if (variables.size() >= maximum)
            continue;
        variables.push_back(
            {value->getId(), relational::NumericType::integer(),
             "svf_" + std::to_string(value->getId())});
    }
}

const std::vector<TrackedRelationalVariable>&
AbstractInterpretation::trackedRelationalVariables(
    const ICFGNode* node) const
{
    if (node)
    {
        const FunObjVar* function = node->getFun();
        const auto it = relationalVariablesByFunction.find(function);
        if (it != relationalVariablesByFunction.end())
            return it->second;
    }
    return globalRelationalVariables;
}

AbstractInterpretation::RelationalStatePtr
AbstractInterpretation::makeRelationalTop(const ICFGNode* node) const
{
    if (!Options::AERelational())
        return nullptr;
    return std::make_shared<SVFRelationalBridge>(
        trackedRelationalVariables(node), relationalDomain);
}

AbstractInterpretation::RelationalStatePtr
AbstractInterpretation::snapshotRelationalState(const ICFGNode* node) const
{
    const SVFRelationalBridge* state = getRelationalState(node);
    return state ? std::make_shared<SVFRelationalBridge>(*state) : nullptr;
}

void AbstractInterpretation::initializeRelationalState(const ICFGNode* node)
{
    if (Options::AERelational() && !hasRelationalState(node))
        relationalTrace[node] = makeRelationalTop(node);
}

void AbstractInterpretation::copyRelationalState(
    const ICFGNode* destination, const ICFGNode* source)
{
    if (!Options::AERelational())
        return;

    RelationalStatePtr copy = snapshotRelationalState(source);
    if (!copy)
        copy = makeRelationalTop(destination);
    else
        copy->changeTrackedVariables(
            trackedRelationalVariables(destination));
    relationalTrace[destination] = std::move(copy);
}

void AbstractInterpretation::mergeRelationalFromPredecessors(
    const ICFGNode* node)
{
    if (!Options::AERelational())
        return;

    RelationalStatePtr merged;
    auto mergeSource = [&](const ICFGNode* predecessor,
                           const IntraCFGEdge* conditionalEdge)
    {
        RelationalStatePtr source = snapshotRelationalState(predecessor);
        if (!source)
            source = makeRelationalTop(node);
        else
            source->changeTrackedVariables(trackedRelationalVariables(node));
        if (conditionalEdge)
            assumeRelationalBranch(conditionalEdge, *source);
        if (!merged)
            merged = std::move(source);
        else
            merged->joinWith(*source);
    };

    for (ICFGEdge* edge : node->getInEdges())
    {
        const ICFGNode* predecessor = edge->getSrcNode();
        if (!hasAbsState(predecessor))
            continue;

        if (const IntraCFGEdge* intra =
                SVFUtil::dyn_cast<IntraCFGEdge>(edge))
        {
            if (intra->getCondition())
            {
                AbstractState state = getAbsState(predecessor);
                if (isBranchEdgeFeasible(intra, state))
                    mergeSource(predecessor, intra);
            }
            else
                mergeSource(predecessor, nullptr);
        }
        else if (SVFUtil::isa<CallCFGEdge>(edge))
            mergeSource(predecessor, nullptr);
        else if (SVFUtil::isa<RetCFGEdge>(edge))
        {
            if (Options::HandleRecur() == TOP)
                mergeSource(predecessor, nullptr);
            else
            {
                const RetICFGNode* returnSite =
                    SVFUtil::dyn_cast<RetICFGNode>(node);
                const CallICFGNode* callSite =
                    returnSite ? returnSite->getCallICFGNode() : nullptr;
                if (callSite && hasAbsState(callSite))
                    mergeSource(predecessor, nullptr);
            }
        }
    }

    relationalTrace[node] = merged ? std::move(merged)
                                   : makeRelationalTop(node);
}

bool AbstractInterpretation::appendRelationalOperand(
    const ICFGNode* node, const SVFVar* operand,
    const relational::Rational& multiplier, SVFRelationalBridge& state,
    std::vector<SVFRelationalBridge::AffineTerm>& terms,
    relational::Rational& constant)
{
    if (state.tracks(operand->getId()))
    {
        terms.emplace_back(operand->getId(), multiplier);
        return true;
    }
    if (const ConstIntValVar* integer =
            SVFUtil::dyn_cast<ConstIntValVar>(operand))
    {
        constant += multiplier *
                    relational::Rational(integer->getSExtValue());
        return true;
    }

    const AbstractValue& value = getAbsValue(operand, node);
    if (value.isInterval() && value.getInterval().is_numeral())
    {
        constant += multiplier * relational::Rational(
                                     value.getInterval().getIntNumeral());
        return true;
    }
    return false;
}

void AbstractInterpretation::assumeRelationalBranch(
    const IntraCFGEdge* edge, SVFRelationalBridge& state)
{
    const SVFVar* condition = edge->getCondition();
    if (!condition || condition->getInEdges().empty())
        return;
    const CmpStmt* comparison = SVFUtil::dyn_cast<CmpStmt>(
        *condition->getInEdges().begin());
    if (!comparison || comparison->getOpVarID(0) == IRGraph::NullPtr ||
            comparison->getOpVarID(1) == IRGraph::NullPtr)
        return;

    s32_t predicate = comparison->getPredicate();
    if (edge->getSuccessorCondValue() == 0)
    {
        switch (predicate)
        {
        case CmpStmt::ICMP_EQ:
            predicate = CmpStmt::ICMP_NE;
            break;
        case CmpStmt::ICMP_NE:
            predicate = CmpStmt::ICMP_EQ;
            break;
        case CmpStmt::ICMP_SGT:
            predicate = CmpStmt::ICMP_SLE;
            break;
        case CmpStmt::ICMP_SGE:
            predicate = CmpStmt::ICMP_SLT;
            break;
        case CmpStmt::ICMP_SLT:
            predicate = CmpStmt::ICMP_SGE;
            break;
        case CmpStmt::ICMP_SLE:
            predicate = CmpStmt::ICMP_SGT;
            break;
        default:
            return;
        }
    }

    relational::ConstraintKind kind;
    switch (predicate)
    {
    case CmpStmt::ICMP_EQ:
        kind = relational::ConstraintKind::Equal;
        break;
    case CmpStmt::ICMP_NE:
        kind = relational::ConstraintKind::NotEqual;
        break;
    case CmpStmt::ICMP_SGT:
        kind = relational::ConstraintKind::GreaterThan;
        break;
    case CmpStmt::ICMP_SGE:
        kind = relational::ConstraintKind::GreaterEqual;
        break;
    case CmpStmt::ICMP_SLT:
        kind = relational::ConstraintKind::LessThan;
        break;
    case CmpStmt::ICMP_SLE:
        kind = relational::ConstraintKind::LessEqual;
        break;
    default:
        return;
    }

    std::vector<SVFRelationalBridge::AffineTerm> terms;
    relational::Rational constant;
    const ICFGNode* predecessor = edge->getSrcNode();
    if (!appendRelationalOperand(predecessor, comparison->getOpVar(0),
                                 relational::Rational(1), state, terms,
                                 constant) ||
            !appendRelationalOperand(predecessor,
                                     comparison->getOpVar(1),
                                     relational::Rational(-1), state, terms,
                                     constant))
        return;
    state.assumeAffine(std::move(terms), std::move(constant), kind);
}

void AbstractInterpretation::assignRelationalInterval(
    const ICFGNode* node, const SVFVar* target,
    const IntervalValue& interval)
{
    if (!Options::AERelational())
        return;
    initializeRelationalState(node);
    RelationalStatePtr& state = relationalTrace[node];
    if (state->tracks(target->getId()))
        state->assignInterval(target->getId(), interval);
}

void AbstractInterpretation::synchronizeRelationalWithIntervals(
    const ICFGNode* node)
{
    if (!Options::AERelational())
        return;
    initializeRelationalState(node);
    SVFRelationalBridge& state = *relationalTrace[node];
    for (const TrackedRelationalVariable& tracked :
            trackedRelationalVariables(node))
    {
        state.forget(tracked.id);
        const SVFVar* variable = getSVFVar(tracked.id);
        if (!hasAbsValue(variable, node))
            continue;
        const AbstractValue& value = getAbsValue(variable, node);
        if (value.isInterval())
            state.meetInterval(tracked.id, value.getInterval());
    }
}

void AbstractInterpretation::updateRelationalOnBinary(
    const BinaryOPStmt* binary, const IntervalValue& result)
{
    if (!Options::AERelational())
        return;
    const ICFGNode* node = binary->getICFGNode();
    initializeRelationalState(node);
    SVFRelationalBridge& state = *relationalTrace[node];
    const SVFVar* target = binary->getRes();
    if (!state.tracks(target->getId()))
        return;

    const bool affineOpcode = binary->getOpcode() == BinaryOPStmt::Add ||
                              binary->getOpcode() == BinaryOPStmt::Sub;
    const IntervalValue typeRange = utils->getRangeLimitFromType(
        target->getType());
    const AbstractValue& lhsValue =
        getAbsValue(binary->getOpVar(0), node);
    const AbstractValue& rhsValue =
        getAbsValue(binary->getOpVar(1), node);
    bool noOverflow = affineOpcode && lhsValue.isInterval() &&
                      rhsValue.isInterval() &&
                      !lhsValue.getInterval().isBottom() &&
                      !rhsValue.getInterval().isBottom() &&
                      !lhsValue.getInterval().is_infinite() &&
                      !rhsValue.getInterval().is_infinite() &&
                      !typeRange.isBottom() && !typeRange.is_infinite();
    if (noOverflow)
    {
        const IntervalValue& lhs = lhsValue.getInterval();
        const IntervalValue& rhs = rhsValue.getInterval();
        const relational::Rational mathematicalLower =
            relational::Rational(lhs.lb().getIntNumeral()) +
            (binary->getOpcode() == BinaryOPStmt::Add
                 ? relational::Rational(rhs.lb().getIntNumeral())
                 : -relational::Rational(rhs.ub().getIntNumeral()));
        const relational::Rational mathematicalUpper =
            relational::Rational(lhs.ub().getIntNumeral()) +
            (binary->getOpcode() == BinaryOPStmt::Add
                 ? relational::Rational(rhs.ub().getIntNumeral())
                 : -relational::Rational(rhs.lb().getIntNumeral()));
        noOverflow =
            mathematicalLower >= relational::Rational(
                                     typeRange.lb().getIntNumeral()) &&
            mathematicalUpper <= relational::Rational(
                                     typeRange.ub().getIntNumeral());
    }
    if (!affineOpcode || !noOverflow)
    {
        state.assignInterval(target->getId(), result);
        return;
    }

    std::vector<SVFRelationalBridge::AffineTerm> terms;
    relational::Rational constant;
    const relational::Rational rhsSign(
        binary->getOpcode() == BinaryOPStmt::Add ? 1 : -1);
    if (!appendRelationalOperand(node, binary->getOpVar(0),
                                 relational::Rational(1), state, terms,
                                 constant) ||
            !appendRelationalOperand(node, binary->getOpVar(1), rhsSign,
                                     state, terms, constant))
    {
        state.assignInterval(target->getId(), result);
        return;
    }
    state.assignAffine(target->getId(), std::move(terms),
                       std::move(constant));
    state.meetInterval(target->getId(), result);
}

void AbstractInterpretation::updateRelationalCopyValue(
    const ICFGNode* node, const SVFVar* target, const SVFVar* source,
    bool exactMathematicalCopy)
{
    if (!Options::AERelational())
        return;
    initializeRelationalState(node);
    SVFRelationalBridge& state = *relationalTrace[node];
    if (!state.tracks(target->getId()))
        return;

    const AbstractValue& result = getAbsValue(target, node);
    if (!result.isInterval())
    {
        state.forget(target->getId());
        return;
    }
    if (exactMathematicalCopy)
    {
        std::vector<SVFRelationalBridge::AffineTerm> terms;
        relational::Rational constant;
        if (appendRelationalOperand(node, source,
                                    relational::Rational(1), state, terms,
                                    constant))
        {
            state.assignAffine(target->getId(), std::move(terms),
                               std::move(constant));
            state.meetInterval(target->getId(), result.getInterval());
            return;
        }
    }
    state.assignInterval(target->getId(), result.getInterval());
}

void AbstractInterpretation::updateRelationalOnCopy(const CopyStmt* copy)
{
    const bool exact = copy->getCopyKind() == CopyStmt::COPYVAL ||
                       copy->getCopyKind() == CopyStmt::SEXT;
    updateRelationalCopyValue(copy->getICFGNode(), copy->getLHSVar(),
                              copy->getRHSVar(), exact);
}

bool AbstractInterpretation::widenRelationalCycleState(
    const RelationalStatePtr& previous, const RelationalStatePtr& current,
    const ICFGNode* cycleHead)
{
    if (!Options::AERelational())
        return true;
    RelationalStatePtr oldState = previous
                                      ? std::make_shared<SVFRelationalBridge>(
                                            *previous)
                                      : makeRelationalTop(cycleHead);
    RelationalStatePtr nextState = current
                                       ? std::make_shared<SVFRelationalBridge>(
                                             *current)
                                       : makeRelationalTop(cycleHead);
    oldState->changeTrackedVariables(trackedRelationalVariables(cycleHead));
    nextState->changeTrackedVariables(trackedRelationalVariables(cycleHead));
    RelationalStatePtr widened =
        std::make_shared<SVFRelationalBridge>(*oldState);
    widened->widenWith(*nextState);
    const bool fixpoint = widened->equals(*oldState);
    relationalTrace[cycleHead] = std::move(widened);
    return fixpoint;
}

bool AbstractInterpretation::narrowRelationalCycleState(
    const RelationalStatePtr& previous, const RelationalStatePtr& current,
    const ICFGNode* cycleHead)
{
    if (!Options::AERelational())
        return true;
    RelationalStatePtr oldState = previous
                                      ? std::make_shared<SVFRelationalBridge>(
                                            *previous)
                                      : makeRelationalTop(cycleHead);
    RelationalStatePtr nextState = current
                                       ? std::make_shared<SVFRelationalBridge>(
                                             *current)
                                       : makeRelationalTop(cycleHead);
    oldState->changeTrackedVariables(trackedRelationalVariables(cycleHead));
    nextState->changeTrackedVariables(trackedRelationalVariables(cycleHead));
    if (!nextState->includedIn(*oldState))
    {
        relationalTrace[cycleHead] = oldState;
        return true;
    }
    RelationalStatePtr narrowed =
        std::make_shared<SVFRelationalBridge>(*oldState);
    narrowed->narrowWith(*nextState);
    const bool fixpoint = narrowed->equals(*oldState);
    relationalTrace[cycleHead] = std::move(narrowed);
    return fixpoint;
}
