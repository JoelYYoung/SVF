//===- relational-domain-eval.cpp -- APRON/Z3 differential evaluator ----===//

#include "AE/Core/RelationalDomain.h"

#include <chrono>
#include <cfenv>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace SVF;

namespace
{

using Clock = std::chrono::steady_clock;
using Factory = std::function<std::unique_ptr<RelationalDomain>(
    const std::vector<RelationalVariable>&)>;

struct Result
{
    std::string backend;
    std::string scenario;
    std::string result;
    long long microseconds = 0;
    std::string note;
};

std::uint64_t bits(double value)
{
    std::uint64_t result = 0;
    static_assert(sizeof(result) == sizeof(value), "unexpected double size");
    std::memcpy(&result, &value, sizeof(result));
    return result;
}

std::uint32_t bits(float value)
{
    std::uint32_t result = 0;
    static_assert(sizeof(result) == sizeof(value), "unexpected float size");
    std::memcpy(&result, &value, sizeof(result));
    return result;
}

RelationalExpr var(RelationalVariableId id, RelationalNumericType type)
{
    return RelationalExpr::variableExpr(id, type);
}

RelationalConstraint constraint(RelationalExpr lhs,
                                RelationalPredicate predicate,
                                RelationalExpr rhs)
{
    return {std::move(lhs), predicate, std::move(rhs)};
}

void emitCsvField(const std::string& value)
{
    std::cout << '"';
    for (char character : value)
    {
        if (character == '"')
            std::cout << "\"\"";
        else
            std::cout << character;
    }
    std::cout << '"';
}

template <typename Action>
Result evaluate(const std::string& backend, const std::string& scenario,
                Action action)
{
    const auto start = Clock::now();
    Result result;
    result.backend = backend;
    result.scenario = scenario;
    try
    {
        result.result = toString(action());
    }
    catch (const std::exception& error)
    {
        result.result = "unsupported";
        result.note = error.what();
    }
    result.microseconds =
        std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() -
                                                              start)
            .count();
    return result;
}

