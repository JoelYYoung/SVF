//===- RelationalEnvironment.cpp -- Variables and dimensions ------------===//

#include "AE/Core/RelationalEnvironment.h"

#include <algorithm>
#include <map>
#include <stdexcept>
#include <utility>

using namespace relational;

struct Environment::Data
{
    explicit Data(std::vector<VariableDeclaration> declarations)
        : variables(std::move(declarations))
    {
        std::sort(variables.begin(), variables.end(),
                  [](const VariableDeclaration& lhs,
                     const VariableDeclaration& rhs)
                  { return lhs.variable < rhs.variable; });
        for (Dimension dimension = 0; dimension < variables.size(); ++dimension)
        {
            const auto [it, inserted] =
                dimensions.emplace(variables[dimension].variable, dimension);
            (void)it;
            if (!inserted)
                throw std::invalid_argument(
                    "duplicate variable in relational environment");
        }
    }

    std::vector<VariableDeclaration> variables;
    std::map<Variable, Dimension> dimensions;
};

Environment::Environment() : data_(std::make_shared<Data>(
                                      std::vector<VariableDeclaration>{}))
{
}
Environment::Environment(std::vector<VariableDeclaration> variables)
    : data_(std::make_shared<Data>(std::move(variables)))
{
}

std::size_t Environment::size() const
{
    return data_->variables.size();
}

bool Environment::contains(Variable variable) const
{
    return data_->dimensions.find(variable) != data_->dimensions.end();
}

Dimension Environment::dimensionOf(Variable variable) const
{
    const auto it = data_->dimensions.find(variable);
    if (it == data_->dimensions.end())
        throw std::out_of_range("variable is not in relational environment");
    return it->second;
}

Variable Environment::variableOf(Dimension dimension) const
{
    if (dimension >= size())
        throw std::out_of_range("invalid relational dimension");
    return data_->variables[dimension].variable;
}

const NumericType& Environment::typeOf(Variable variable) const
{
    return data_->variables[dimensionOf(variable)].type;
}

const std::string& Environment::nameOf(Variable variable) const
{
    return data_->variables[dimensionOf(variable)].name;
}

const std::vector<VariableDeclaration>& Environment::variables() const
{
    return data_->variables;
}

Environment Environment::add(
    std::vector<VariableDeclaration> declarations) const
{
    std::vector<VariableDeclaration> combined = data_->variables;
    combined.insert(combined.end(), std::make_move_iterator(declarations.begin()),
                    std::make_move_iterator(declarations.end()));
    return Environment(std::move(combined));
}

Environment Environment::remove(const std::vector<Variable>& removed) const
{
    std::vector<VariableDeclaration> remaining;
    remaining.reserve(size());
    for (const VariableDeclaration& declaration : data_->variables)
    {
        if (std::find(removed.begin(), removed.end(), declaration.variable) ==
                removed.end())
            remaining.push_back(declaration);
    }
    return Environment(std::move(remaining));
}

Environment Environment::merge(const Environment& other) const
{
    std::map<Variable, VariableDeclaration> combined;
    for (const VariableDeclaration& declaration : data_->variables)
        combined.emplace(declaration.variable, declaration);
    for (const VariableDeclaration& declaration : other.data_->variables)
    {
        const auto [it, inserted] =
            combined.emplace(declaration.variable, declaration);
        if (!inserted && it->second.type != declaration.type)
            throw std::invalid_argument(
                "cannot merge relational environments with conflicting types");
    }

    std::vector<VariableDeclaration> declarations;
    declarations.reserve(combined.size());
    for (auto& entry : combined)
        declarations.push_back(std::move(entry.second));
    return Environment(std::move(declarations));
}

bool relational::operator==(const Environment& lhs, const Environment& rhs)
{
    if (lhs.data_ == rhs.data_)
        return true;
    if (lhs.data_->variables.size() != rhs.data_->variables.size())
        return false;
    for (std::size_t index = 0; index < lhs.data_->variables.size(); ++index)
    {
        const VariableDeclaration& left = lhs.data_->variables[index];
        const VariableDeclaration& right = rhs.data_->variables[index];
        if (left.variable != right.variable || left.type != right.type ||
                left.name != right.name)
            return false;
    }
    return true;
}
