//===- SVFRelationalIntegration.cpp -- AE reduced-product lifecycle -----===//

#include "AE/Svfexe/AbstractInterpretation.h"

#include "SVFIR/SVFIR.h"
#include "Util/Options.h"

#include <memory>
#include <string>
#include <utility>

using namespace SVF;

#ifdef SVF_BUILD_RELATIONAL_DOMAIN

bool AbstractInterpretation::hasRelationalState(const ICFGNode* node) const
{
    const auto it = abstractTrace.find(node);
    return it != abstractTrace.end() &&
           it->second.hasRelationalNumericalState();
}

const SVFRelationalBridge*
AbstractInterpretation::getRelationalState(const ICFGNode* node) const
{
    const auto it = abstractTrace.find(node);
    return it == abstractTrace.end()
               ? nullptr
               : it->second.getRelationalNumericalState();
}

bool AbstractInterpretation::isRelationalStateBottom(
    const ICFGNode* node) const
{
    const SVFRelationalBridge* state = getRelationalState(node);
    return Options::AERelational() && state && state->isBottom();
}

bool AbstractInterpretation::isRelationalBranchFeasible(
    const IntraCFGEdge* edge)
{
    if (!Options::AERelational())
        return true;
    const SVFRelationalBridge* source =
        getRelationalState(edge->getSrcNode());
    if (!source)
        return true;
    SVFRelationalBridge state(*source);
    assumeRelationalBranch(edge, state);
    return !state.isBottom();
}

