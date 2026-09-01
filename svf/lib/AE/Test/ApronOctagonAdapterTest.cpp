//===- ApronOctagonAdapterTest.cpp -- Adapter differential test ----------===//

#include "ApronOctagonState.h"

#include "AE/Core/OctagonDomain.h"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

namespace AD = SVF::AbstractDomain;

namespace
{

void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

AD::VariableEnvironment environment(std::initializer_list<unsigned> ids)
{
    std::vector<AD::VariableDeclaration> declarations;
    for (unsigned id : ids)
        declarations.push_back(
            {AD::Variable(id), AD::NumericType::integer(),
             "v" + std::to_string(id)});
    return AD::VariableEnvironment(std::move(declarations));
}

void requireEquivalent(const AD::OctagonState& native,
                       const AD::ApronOctagonState& apron,
                       const std::string& stage)
{
    require(native.isBottom() == apron.isBottom(),
            stage + ": bottom mismatch");
    if (native.isBottom())
        return;
    const AD::VariableEnvironment& env = native.environment();
    require(env == apron.environment(), stage + ": environment mismatch");
    require(native.hash() == apron.hash(), stage + ": semantic hash mismatch");
    for (const AD::VariableDeclaration& lhs : env.variables())
    {
        require(native.bound(lhs.variable) == apron.bound(lhs.variable),
                stage + ": unary bound mismatch");
        for (const AD::VariableDeclaration& rhs : env.variables())
        {
            for (int lhsSign : {-1, 1})
            {
                for (int rhsSign : {-1, 1})
                {
                    AD::LinearExpression objective;
                    objective.setCoefficient(lhs.variable,
                                             AD::Rational(lhsSign));
                    objective.setCoefficient(
                        rhs.variable,
                        objective.coefficient(rhs.variable) +
                            AD::Rational(rhsSign));
                    require(native.bound(objective) == apron.bound(objective),
                            stage + ": relational objective mismatch");
                }
            }
        }
    }
}

} // namespace

int main()
{
    try
    {
        const AD::Variable x(1), y(2), z(3), w(4);
        AD::VariableEnvironment env = environment({1, 2, 3});
        AD::OctagonState native = AD::OctagonState::top(env);
        AD::ApronOctagonState apron = AD::ApronOctagonState::top(env);

        AD::LinearExpression xMinusY(x);
        xMinusY.setCoefficient(y, AD::Rational(-1));
        AD::LinearExpression yMinusZ(y);
        yMinusZ.setCoefficient(z, AD::Rational(-1));
        AD::LinearExpression xLower(x);
        AD::LinearExpression xUpper(x);
        xUpper.setConstant(AD::Rational(-10));
        AD::LinearConstraintSet initial = {
            {xMinusY, AD::ConstraintKind::Equal},
            {yMinusZ, AD::ConstraintKind::Equal},
            {xLower, AD::ConstraintKind::GreaterEqual},
            {xUpper, AD::ConstraintKind::LessEqual},
        };
        native.assumeAll(initial);
        apron.assumeAll(initial);
        requireEquivalent(native, apron, "assume-all");

        AD::LinearExpression update(y);
        update.setConstant(AD::Rational(1));
        native.assign(z, update);
        apron.assign(z, update);
        requireEquivalent(native, apron, "assignment");

        AD::OctagonState nativeAlternative = native;
        AD::ApronOctagonState apronAlternative = apron;
        nativeAlternative.assign(x, AD::LinearExpression(AD::Rational(20)));
        apronAlternative.assign(x, AD::LinearExpression(AD::Rational(20)));
        native.joinWith(nativeAlternative);
        apron.joinWith(apronAlternative);
        requireEquivalent(native, apron, "join");

        native.assignParallel({{x, AD::LinearExpression(z)},
                               {z, AD::LinearExpression(x)}});
        apron.assignParallel({{x, AD::LinearExpression(z)},
                              {z, AD::LinearExpression(x)}});
        requireEquivalent(native, apron, "parallel-assignment");

        AD::VariableEnvironment expanded = env.add(
            {{w, AD::NumericType::integer(), "v4"}});
        native.changeEnvironment(expanded, true);
        apron.changeEnvironment(expanded, true);
        requireEquivalent(native, apron, "environment-add");

        AD::VariableEnvironment projected = expanded.remove({y});
        native.changeEnvironment(projected);
        apron.changeEnvironment(projected);
        requireEquivalent(native, apron, "environment-project");

        const AD::Variable copy(5);
        const AD::VariableDeclaration copyDeclaration{
            copy, AD::NumericType::integer(), "v5"};
        native.expand(x, {copyDeclaration});
        apron.expand(x, {copyDeclaration});
        requireEquivalent(native, apron, "expand");
        native.fold(x, {copy});
        apron.fold(x, {copy});
        requireEquivalent(native, apron, "fold");

        std::cout << "APRON Octagon adapter differential: PASS\n";
        return EXIT_SUCCESS;
    }
    catch (const std::exception& error)
    {
        std::cerr << "APRON Octagon adapter differential: FAIL: "
                  << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
