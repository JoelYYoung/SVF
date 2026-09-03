//===- TreeExpression.h -- Domain-neutral expression trees -----*- C++ -*-===//

#ifndef SVF_AE_TREE_EXPRESSION_H
#define SVF_AE_TREE_EXPRESSION_H

#include "AE/Core/LinearExpression.h"

#include <memory>
#include <optional>
#include <vector>

namespace SVF::AbstractDomain
{

enum class UnaryOperator
{
    Negate,
    Cast,
    SquareRoot
};

enum class BinaryOperator
{
    Add,
    Subtract,
    Multiply,
    Divide,
    Remainder
};

class TreeExpression
{
public:
    enum class Kind
    {
        Constant,
        Variable,
        Unary,
        Binary
    };

    static TreeExpression constant(Rational value,
                                   NumericType type = NumericType::real());
    static TreeExpression variable(Variable value, NumericType type);
    static TreeExpression unary(
        UnaryOperator operation, TreeExpression operand, NumericType type,
        RoundingMode rounding = RoundingMode::NearestTiesToEven);
    static TreeExpression binary(
        BinaryOperator operation, TreeExpression lhs, TreeExpression rhs,
        NumericType type,
        RoundingMode rounding = RoundingMode::NearestTiesToEven);

    Kind kind() const
    {
        return kind_;
    }
    const NumericType& type() const
    {
        return type_;
    }
    const Rational& constant() const
    {
        return constant_;
    }
    Variable variable() const
    {
        return variable_;
    }
    UnaryOperator unaryOperator() const
    {
        return unaryOperator_;
    }
    BinaryOperator binaryOperator() const
    {
        return binaryOperator_;
    }
    RoundingMode roundingMode() const
    {
        return roundingMode_;
    }
    const TreeExpression& lhs() const;
    const TreeExpression& rhs() const;

    /// Return an exact affine expression when the tree is affine under
    /// mathematical integer/real semantics.  Floating and nonlinear trees
    /// deliberately return nullopt and must use a sound backend fallback.
    std::optional<LinearExpression> asLinear() const;

private:
    Kind kind_ = Kind::Constant;
    NumericType type_ = NumericType::real();
    Rational constant_;
    Variable variable_;
    UnaryOperator unaryOperator_ = UnaryOperator::Negate;
    BinaryOperator binaryOperator_ = BinaryOperator::Add;
    RoundingMode roundingMode_ = RoundingMode::NearestTiesToEven;
    std::shared_ptr<const TreeExpression> lhs_;
    std::shared_ptr<const TreeExpression> rhs_;
};
class TreeConstraint
{
public:
    TreeConstraint(TreeExpression expression, ConstraintKind kind);

    const TreeExpression& expression() const
    {
        return expression_;
    }
    ConstraintKind kind() const
    {
        return kind_;
    }

private:
    TreeExpression expression_;
    ConstraintKind kind_;
};
struct TreeAssignment
{
    Variable target;
    TreeExpression expression;
};

using TreeAssignmentList = std::vector<TreeAssignment>;

} // namespace SVF::AbstractDomain

#endif // SVF_AE_TREE_EXPRESSION_H