void AbstractInterpretation::initializeRelationalEnvironments()
{
    if (!Options::AERelational())
        return;

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
            {value->getId(), SVF::NumericType::integer(),
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

void AbstractInterpretation::initializeRelationalState(const ICFGNode* node)
{
    if (!Options::AERelational())
        return;
    abstractTrace[node].initializeRelationalNumericalState(
        trackedRelationalVariables(node));
}

void AbstractInterpretation::adaptRelationalState(
    const ICFGNode* node, IntervalState& state) const
{
    if (!Options::AERelational())
        return;
    if (!state.hasRelationalNumericalState())
        state.initializeRelationalNumericalState(
            trackedRelationalVariables(node));
    else
        state.changeRelationalNumericalEnvironment(
            trackedRelationalVariables(node));
}

bool AbstractInterpretation::appendRelationalOperand(
    const ICFGNode* node, const SVFVar* operand,
    const SVF::Rational& multiplier, SVFRelationalBridge& state,
    std::vector<SVFRelationalBridge::AffineTerm>& terms,
    SVF::Rational& constant)
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
                    SVF::Rational(integer->getSExtValue());
        return true;
    }

    const AbstractValue& value = getAbsValue(operand, node);
    if (value.isInterval() && value.getInterval().is_numeral())
    {
        constant += multiplier * SVF::Rational(
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

    SVF::ConstraintKind kind;
    switch (predicate)
    {
    case CmpStmt::ICMP_EQ:
        kind = SVF::ConstraintKind::Equal;
        break;
    case CmpStmt::ICMP_NE:
        kind = SVF::ConstraintKind::NotEqual;
        break;
    case CmpStmt::ICMP_SGT:
        kind = SVF::ConstraintKind::GreaterThan;
        break;
    case CmpStmt::ICMP_SGE:
        kind = SVF::ConstraintKind::GreaterEqual;
        break;
    case CmpStmt::ICMP_SLT:
        kind = SVF::ConstraintKind::LessThan;
        break;
    case CmpStmt::ICMP_SLE:
        kind = SVF::ConstraintKind::LessEqual;
        break;
    default:
        return;
    }

    std::vector<SVFRelationalBridge::AffineTerm> terms;
    SVF::Rational constant;
    const ICFGNode* predecessor = edge->getSrcNode();
    if (!appendRelationalOperand(predecessor, comparison->getOpVar(0),
                                 SVF::Rational(1), state, terms,
                                 constant) ||
            !appendRelationalOperand(predecessor,
                                     comparison->getOpVar(1),
                                     SVF::Rational(-1), state, terms,
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
    SVFRelationalBridge& state =
        *abstractTrace[node].getRelationalNumericalState();
    if (state.tracks(target->getId()))
    {
        // SVF uses a bottom IntervalValue as its uninitialized/no-numeric
        // sentinel, not as whole-path unreachability. Forgetting is the sound
        // relational interpretation of that sentinel.
        if (interval.isBottom())
            state.forget(target->getId());
        else
            state.assignInterval(target->getId(), interval);
        reduceRelationalInterval(node, target);
    }
}

void AbstractInterpretation::synchronizeRelationalWithIntervals(
    const ICFGNode* node)
{
    if (!Options::AERelational())
        return;
    initializeRelationalState(node);
    SVFRelationalBridge& state =
        *abstractTrace[node].getRelationalNumericalState();
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

void AbstractInterpretation::reduceRelationalInterval(
    const ICFGNode* node, const SVFVar* variable)
{
    if (!Options::AERelational() || !variable)
        return;
    const SVFRelationalBridge* relationalState = getRelationalState(node);
    if (!relationalState || !relationalState->tracks(variable->getId()) ||
            !hasAbsValue(variable, node))
        return;

    const AbstractValue& current = getAbsValue(variable, node);
    if (!current.isInterval())
        return;
    IntervalValue reduced = current.getInterval();
    reduced.meet_with(
        relationalState->projectInterval(variable->getId()));
    updateAbsValue(variable, reduced, node);
}

void AbstractInterpretation::reduceRelationalIntervals(
    const ICFGNode* node)
{
    if (!Options::AERelational() || Options::AESparsity() != Dense)
        return;
    for (const TrackedRelationalVariable& tracked :
            trackedRelationalVariables(node))
        reduceRelationalInterval(node, getSVFVar(tracked.id));
}

void AbstractInterpretation::updateRelationalOnBinary(
    const BinaryOPStmt* binary, const IntervalValue& result)
{
    if (!Options::AERelational())
        return;
    const ICFGNode* node = binary->getICFGNode();
    initializeRelationalState(node);
    SVFRelationalBridge& state =
        *abstractTrace[node].getRelationalNumericalState();
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
    auto reducedOperandInterval = [&](const SVFVar* operand,
                                      const AbstractValue& value)
    {
        if (const ConstIntValVar* integer =
                SVFUtil::dyn_cast<ConstIntValVar>(operand))
            return IntervalValue(integer->getSExtValue());
        IntervalValue interval =
            value.isInterval() && !value.getInterval().isBottom()
                ? value.getInterval()
                : IntervalValue::top();
        if (state.tracks(operand->getId()))
            interval.meet_with(state.projectInterval(operand->getId()));
        return interval;
    };
    const IntervalValue lhs =
        reducedOperandInterval(binary->getOpVar(0), lhsValue);
    const IntervalValue rhs =
        reducedOperandInterval(binary->getOpVar(1), rhsValue);
    bool noOverflow = affineOpcode && !lhs.isBottom() && !rhs.isBottom() &&
                      !lhs.is_infinite() && !rhs.is_infinite() &&
                      !typeRange.isBottom() && !typeRange.is_infinite();
    if (noOverflow)
    {
        const SVF::Rational mathematicalLower =
            SVF::Rational(lhs.lb().getIntNumeral()) +
            (binary->getOpcode() == BinaryOPStmt::Add
                 ? SVF::Rational(rhs.lb().getIntNumeral())
                 : -SVF::Rational(rhs.ub().getIntNumeral()));
        const SVF::Rational mathematicalUpper =
            SVF::Rational(lhs.ub().getIntNumeral()) +
            (binary->getOpcode() == BinaryOPStmt::Add
                 ? SVF::Rational(rhs.ub().getIntNumeral())
                 : -SVF::Rational(rhs.lb().getIntNumeral()));
        noOverflow =
            mathematicalLower >= SVF::Rational(
                                     typeRange.lb().getIntNumeral()) &&
            mathematicalUpper <= SVF::Rational(
                                     typeRange.ub().getIntNumeral());
    }
    if (!affineOpcode || !noOverflow)
    {
        assignRelationalInterval(node, target, result);
        return;
    }

    std::vector<SVFRelationalBridge::AffineTerm> terms;
    SVF::Rational constant;
    const SVF::Rational rhsSign(
        binary->getOpcode() == BinaryOPStmt::Add ? 1 : -1);
    if (!appendRelationalOperand(node, binary->getOpVar(0),
                                 SVF::Rational(1), state, terms,
                                 constant) ||
            !appendRelationalOperand(node, binary->getOpVar(1), rhsSign,
                                     state, terms, constant))
    {
        assignRelationalInterval(node, target, result);
        return;
    }
    state.assignAffine(target->getId(), std::move(terms),
                       std::move(constant));
    if (!result.isBottom())
        state.meetInterval(target->getId(), result);
    reduceRelationalInterval(node, target);
}

void AbstractInterpretation::updateRelationalCopyValue(
    const ICFGNode* node, const SVFVar* target, const SVFVar* source,
    bool exactMathematicalCopy)
{
    if (!Options::AERelational())
        return;
    initializeRelationalState(node);
    SVFRelationalBridge& state =
        *abstractTrace[node].getRelationalNumericalState();
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
        SVF::Rational constant;
        if (appendRelationalOperand(node, source,
                                    SVF::Rational(1), state, terms,
                                    constant))
        {
            state.assignAffine(target->getId(), std::move(terms),
                               std::move(constant));
            state.meetInterval(target->getId(), result.getInterval());
            reduceRelationalInterval(node, target);
            return;
        }
    }
    state.assignInterval(target->getId(), result.getInterval());
    reduceRelationalInterval(node, target);
}

void AbstractInterpretation::updateRelationalOnCopy(const CopyStmt* copy)
{
    const bool exact = copy->getCopyKind() == CopyStmt::COPYVAL ||
                       copy->getCopyKind() == CopyStmt::SEXT;
    updateRelationalCopyValue(copy->getICFGNode(), copy->getLHSVar(),
                              copy->getRHSVar(), exact);
}

#else

bool AbstractInterpretation::hasRelationalState(const ICFGNode*) const
{
    return false;
}

const SVFRelationalBridge*
AbstractInterpretation::getRelationalState(const ICFGNode*) const
{
    return nullptr;
}

void AbstractInterpretation::initializeRelationalEnvironments()
{
}

void AbstractInterpretation::initializeRelationalState(const ICFGNode*)
{
}

void AbstractInterpretation::adaptRelationalState(
    const ICFGNode*, IntervalState&) const
{
}

bool AbstractInterpretation::isRelationalStateBottom(const ICFGNode*) const
{
    return false;
}

bool AbstractInterpretation::isRelationalBranchFeasible(const IntraCFGEdge*)
{
    return true;
}

void AbstractInterpretation::assignRelationalInterval(
    const ICFGNode*, const SVFVar*, const IntervalValue&)
{
}

void AbstractInterpretation::synchronizeRelationalWithIntervals(
    const ICFGNode*)
{
}

void AbstractInterpretation::reduceRelationalInterval(
    const ICFGNode*, const SVFVar*)
{
}

void AbstractInterpretation::reduceRelationalIntervals(const ICFGNode*)
{
}

void AbstractInterpretation::updateRelationalOnBinary(
    const BinaryOPStmt*, const IntervalValue&)
{
}

void AbstractInterpretation::updateRelationalOnCopy(const CopyStmt*)
{
}

void AbstractInterpretation::updateRelationalCopyValue(
    const ICFGNode*, const SVFVar*, const SVFVar*, bool)
{
}

#endif
