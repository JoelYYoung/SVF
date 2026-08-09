//===- Z3SoundnessChecker.cpp -- Test-only relational soundness ----------===//

#include "Z3SoundnessChecker.h"

#include <sstream>
#include <stdexcept>

using namespace SVF;
using namespace SVF::test;

Z3SoundnessChecker::Z3SoundnessChecker(const Environment& environment)
    : environment_(environment)
{
    for (const VariableDeclaration& declaration : environment.variables())
    {
        if (declaration.type.kind == NumericKind::IEEEFloat)
            throw std::invalid_argument(
                "the linear soundness checker does not encode IEEE floats");
        const char* name = declaration.name.c_str();
        z3::expr symbol = declaration.type.kind == NumericKind::Integer
                              ? context_.int_const(name)
                              : context_.real_const(name);
        variables_.emplace(declaration.variable, std::move(symbol));
    }
}

z3::expr Z3SoundnessChecker::variable(Variable value)
{
    const auto it = variables_.find(value);
    if (it == variables_.end())
        throw std::invalid_argument("Z3 checker received an unknown variable");
    if (environment_.typeOf(value).kind == NumericKind::Integer)
        return z3::to_real(it->second);
    return it->second;
}

z3::expr Z3SoundnessChecker::linear(
    const LinearExpression& expression)
{
    z3::expr result = context_.real_val(expression.constant().toString().c_str());
    for (const auto& [value, coefficient] : expression.terms())
    {
        z3::expr factor =
            context_.real_val(coefficient.toString().c_str());
        result = result + factor * variable(value);
    }
    return result;
}

z3::expr Z3SoundnessChecker::constraint(
    const LinearConstraint& value)
{
    z3::expr expression = linear(value.expression());
    z3::expr zero = context_.real_val(0);
    switch (value.kind())
    {
    case ConstraintKind::Equal:
        return expression == zero;
    case ConstraintKind::NotEqual:
        return expression != zero;
    case ConstraintKind::LessThan:
        return expression < zero;
    case ConstraintKind::LessEqual:
        return expression <= zero;
    case ConstraintKind::GreaterThan:
        return expression > zero;
    case ConstraintKind::GreaterEqual:
        return expression >= zero;
    }
    throw std::logic_error("unknown relational constraint kind");
}

z3::expr Z3SoundnessChecker::state(const OctagonState& value)
{
    requireEnvironment(value);
    if (value.isBottom())
        return context_.bool_val(false);

    z3::expr result = context_.bool_val(true);
    for (const LinearConstraint& item : value.toConstraints())
        result = result && constraint(item);
    return result;
}

ProofResult Z3SoundnessChecker::prove(const z3::expr& premise,
                                      const z3::expr& conclusion,
                                      const std::string& obligation)
{
    z3::solver solver(context_);
    solver.add(premise && !conclusion);
    const z3::check_result result = solver.check();
    if (result == z3::unsat)
        return {true, obligation + ": unsat (proved)"};

    std::ostringstream detail;
    detail << obligation << ": ";
    if (result == z3::sat)
        detail << "sat counterexample " << solver.get_model();
    else
        detail << "unknown: " << solver.reason_unknown();
    return {false, detail.str()};
}

void Z3SoundnessChecker::requireEnvironment(const OctagonState& value) const
{
    if (value.environment() != environment_)
        throw std::invalid_argument(
            "Z3 soundness obligation uses a different environment");
}

ProofResult Z3SoundnessChecker::implies(const OctagonState& premise,
                                        const OctagonState& conclusion,
                                        const std::string& obligation)
{
    return prove(state(premise), state(conclusion), obligation);
}

ProofResult Z3SoundnessChecker::checkJoin(const OctagonState& lhs,
                                          const OctagonState& rhs,
                                          const OctagonState& result)
{
    return prove(state(lhs) || state(rhs), state(result),
                 "join: (lhs or rhs) implies result");
}

ProofResult Z3SoundnessChecker::checkMeet(const OctagonState& lhs,
                                          const OctagonState& rhs,
                                          const OctagonState& result)
{
    z3::expr intersection = state(lhs) && state(rhs);
    z3::expr resultFormula = state(result);
    ProofResult sound = prove(intersection, resultFormula,
                              "meet: (lhs and rhs) implies result");
    if (!sound.proved)
        return sound;
    ProofResult precise = prove(resultFormula, intersection,
                                "meet: result implies (lhs and rhs)");
    if (!precise.proved)
        return precise;
    return {true, sound.detail + "; " + precise.detail};
}

