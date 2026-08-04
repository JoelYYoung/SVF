//===- relational-domain-test.cpp -- Relational fixpoint regression tests ===//

#include "AE/Core/RelationalDomain.h"

#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace SVF;

namespace
{

unsigned failures = 0;

RelationalExpr realVar(RelationalVariableId id)
{
    return RelationalExpr::variableExpr(id, RelationalNumericType::Real);
}

RelationalExpr integerVar(RelationalVariableId id)
{
    return RelationalExpr::variableExpr(id, RelationalNumericType::Integer);
}

RelationalExpr float32Var(RelationalVariableId id)
{
    return RelationalExpr::variableExpr(id, RelationalNumericType::Float32);
}

RelationalConstraint relation(RelationalExpr lhs,
                              RelationalPredicate predicate,
                              RelationalExpr rhs)
{
    return {std::move(lhs), predicate, std::move(rhs)};
}

void expect(const std::string& name, RelationalCheckResult actual,
            RelationalCheckResult expected)
{
    if (actual == expected)
        return;
    ++failures;
    std::cerr << "FAIL: " << name << " expected " << toString(expected)
              << ", got " << toString(actual) << '\n';
}

void testWideningKeepsStableLowerBound()
{
    auto top = makeZ3RelationalDomain({{0, RelationalNumericType::Real}}, 5000);
    auto zero = top->clone();
    auto one = top->clone();
    zero->assume(relation(realVar(0), RelationalPredicate::Equal,
                          RelationalExpr::realConstant("0")));
    one->assume(relation(realVar(0), RelationalPredicate::Equal,
                         RelationalExpr::realConstant("1")));

    auto growing = zero->join(*one);
    auto widened = zero->widening(*growing);
    expect("widening contains its old state", zero->isSubsetOf(*widened),
           RelationalCheckResult::True);
    expect("widening contains its successor", growing->isSubsetOf(*widened),
           RelationalCheckResult::True);
    expect("widening keeps stable x >= 0",
           widened->entails(relation(realVar(0),
                                     RelationalPredicate::GreaterEqual,
                                     RelationalExpr::realConstant("0"))),
           RelationalCheckResult::True);
    expect("widening drops increasing x <= 1",
           widened->entails(relation(realVar(0),
                                     RelationalPredicate::LessEqual,
                                     RelationalExpr::realConstant("1"))),
           RelationalCheckResult::False);

    auto narrowed = widened->narrowing(*growing);
    expect("narrowing restores the infinite upper bound",
           narrowed->entails(relation(realVar(0),
                                      RelationalPredicate::LessEqual,
                                      RelationalExpr::realConstant("1"))),
           RelationalCheckResult::True);
    expect("narrowing remains above its successor",
           growing->isSubsetOf(*narrowed), RelationalCheckResult::True);
}

void testWideningKeepsRelationalEquality()
{
    const std::vector<RelationalVariable> variables = {
        {0, RelationalNumericType::Real},
        {1, RelationalNumericType::Real},
    };
    auto top = makeZ3RelationalDomain(variables, 5000);
    auto zero = top->clone();
    auto one = top->clone();
    for (RelationalVariableId id : {0U, 1U})
    {
        zero->assume(relation(realVar(id), RelationalPredicate::Equal,
                              RelationalExpr::realConstant("0")));
        one->assume(relation(realVar(id), RelationalPredicate::Equal,
                             RelationalExpr::realConstant("1")));
    }

    auto growing = zero->join(*one);
    auto widened = zero->widening(*growing);
    expect("widening keeps stable x - y = 0",
           widened->entails(relation(realVar(0), RelationalPredicate::Equal,
                                     realVar(1))),
           RelationalCheckResult::True);
}

void testNarrowingOnlyRestoresInfiniteBounds()
{
    auto current =
        makeZ3RelationalDomain({{0, RelationalNumericType::Real}}, 5000);
    current->assume(relation(realVar(0), RelationalPredicate::LessEqual,
                             RelationalExpr::realConstant("10")));
    auto successor = current->clone();
    successor->assume(relation(realVar(0), RelationalPredicate::LessEqual,
                               RelationalExpr::realConstant("5")));
    successor->assume(relation(realVar(0), RelationalPredicate::GreaterEqual,
                               RelationalExpr::realConstant("0")));

    auto narrowed = current->narrowing(*successor);
    expect("narrowing restores missing x >= 0",
           narrowed->entails(relation(realVar(0),
                                      RelationalPredicate::GreaterEqual,
                                      RelationalExpr::realConstant("0"))),
           RelationalCheckResult::True);
    expect("narrowing retains finite x <= 10 instead of meeting x <= 5",
           narrowed->entails(relation(realVar(0),
                                      RelationalPredicate::LessEqual,
                                      RelationalExpr::realConstant("5"))),
           RelationalCheckResult::False);
    expect("narrowing keeps the original finite upper bound",
           narrowed->entails(relation(realVar(0),
                                      RelationalPredicate::LessEqual,
                                      RelationalExpr::realConstant("10"))),
           RelationalCheckResult::True);
}

void testRelationalLowerBoundApproximation()
{
    const std::vector<RelationalVariable> variables = {
        {0, RelationalNumericType::Real},
        {1, RelationalNumericType::Real},
    };
    auto state = makeZ3RelationalDomain(variables, 5000);
    state->assume(relation(realVar(0), RelationalPredicate::Equal,
                           RelationalExpr::realConstant("2")));
    state->assume(relation(realVar(1), RelationalPredicate::Equal,
                           RelationalExpr::realConstant("5")));
    auto lower = state->lowerBoundApproximation();

    expect("lower-bound projection is a sound over-approximation",
           state->isSubsetOf(*lower), RelationalCheckResult::True);
    expect("lower-bound projection keeps x >= 2",
           lower->entails(relation(realVar(0),
                                   RelationalPredicate::GreaterEqual,
                                   RelationalExpr::realConstant("2"))),
           RelationalCheckResult::True);
    RelationalExpr sum = RelationalExpr::binary(
        RelationalBinaryOperator::Add, RelationalNumericType::Real,
        realVar(0), realVar(1));
    expect("lower-bound projection keeps relational x + y >= 7",
           lower->entails(relation(std::move(sum),
                                   RelationalPredicate::GreaterEqual,
                                   RelationalExpr::realConstant("7"))),
           RelationalCheckResult::True);
    expect("lower-bound projection does not invent an upper bound",
           lower->entails(relation(realVar(0), RelationalPredicate::LessEqual,
                                   RelationalExpr::realConstant("2"))),
           RelationalCheckResult::False);
}

void testIntegerTemplates()
{
    auto top =
        makeZ3RelationalDomain({{0, RelationalNumericType::Integer}}, 5000);
    auto zero = top->clone();
    auto one = top->clone();
    zero->assume(relation(integerVar(0), RelationalPredicate::Equal,
                          RelationalExpr::integerConstant(0)));
    one->assume(relation(integerVar(0), RelationalPredicate::Equal,
                         RelationalExpr::integerConstant(1)));
    auto growing = zero->join(*one);
    auto widened = zero->widening(*growing);
    expect("integer widening keeps stable lower bound",
           widened->entails(relation(integerVar(0),
                                     RelationalPredicate::GreaterEqual,
                                     RelationalExpr::integerConstant(0))),
           RelationalCheckResult::True);
}

void testNarrowingPrecondition()
{
    auto current =
        makeZ3RelationalDomain({{0, RelationalNumericType::Real}}, 5000);
    current->assume(relation(realVar(0), RelationalPredicate::LessEqual,
                             RelationalExpr::realConstant("0")));
    auto unrelated = current->clone();
    unrelated->forget(0);
    unrelated->assume(relation(realVar(0), RelationalPredicate::GreaterEqual,
                               RelationalExpr::realConstant("1")));
    try
    {
        (void)current->narrowing(*unrelated);
        ++failures;
        std::cerr << "FAIL: narrowing accepted a non-descending successor\n";
    }
    catch (const std::invalid_argument&)
    {
    }
}

void testFloatingPointProjectionStaysSound()
{
    auto top =
        makeZ3RelationalDomain({{0, RelationalNumericType::Float32}}, 5000);
    auto zero = top->clone();
    auto one = top->clone();
    zero->assign(0, RelationalExpr::float32Constant(0x00000000U));
    one->assign(0, RelationalExpr::float32Constant(0x3f800000U));
    auto growing = zero->join(*one);
    auto widened = zero->widening(*growing);

    expect("FPA widening contains the old IEEE state",
           zero->isSubsetOf(*widened), RelationalCheckResult::True);
    expect("FPA widening contains the successor IEEE state",
           growing->isSubsetOf(*widened), RelationalCheckResult::True);
    expect("FPA widening does not reinterpret float bits as a Real bound",
           widened->entails(relation(
               float32Var(0), RelationalPredicate::BitwiseEqual,
               RelationalExpr::float32Constant(0x00000000U))),
           RelationalCheckResult::False);

    auto lower = zero->lowerBoundApproximation();
    expect("lower-bound projection does not invent an order for IEEE values",
           lower->entails(relation(
               float32Var(0), RelationalPredicate::BitwiseEqual,
               RelationalExpr::float32Constant(0x00000000U))),
           RelationalCheckResult::False);
}

} // namespace

int main()
{
    testWideningKeepsStableLowerBound();
    testWideningKeepsRelationalEquality();
    testNarrowingOnlyRestoresInfiniteBounds();
    testRelationalLowerBoundApproximation();
    testIntegerTemplates();
    testNarrowingPrecondition();
    testFloatingPointProjectionStaysSound();

    if (failures != 0)
    {
        std::cerr << failures << " relational-domain test(s) failed\n";
        return 1;
    }
    std::cout << "all relational-domain tests passed\n";
    return 0;
}
