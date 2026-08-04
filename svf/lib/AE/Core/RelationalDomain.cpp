//===- RelationalDomain.cpp -- Relational numerical domain API ----------===//

#include "AE/Core/RelationalDomain.h"

#include <stdexcept>
#include <utility>

using namespace SVF;

RelationalExpr RelationalExpr::variableExpr(RelationalVariableId id,
                                            RelationalNumericType type)
{
    RelationalExpr result;
    result.kind = Kind::Variable;
    result.type = type;
    result.variable = id;
    return result;
}

RelationalExpr RelationalExpr::integerConstant(std::int64_t value)
{
    RelationalExpr result;
    result.kind = Kind::IntegerConstant;
    result.type = RelationalNumericType::Integer;
    result.literal = std::to_string(value);
    return result;
}

RelationalExpr RelationalExpr::realConstant(std::string value)
{
    if (value.empty())
        throw std::invalid_argument("a real constant cannot be empty");
    RelationalExpr result;
    result.kind = Kind::RealConstant;
    result.type = RelationalNumericType::Real;
    result.literal = std::move(value);
    return result;
}

RelationalExpr RelationalExpr::float32Constant(std::uint32_t bits)
{
    RelationalExpr result;
    result.kind = Kind::FloatConstant;
    result.type = RelationalNumericType::Float32;
    result.floatBits = bits;
    return result;
}

RelationalExpr RelationalExpr::float64Constant(std::uint64_t bits)
{
    RelationalExpr result;
    result.kind = Kind::FloatConstant;
    result.type = RelationalNumericType::Float64;
    result.floatBits = bits;
    return result;
}

RelationalExpr RelationalExpr::binary(RelationalBinaryOperator op,
                                      RelationalNumericType resultType,
                                      RelationalExpr left,
                                      RelationalExpr right,
                                      RelationalRoundingMode rounding)
{
    if (left.type != resultType || right.type != resultType)
        throw std::invalid_argument(
            "relational binary operands must match the result type");
    RelationalExpr result;
    result.kind = Kind::Binary;
    result.type = resultType;
    result.binaryOperator = op;
    result.roundingMode = rounding;
    result.lhs = std::make_shared<RelationalExpr>(std::move(left));
    result.rhs = std::make_shared<RelationalExpr>(std::move(right));
    return result;
}

const char* SVF::toString(RelationalCheckResult result)
{
    switch (result)
    {
    case RelationalCheckResult::True:
        return "true";
    case RelationalCheckResult::False:
        return "false";
    case RelationalCheckResult::Unknown:
        return "unknown";
    }
    return "unknown";
}