ProofResult Z3SoundnessChecker::checkAssume(
    const OctagonState& before, const LinearConstraint& assumption,
    const OctagonState& result)
{
    return prove(state(before) && constraint(assumption), state(result),
                 "assume: before and condition imply result");
}

ProofResult Z3SoundnessChecker::checkAssignment(
    const OctagonState& before, Variable target,
    const LinearExpression& expression, const OctagonState& result)
{
    requireEnvironment(before);
    requireEnvironment(result);
    if (!environment_.contains(target))
        throw std::invalid_argument("assignment target is not in environment");

    z3::expr beforeFormula = state(before);
    z3::expr_vector currentSymbols(context_);
    z3::expr_vector oldSymbols(context_);
    std::map<Variable, z3::expr> oldVariables;
    for (const VariableDeclaration& declaration : environment_.variables())
    {
        const z3::expr& current = variables_.at(declaration.variable);
        const std::string name = declaration.name + "_old";
        z3::expr old = context_.constant(name.c_str(), current.get_sort());
        currentSymbols.push_back(current);
        oldSymbols.push_back(old);
        oldVariables.emplace(declaration.variable, std::move(old));
    }
    z3::expr oldState = beforeFormula.substitute(currentSymbols, oldSymbols);

    z3::expr rhs = context_.real_val(expression.constant().toString().c_str());
    for (const auto& [value, coefficient] : expression.terms())
    {
        const auto oldIt = oldVariables.find(value);
        if (oldIt == oldVariables.end())
            throw std::invalid_argument(
                "assignment expression contains an unknown variable");
        z3::expr oldValue =
            environment_.typeOf(value).kind == NumericKind::Integer
                ? z3::to_real(oldIt->second)
                : oldIt->second;
        rhs = rhs +
              context_.real_val(coefficient.toString().c_str()) * oldValue;
    }

    z3::expr transition = context_.bool_val(true);
    for (const VariableDeclaration& declaration : environment_.variables())
    {
        const z3::expr& current = variables_.at(declaration.variable);
        const z3::expr& old = oldVariables.at(declaration.variable);
        if (declaration.variable == target)
        {
            z3::expr currentValue =
                declaration.type.kind == NumericKind::Integer
                    ? z3::to_real(current)
                    : current;
            transition = transition && currentValue == rhs;
        }
        else
        {
            transition = transition && current == old;
        }
    }
    return prove(oldState && transition, state(result),
                 "assign: old state and transition imply result");
}

ProofResult Z3SoundnessChecker::checkForget(const OctagonState& before,
                                            Variable forgotten,
                                            const OctagonState& result)
{
    requireEnvironment(before);
    requireEnvironment(result);
    const auto it = variables_.find(forgotten);
    if (it == variables_.end())
        throw std::invalid_argument("forgotten variable is not in environment");

    const std::string name = environment_.nameOf(forgotten) + "_forgotten";
    z3::expr old = context_.constant(name.c_str(), it->second.get_sort());
    z3::expr_vector from(context_);
    z3::expr_vector to(context_);
    from.push_back(it->second);
    to.push_back(old);
    z3::expr projected = z3::exists(old, state(before).substitute(from, to));
    return prove(projected, state(result),
                 "forget: existential old target implies result for arbitrary new target");
}

ProofResult Z3SoundnessChecker::checkWidening(
    const OctagonState& current, const OctagonState& next,
    const OctagonState& result)
{
    return prove(state(current) || state(next), state(result),
                 "widening: (current or next) implies result");
}

ProofResult Z3SoundnessChecker::checkNarrowing(
    const OctagonState& current, const OctagonState& next,
    const OctagonState& result)
{
    ProofResult lower = prove(state(next), state(result),
                              "narrowing: next implies result");
    if (!lower.proved)
        return lower;
    ProofResult upper = prove(state(result), state(current),
                              "narrowing: result implies current");
    if (!upper.proved)
        return upper;
    return {true, lower.detail + "; " + upper.detail};
}

ProofResult Z3SoundnessChecker::checkProjection(
    const OctagonState& source, const OctagonState& result)
{
    return prove(state(source), state(result),
                 "projection: source implies result");
}
