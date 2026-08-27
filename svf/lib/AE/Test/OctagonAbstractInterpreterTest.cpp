//===- OctagonAbstractInterpreterTest.cpp -- Small worklist AE test -------===//

#include "AE/Core/BoxDomain.h"
#include "AE/Core/OctagonDomain.h"

#include <cstdlib>
#include <iostream>
#include <map>
#include <optional>
#include <queue>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

using namespace SVF::AbstractDomain;

namespace
{

struct Havoc
{
    Variable target;
};

struct Assign
{
    Variable target;
    LinearExpression expression;
};

struct Assert
{
    std::string label;
    LinearConstraint condition;
};

using Instruction = std::variant<Havoc, Assign, Assert>;

struct ControlEdge
{
    std::size_t successor;
    std::optional<LinearConstraint> assumption;
};

struct BasicBlock
{
    std::vector<Instruction> instructions;
    std::vector<ControlEdge> successors;
};

template <typename NumericalStateT>
class WorklistInterpreter
{
public:
    WorklistInterpreter(VariableEnvironment environment,
                        std::vector<BasicBlock> program)
        : environment_(std::move(environment)), program_(std::move(program)),
          entries_(program_.size())
    {
    }

    void run(std::size_t entry)
    {
        if (entry >= program_.size())
            throw std::out_of_range("entry block is outside the program");

        entries_[entry] = NumericalStateT::top(environment_);
        std::queue<std::size_t> worklist;
        std::vector<bool> queued(program_.size(), false);
        worklist.push(entry);
        queued[entry] = true;
        std::size_t iterations = 0;

        while (!worklist.empty())
        {
            if (++iterations > 1000)
                throw std::runtime_error(
                    "test interpreter requires widening for this CFG");

            const std::size_t blockIndex = worklist.front();
            worklist.pop();
            queued[blockIndex] = false;
            NumericalStateT output = *entries_[blockIndex];
            execute(program_[blockIndex], output);

            for (const ControlEdge& edge :
                 program_[blockIndex].successors)
            {
                if (edge.successor >= program_.size())
                    throw std::out_of_range(
                        "successor block is outside the program");
                NumericalStateT candidate = output;
                if (edge.assumption)
                    candidate.assume(*edge.assumption);
                if (candidate.isBottom())
                    continue;

                bool changed = false;
                if (!entries_[edge.successor])
                {
                    entries_[edge.successor] = std::move(candidate);
                    changed = true;
                }
                else
                {
                    NumericalStateT joined =
                        entries_[edge.successor]->join(candidate);
                    changed = joined.isEquivalentTo(
                                  *entries_[edge.successor]) !=
                              CheckResult::True;
                    if (changed)
                        entries_[edge.successor] = std::move(joined);
                }

                if (changed && !queued[edge.successor])
                {
                    worklist.push(edge.successor);
                    queued[edge.successor] = true;
                }
            }
        }
    }

    CheckResult assertion(const std::string& label) const
    {
        const auto result = assertions_.find(label);
        if (result == assertions_.end())
            throw std::out_of_range("assertion was not evaluated: " + label);
        return result->second;
    }

    const NumericalStateT& entryState(std::size_t block) const
    {
        if (block >= entries_.size() || !entries_[block])
            throw std::out_of_range("block is unreachable");
        return *entries_[block];
    }

private:
    void execute(const BasicBlock& block, NumericalStateT& state)
    {
        for (const Instruction& instruction : block.instructions)
        {
            std::visit(
                [&](const auto& operation)
                {
                    using Operation = std::decay_t<decltype(operation)>;
                    if constexpr (std::is_same_v<Operation, Havoc>)
                        state.forget(operation.target);
                    else if constexpr (std::is_same_v<Operation, Assign>)
                        state.assign(operation.target, operation.expression);
                    else
                        assertions_[operation.label] =
                            state.entails(operation.condition);
                },
                instruction);
        }
    }

    VariableEnvironment environment_;
    std::vector<BasicBlock> program_;
    std::vector<std::optional<NumericalStateT>> entries_;
    std::map<std::string, CheckResult> assertions_;
};

void require(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

void testRelationalCopyAssertion()
{
    const Variable a(1);
    const Variable x(2);
    const VariableEnvironment environment(
        {{a, NumericType::integer(), "a"},
         {x, NumericType::integer(), "x"}});
    const LinearConstraint aPositive = greaterThan(
        LinearExpression(a), LinearExpression(Rational(0)));
    const LinearConstraint aNotPositive = lessEqual(
        LinearExpression(a), LinearExpression(Rational(0)));
    const LinearConstraint xPositive = greaterThan(
        LinearExpression(x), LinearExpression(Rational(0)));

    std::vector<BasicBlock> program(3);
    program[0].instructions =
        {Havoc{a}, Assign{x, LinearExpression(a)}};
    program[0].successors = {{1, aPositive}, {2, aNotPositive}};
    program[1].instructions = {Assert{"x_positive", xPositive}};

    WorklistInterpreter<BoxState> intervalLike(environment, program);
    intervalLike.run(0);
    require(intervalLike.assertion("x_positive") == CheckResult::Unknown,
            "Box should not propagate a > 0 through the lost x == a relation");

    WorklistInterpreter<OctagonState> relational(environment, program);
    relational.run(0);
    require(relational.assertion("x_positive") == CheckResult::True,
            "Octagon must prove x > 0 from x == a and a > 0");

    const OctagonState& thenEntry = relational.entryState(1);
    require(thenEntry.entails(equal(LinearExpression(x),
                                   LinearExpression(a))) ==
                CheckResult::True &&
                thenEntry.entails(aPositive) == CheckResult::True &&
                thenEntry.entails(xPositive) == CheckResult::True,
            "then-entry state must retain the copy relation and branch fact");
}

} // namespace

int main()
{
    try
    {
        testRelationalCopyAssertion();
        std::cout << "Octagon abstract interpreter test: PASS\n";
        return EXIT_SUCCESS;
    }
    catch (const std::exception& error)
    {
        std::cerr << "Octagon abstract interpreter test: FAIL: "
                  << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