void runBackend(const Factory& factory, const std::string& requestedName,
                std::vector<Result>& results)
{
    std::string backend = requestedName;
    try
    {
        auto probe = factory({{0, RelationalNumericType::Real}});
        backend = probe->backendName();
    }
    catch (const std::exception& error)
    {
        results.push_back(
            {requestedName, "backend-available", "unsupported", 0, error.what()});
        return;
    }

    results.push_back(evaluate(backend, "preserves-host-rounding-mode", [&]()
    {
        // Some APRON floating implementations change the process-wide FPU
        // mode.  Detect that integration hazard for the manager under test.
        return std::fegetround() == FE_TONEAREST
                   ? RelationalCheckResult::True
                   : RelationalCheckResult::False;
    }));

    results.push_back(evaluate(backend, "relational-equality-bound", [&]()
    {
        const std::vector<RelationalVariable> variables = {
            {0, RelationalNumericType::Real},
            {1, RelationalNumericType::Real},
        };
        auto state = factory(variables);
        state->assume(constraint(var(0, RelationalNumericType::Real),
                                 RelationalPredicate::Equal,
                                 var(1, RelationalNumericType::Real)));
        state->assume(constraint(var(0, RelationalNumericType::Real),
                                 RelationalPredicate::LessEqual,
                                 RelationalExpr::realConstant("5")));
        return state->entails(
            constraint(var(1, RelationalNumericType::Real),
                       RelationalPredicate::LessEqual,
                       RelationalExpr::realConstant("5")));
    }));

    results.push_back(evaluate(backend, "strong-assignment-kills-old-value", [&]()
    {
        const std::vector<RelationalVariable> variables = {
            {0, RelationalNumericType::Real},
        };
        auto state = factory(variables);
        state->assume(constraint(var(0, RelationalNumericType::Real),
                                 RelationalPredicate::Equal,
                                 RelationalExpr::realConstant("0")));
        state->assign(0, RelationalExpr::realConstant("1"));
        return state->entails(
            constraint(var(0, RelationalNumericType::Real),
                       RelationalPredicate::Equal,
                       RelationalExpr::realConstant("1")));
    }));

    results.push_back(evaluate(backend, "contradictory-state-is-bottom", [&]()
    {
        const std::vector<RelationalVariable> variables = {
            {0, RelationalNumericType::Real},
        };
        auto state = factory(variables);
        state->assume(constraint(var(0, RelationalNumericType::Real),
                                 RelationalPredicate::LessThan,
                                 RelationalExpr::realConstant("0")));
        state->assume(constraint(var(0, RelationalNumericType::Real),
                                 RelationalPredicate::GreaterEqual,
                                 RelationalExpr::realConstant("0")));
        return state->isBottom();
    }));

    results.push_back(evaluate(backend, "convex-join-keeps-correlation", [&]()
    {
        const std::vector<RelationalVariable> variables = {
            {0, RelationalNumericType::Real},
            {1, RelationalNumericType::Real},
        };
        auto top = factory(variables);
        auto first = top->clone();
        auto second = top->clone();
        for (RelationalVariableId id : {0U, 1U})
        {
            first->assume(constraint(var(id, RelationalNumericType::Real),
                                     RelationalPredicate::Equal,
                                     RelationalExpr::realConstant("0")));
            second->assume(constraint(var(id, RelationalNumericType::Real),
                                      RelationalPredicate::Equal,
                                      RelationalExpr::realConstant("1")));
        }
        auto joined = first->join(*second);
        return joined->entails(
            constraint(var(0, RelationalNumericType::Real),
                       RelationalPredicate::Equal,
                       var(1, RelationalNumericType::Real)));
    }));

    results.push_back(evaluate(backend, "non-convex-join-excludes-middle", [&]()
    {
        const std::vector<RelationalVariable> variables = {
            {0, RelationalNumericType::Real},
        };
        auto top = factory(variables);
        auto zero = top->clone();
        auto two = top->clone();
        zero->assume(constraint(var(0, RelationalNumericType::Real),
                                RelationalPredicate::Equal,
                                RelationalExpr::realConstant("0")));
        two->assume(constraint(var(0, RelationalNumericType::Real),
                               RelationalPredicate::Equal,
                               RelationalExpr::realConstant("2")));
        auto joined = zero->join(*two);
        return joined->entails(
            constraint(var(0, RelationalNumericType::Real),
                       RelationalPredicate::NotEqual,
                       RelationalExpr::realConstant("1")));
    }));

    results.push_back(evaluate(backend, "widening-retains-stable-lower-bound", [&]()
    {
        const std::vector<RelationalVariable> variables = {
            {0, RelationalNumericType::Real},
        };
        auto top = factory(variables);
        auto zero = top->clone();
        auto one = top->clone();
        zero->assume(constraint(var(0, RelationalNumericType::Real),
                                RelationalPredicate::Equal,
                                RelationalExpr::realConstant("0")));
        one->assume(constraint(var(0, RelationalNumericType::Real),
                               RelationalPredicate::Equal,
                               RelationalExpr::realConstant("1")));
        auto next = zero->join(*one);
        auto widened = zero->widening(*next);
        return widened->entails(
            constraint(var(0, RelationalNumericType::Real),
                       RelationalPredicate::GreaterEqual,
                       RelationalExpr::realConstant("0")));
    }));

    results.push_back(evaluate(backend, "real-encoding-0.1-plus-0.2-equals-0.3", [&]()
    {
        const std::vector<RelationalVariable> variables = {
            {0, RelationalNumericType::Real},
            {1, RelationalNumericType::Real},
            {2, RelationalNumericType::Real},
        };
        auto state = factory(variables);
        state->assign(0, RelationalExpr::realConstant("0.1"));
        state->assign(1, RelationalExpr::realConstant("0.2"));
        state->assign(2, RelationalExpr::binary(
                             RelationalBinaryOperator::Add,
                             RelationalNumericType::Real,
                             var(0, RelationalNumericType::Real),
                             var(1, RelationalNumericType::Real)));
        return state->entails(
            constraint(var(2, RelationalNumericType::Real),
                       RelationalPredicate::Equal,
                       RelationalExpr::realConstant("0.3")));
    }));

    results.push_back(evaluate(backend, "float64-0.1-plus-0.2-not-equal-0.3", [&]()
    {
        const std::vector<RelationalVariable> variables = {
            {0, RelationalNumericType::Float64},
            {1, RelationalNumericType::Float64},
            {2, RelationalNumericType::Float64},
            {3, RelationalNumericType::Float64},
        };
        auto state = factory(variables);
        state->assign(0, RelationalExpr::float64Constant(bits(0.1)));
        state->assign(1, RelationalExpr::float64Constant(bits(0.2)));
        state->assign(2, RelationalExpr::float64Constant(bits(0.3)));
        state->assign(3, RelationalExpr::binary(
                             RelationalBinaryOperator::Add,
                             RelationalNumericType::Float64,
                             var(0, RelationalNumericType::Float64),
                             var(1, RelationalNumericType::Float64)));
        return state->entails(
            constraint(var(3, RelationalNumericType::Float64),
                       RelationalPredicate::NotEqual,
                       var(2, RelationalNumericType::Float64)));
    }));

    results.push_back(evaluate(backend, "float32-ulp-absorption", [&]()
    {
        const std::vector<RelationalVariable> variables = {
            {0, RelationalNumericType::Float32},
            {1, RelationalNumericType::Float32},
            {2, RelationalNumericType::Float32},
        };
        auto state = factory(variables);
        state->assign(0,
                      RelationalExpr::float32Constant(bits(16777216.0F)));
        state->assign(1, RelationalExpr::float32Constant(bits(1.0F)));
        state->assign(2, RelationalExpr::binary(
                             RelationalBinaryOperator::Add,
                             RelationalNumericType::Float32,
                             var(0, RelationalNumericType::Float32),
                             var(1, RelationalNumericType::Float32)));
        return state->entails(
            constraint(var(2, RelationalNumericType::Float32),
                       RelationalPredicate::Equal,
                       var(0, RelationalNumericType::Float32)));
    }));

    results.push_back(evaluate(backend, "float64-subnormal-underflow-to-zero", [&]()
    {
        const std::vector<RelationalVariable> variables = {
            {0, RelationalNumericType::Float64},
            {1, RelationalNumericType::Float64},
            {2, RelationalNumericType::Float64},
        };
        auto state = factory(variables);
        state->assign(0, RelationalExpr::float64Constant(1));
        state->assign(1, RelationalExpr::float64Constant(bits(2.0)));
        state->assign(2, RelationalExpr::binary(
                             RelationalBinaryOperator::Divide,
                             RelationalNumericType::Float64,
                             var(0, RelationalNumericType::Float64),
                             var(1, RelationalNumericType::Float64)));
        return state->entails(
            constraint(var(2, RelationalNumericType::Float64),
                       RelationalPredicate::Equal,
                       RelationalExpr::float64Constant(0)));
    }));

    results.push_back(evaluate(backend, "float64-overflow-to-infinity", [&]()
    {
        const std::vector<RelationalVariable> variables = {
            {0, RelationalNumericType::Float64},
            {1, RelationalNumericType::Float64},
        };
        auto state = factory(variables);
        state->assign(0, RelationalExpr::float64Constant(0x7fefffffffffffffULL));
        state->assign(1, RelationalExpr::binary(
                             RelationalBinaryOperator::Add,
                             RelationalNumericType::Float64,
                             var(0, RelationalNumericType::Float64),
                             var(0, RelationalNumericType::Float64)));
        return state->entails(
            constraint(var(1, RelationalNumericType::Float64),
                       RelationalPredicate::Equal,
                       RelationalExpr::float64Constant(0x7ff0000000000000ULL)));
    }));

    results.push_back(evaluate(backend, "float64-nan-is-not-equal-to-itself", [&]()
    {
        const std::vector<RelationalVariable> variables = {
            {0, RelationalNumericType::Float64},
        };
        auto state = factory(variables);
        state->assign(0,
                      RelationalExpr::float64Constant(0x7ff8000000000001ULL));
        return state->entails(
            constraint(var(0, RelationalNumericType::Float64),
                       RelationalPredicate::NotEqual,
                       var(0, RelationalNumericType::Float64)));
    }));

    results.push_back(evaluate(backend, "float64-signed-zero-bit-patterns-differ", [&]()
    {
        const std::vector<RelationalVariable> variables = {
            {0, RelationalNumericType::Float64},
            {1, RelationalNumericType::Float64},
        };
        auto state = factory(variables);
        state->assign(0, RelationalExpr::float64Constant(0));
        state->assign(1,
                      RelationalExpr::float64Constant(0x8000000000000000ULL));
        return state->entails(
            constraint(var(0, RelationalNumericType::Float64),
                       RelationalPredicate::BitwiseNotEqual,
                       var(1, RelationalNumericType::Float64)));
    }));
}

} // namespace

int main()
{
    std::vector<Result> results;
    runBackend(
        [](const std::vector<RelationalVariable>& variables)
        { return makeZ3RelationalDomain(variables, 5000); },
        "z3", results);
    runBackend(
        [](const std::vector<RelationalVariable>& variables)
        { return makeApronRelationalDomain(variables); },
        "apron", results);

    std::cout << "backend,scenario,result,microseconds,note\n";
    for (const Result& result : results)
    {
        emitCsvField(result.backend);
        std::cout << ',';
        emitCsvField(result.scenario);
        std::cout << ',';
        emitCsvField(result.result);
        std::cout << ',' << result.microseconds << ',';
        emitCsvField(result.note);
        std::cout << '\n';
    }
    return 0;
}
