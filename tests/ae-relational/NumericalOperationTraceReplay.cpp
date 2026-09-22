#include "AE/Core/Expression.h"
#include "AE/Core/NumericalDomainFactory.h"

#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace AD = SVF::AbstractDomain;

namespace
{

using Clock = std::chrono::steady_clock;
using Bytes = std::string;

class Reader
{
public:
    explicit Reader(Bytes bytes) : bytes_(std::move(bytes)) {}

    std::uint8_t byte()
    {
        require(1);
        return static_cast<std::uint8_t>(bytes_[position_++]);
    }

    std::uint32_t u32()
    {
        std::uint32_t value = 0;
        for (unsigned index = 0; index < 4; ++index)
            value |= static_cast<std::uint32_t>(byte()) << (8 * index);
        return value;
    }

    std::string string()
    {
        const std::uint32_t size = u32();
        require(size);
        const std::string value = bytes_.substr(position_, size);
        position_ += size;
        return value;
    }

    bool empty() const
    {
        return position_ == bytes_.size();
    }

private:
    void require(std::size_t count) const
    {
        if (count > bytes_.size() - position_)
            throw std::invalid_argument("truncated numerical trace payload");
    }

    Bytes bytes_;
    std::size_t position_ = 0;
};

Bytes unhex(const std::string& text)
{
    if (text.size() % 2 != 0)
        throw std::invalid_argument("odd-length numerical trace hex field");
    auto digit = [](char value) -> unsigned
    {
        if (value >= '0' && value <= '9')
            return static_cast<unsigned>(value - '0');
        if (value >= 'a' && value <= 'f')
            return static_cast<unsigned>(value - 'a' + 10);
        throw std::invalid_argument("invalid numerical trace hex digit");
    };
    Bytes bytes;
    bytes.reserve(text.size() / 2);
    for (std::size_t index = 0; index < text.size(); index += 2)
        bytes.push_back(static_cast<char>((digit(text[index]) << 4) |
                                          digit(text[index + 1])));
    return bytes;
}

std::vector<std::string> fields(const std::string& line)
{
    std::vector<std::string> result;
    std::size_t begin = 0;
    for (;;)
    {
        const std::size_t tab = line.find('\t', begin);
        if (tab == std::string::npos)
        {
            result.push_back(line.substr(begin));
            return result;
        }
        result.push_back(line.substr(begin, tab - begin));
        begin = tab + 1;
    }
}

AD::NumericType readType(Reader& reader)
{
    AD::NumericType type;
    const auto kind = reader.byte();
    if (kind > static_cast<std::uint8_t>(AD::NumericKind::IEEEFloat))
        throw std::invalid_argument("invalid trace numeric kind");
    type.kind = static_cast<AD::NumericKind>(kind);
    type.floatFormat.exponentBits = reader.u32();
    type.floatFormat.significandBits = reader.u32();
    return type;
}

AD::Variable readVariable(Reader& reader)
{
    const std::uint32_t identifier = reader.u32();
    const AD::NumericType type = readType(reader);
    return AD::Variable(identifier, type);
}

AD::LinearExpression readExpression(Reader& reader)
{
    AD::LinearExpression expression(AD::Rational(reader.string()));
    const std::uint32_t terms = reader.u32();
    for (std::uint32_t index = 0; index < terms; ++index)
    {
        const AD::Variable variable = readVariable(reader);
        const AD::Rational coefficient(reader.string());
        expression.setCoefficient(variable, coefficient);
    }
    return expression;
}

AD::ConstraintKind readConstraintKind(Reader& reader)
{
    const auto kind = reader.byte();
    if (kind > static_cast<std::uint8_t>(AD::ConstraintKind::GreaterEqual))
        throw std::invalid_argument("invalid trace constraint kind");
    return static_cast<AD::ConstraintKind>(kind);
}

AD::LinearConstraint readConstraint(Reader& reader)
{
    const AD::ConstraintKind kind = readConstraintKind(reader);
    return AD::LinearConstraint(readExpression(reader), kind);
}

AD::LinearConstraintSet readConstraints(Reader& reader)
{
    AD::LinearConstraintSet constraints;
    const std::uint32_t count = reader.u32();
    constraints.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index)
        constraints.push_back(readConstraint(reader));
    return constraints;
}

AD::TreeExpression readTree(Reader& reader)
{
    const auto kindValue = reader.byte();
    if (kindValue > static_cast<std::uint8_t>(AD::TreeExpression::Kind::Binary))
        throw std::invalid_argument("invalid trace tree kind");
    const auto kind = static_cast<AD::TreeExpression::Kind>(kindValue);
    const AD::NumericType type = readType(reader);
    const auto roundingValue = reader.byte();
    if (roundingValue > static_cast<std::uint8_t>(AD::RoundingMode::TowardNegative))
        throw std::invalid_argument("invalid trace rounding mode");
    const auto rounding = static_cast<AD::RoundingMode>(roundingValue);
    switch (kind)
    {
    case AD::TreeExpression::Kind::Constant:
        return AD::TreeExpression::constant(AD::Rational(reader.string()), type);
    case AD::TreeExpression::Kind::Variable:
        return AD::TreeExpression::variable(readVariable(reader), type);
    case AD::TreeExpression::Kind::Unary:
    {
        const auto operationValue = reader.byte();
        if (operationValue >
            static_cast<std::uint8_t>(AD::UnaryOperator::SquareRoot))
            throw std::invalid_argument("invalid trace unary operator");
        const auto operation = static_cast<AD::UnaryOperator>(operationValue);
        return AD::TreeExpression::unary(operation, readTree(reader), type,
                                         rounding);
    }
    case AD::TreeExpression::Kind::Binary:
    {
        const auto operationValue = reader.byte();
        if (operationValue >
            static_cast<std::uint8_t>(AD::BinaryOperator::Remainder))
            throw std::invalid_argument("invalid trace binary operator");
        const auto operation = static_cast<AD::BinaryOperator>(operationValue);
        AD::TreeExpression lhs = readTree(reader);
        AD::TreeExpression rhs = readTree(reader);
        return AD::TreeExpression::binary(operation, std::move(lhs),
                                          std::move(rhs), type, rounding);
    }
    }
    throw std::invalid_argument("unreachable trace tree kind");
}

AD::Bound readBound(Reader& reader)
{
    const auto kindValue = reader.byte();
    const bool strict = reader.byte() != 0;
    const std::string value = reader.string();
    switch (static_cast<AD::Bound::Kind>(kindValue))
    {
    case AD::Bound::Kind::MinusInfinity:
        return AD::Bound::minusInfinity();
    case AD::Bound::Kind::Finite:
        return AD::Bound::finite(AD::Rational(value), strict);
    case AD::Bound::Kind::PlusInfinity:
        return AD::Bound::plusInfinity();
    }
    throw std::invalid_argument("invalid trace bound kind");
}

AD::Interval readInterval(Reader& reader)
{
    const AD::Bound lower = readBound(reader);
    const AD::Bound upper = readBound(reader);
    return AD::Interval(lower, upper);
}

std::vector<AD::Variable> readVariables(Reader& reader)
{
    std::vector<AD::Variable> variables;
    const std::uint32_t count = reader.u32();
    variables.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index)
        variables.push_back(readVariable(reader));
    return variables;
}

AD::LinearAssignmentList readLinearAssignments(Reader& reader)
{
    AD::LinearAssignmentList assignments;
    const std::uint32_t count = reader.u32();
    assignments.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index)
    {
        const AD::Variable target = readVariable(reader);
        AD::LinearExpression expression = readExpression(reader);
        assignments.push_back({target, std::move(expression)});
    }
    return assignments;
}

AD::TreeAssignmentList readTreeAssignments(Reader& reader)
{
    AD::TreeAssignmentList assignments;
    const std::uint32_t count = reader.u32();
    assignments.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index)
    {
        const AD::Variable target = readVariable(reader);
        AD::TreeExpression expression = readTree(reader);
        assignments.push_back({target, std::move(expression)});
    }
    return assignments;
}

struct Snapshot
{
    AD::DomainKind kind = AD::DomainKind::Box;
    bool bottom = false;
    AD::LinearConstraintSet constraints;
};

Snapshot readSnapshot(const std::string& encoded)
{
    Reader reader(unhex(encoded));
    const auto kind = reader.byte();
    if (kind > static_cast<std::uint8_t>(AD::DomainKind::ConvexPolyhedra))
        throw std::invalid_argument("invalid trace domain kind");
    Snapshot result;
    result.kind = static_cast<AD::DomainKind>(kind);
    result.bottom = reader.byte() != 0;
    result.constraints = readConstraints(reader);
    if (!reader.empty())
        throw std::invalid_argument("trailing bytes in trace snapshot");
    return result;
}

std::unique_ptr<AD::NumericalDomain> makeState(
    const Snapshot& snapshot, AD::NumericalBackendKind backend)
{
    if (!AD::numericalBackendAvailable(snapshot.kind, backend))
        throw std::invalid_argument(
            "selected backend is unavailable for a traced domain");
    std::unique_ptr<AD::NumericalDomain> state =
        AD::makeNumericalDomain(snapshot.kind, snapshot.bottom, backend);
    if (!snapshot.bottom)
        state->assumeAll(snapshot.constraints);
    return state;
}

std::unique_ptr<AD::NumericalDomain> cloneNumerical(
    const AD::NumericalDomain& state)
{
    std::unique_ptr<AD::AbstractDomain> clone = state.clone();
    return std::unique_ptr<AD::NumericalDomain>(
               static_cast<AD::NumericalDomain*>(clone.release()));
}

const char* checkName(AD::CheckResult result)
{
    return AD::toString(result);
}

struct Execution
{
    std::uint64_t checksum = 0;
    const char* validation = "NA";
};

Execution execute(const std::string& operation, Reader& payload,
                  AD::NumericalDomain& state,
                  const AD::NumericalDomain* rhs,
                  const std::string& recordedResult)
{
    Execution result;
    if (operation == "assign-linear")
        state.assign(readVariable(payload), readExpression(payload));
    else if (operation == "assign-tree")
        state.assign(readVariable(payload), readTree(payload));
    else if (operation == "assign-parallel-linear")
        state.assignParallel(readLinearAssignments(payload));
    else if (operation == "assign-parallel-tree")
        state.assignParallel(readTreeAssignments(payload));
    else if (operation == "substitute-linear")
        state.substitute(readVariable(payload), readExpression(payload));
    else if (operation == "substitute-parallel-linear")
        state.substituteParallel(readLinearAssignments(payload));
    else if (operation == "assume-linear")
        state.assume(readConstraint(payload));
    else if (operation == "assume-tree")
    {
        const AD::ConstraintKind kind = readConstraintKind(payload);
        state.assume(AD::TreeConstraint(readTree(payload), kind));
    }
    else if (operation == "assume-all")
        state.assumeAll(readConstraints(payload));
    else if (operation == "forget")
        state.forget(readVariable(payload));
    else if (operation == "project")
        state.project(readVariables(payload));
    else if (operation == "expand" || operation == "fold")
    {
        const AD::Variable variable = readVariable(payload);
        const std::vector<AD::Variable> variables = readVariables(payload);
        if (operation == "expand")
            state.expand(variable, variables);
        else
            state.fold(variable, variables);
    }
    else if (operation == "bound-variable" || operation == "bound-linear")
    {
        const AD::Interval actual = operation == "bound-variable"
            ? state.bound(readVariable(payload))
            : state.bound(readExpression(payload));
        Reader expectedPayload(unhex(recordedResult));
        const AD::Interval expected = readInterval(expectedPayload);
        if (!expectedPayload.empty())
            throw std::invalid_argument("trailing bytes in traced bound result");
        result.checksum = actual.toString().size();
        result.validation = actual == expected ? "true" : "false";
    }
    else if (operation == "support")
    {
        const std::vector<AD::Variable> actual = state.supportVariables();
        Reader expectedPayload(unhex(recordedResult));
        const std::vector<AD::Variable> expected =
            readVariables(expectedPayload);
        if (!expectedPayload.empty())
            throw std::invalid_argument("trailing bytes in traced support result");
        result.checksum = actual.size();
        result.validation = actual == expected ? "true" : "false";
    }
    else if (operation == "export-constraints")
        result.checksum = state.toConstraints().size();
    else if (operation == "topological-close")
        state.close();
    else if (operation == "canonicalize")
        state.canonicalize();
    else if (operation == "assign-interval")
        state.assignBound(readVariable(payload), readInterval(payload));
    else if (operation == "relational-closure")
    {
        const std::vector<AD::Variable> actual =
            state.relationalClosure(readVariables(payload));
        Reader expectedPayload(unhex(recordedResult));
        const std::vector<AD::Variable> expected =
            readVariables(expectedPayload);
        if (!expectedPayload.empty())
            throw std::invalid_argument("trailing bytes in traced closure result");
        result.checksum = actual.size();
        result.validation = actual == expected ? "true" : "false";
    }
    else if (operation == "entails")
    {
        const AD::CheckResult actual = state.entails(readConstraint(payload));
        result.checksum = static_cast<std::uint64_t>(actual);
        result.validation = recordedResult == checkName(actual) ? "true" : "false";
    }
    else if (operation == "join" || operation == "meet" ||
             operation == "widen" || operation == "narrow" ||
             operation == "subset")
    {
        if (!rhs)
            throw std::invalid_argument("binary trace operation has no RHS");
        if (operation == "join")
            state.joinWith(*rhs);
        else if (operation == "meet")
            state.meetWith(*rhs);
        else if (operation == "widen")
            state.widenWith(*rhs);
        else if (operation == "narrow")
            state.narrowWith(*rhs);
        else
        {
            const AD::CheckResult actual = state.isSubsetOf(*rhs);
            result.checksum = static_cast<std::uint64_t>(actual);
            result.validation =
                recordedResult == checkName(actual) ? "true" : "false";
        }
    }
    else
        throw std::invalid_argument("unsupported trace operation: " + operation);
    if (!payload.empty())
        throw std::invalid_argument("trailing bytes in trace operation payload");
    return result;
}

AD::NumericalBackendKind parseBackend(const std::string& name)
{
    if (name == "native")
        return AD::NumericalBackendKind::Native;
    if (name == "elina")
        return AD::NumericalBackendKind::Elina;
    throw std::invalid_argument("backend must be native or elina");
}

const char* domainName(AD::DomainKind kind)
{
    switch (kind)
    {
    case AD::DomainKind::Box:
        return "box";
    case AD::DomainKind::Octagon:
        return "octagon";
    case AD::DomainKind::ConvexPolyhedra:
        return "polyhedra";
    default:
        throw std::invalid_argument("trace snapshot is not a numerical domain");
    }
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 3)
    {
        std::cerr << "usage: " << argv[0] << " TRACE.tsv native|elina\n";
        return 2;
    }
    try
    {
        const AD::NumericalBackendKind backend = parseBackend(argv[2]);
        std::ifstream input(argv[1]);
        if (!input)
            throw std::runtime_error("cannot open numerical operation trace");
        std::string line;
        if (!std::getline(input, line) || line != "SVF-AE-NUMERICAL-TRACE\t1")
            throw std::invalid_argument("unsupported numerical trace version");
        if (!std::getline(input, line) ||
            line != "sequence\tevent\tstate\trhs\toperation\tpayload_hex"
                    "\tbefore_hex\trhs_hex\tafter_hex\tresult\tmetadata_operation"
                    "\tapproximation\texact\tbest\treason_hex")
            throw std::invalid_argument("numerical trace header is missing");

        std::cout << "sequence\tbackend\tdomain\tevent\toperation\telapsed_ns"
                     "\tvalidation\tinput_constraints\tchecksum\n";
        std::size_t rows = 0;
        while (std::getline(input, line))
        {
            const std::vector<std::string> row = fields(line);
            if (row.size() != 15)
                throw std::invalid_argument("numerical trace row is not 15 columns");
            const std::string& sequence = row[0];
            const std::string& event = row[1];
            const std::string& operation = row[4];
            if (event != "operation" && event != "clone")
                continue;
            const Snapshot before = readSnapshot(row[6]);
            if (before.kind == AD::DomainKind::Box &&
                    backend == AD::NumericalBackendKind::Elina)
                continue;
            std::unique_ptr<AD::NumericalDomain> state =
                makeState(before, backend);
            std::unique_ptr<AD::NumericalDomain> rhs;
            if (!row[7].empty())
                rhs = makeState(readSnapshot(row[7]), backend);

            std::uint64_t checksum = 0;
            const char* validation = "NA";
            const Clock::time_point start = Clock::now();
            if (event == "clone")
            {
                std::unique_ptr<AD::NumericalDomain> copy =
                    cloneNumerical(*state);
                checksum = static_cast<std::uint64_t>(copy->isBottom());
                validation = checkName(copy->isEquivalentTo(*state));
            }
            else
            {
                Reader payload(unhex(row[5]));
                const Execution execution =
                    execute(operation, payload, *state, rhs.get(), row[9]);
                checksum = execution.checksum;
                validation = execution.validation;
            }
            const Clock::time_point finish = Clock::now();
            const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     finish - start).count();

            if (!row[8].empty() && event == "operation")
            {
                std::unique_ptr<AD::NumericalDomain> expected =
                    makeState(readSnapshot(row[8]), backend);
                validation = checkName(state->isEquivalentTo(*expected));
            }
            std::cout << sequence << '\t' << AD::numericalBackendName(backend)
                      << '\t' << domainName(before.kind) << '\t'
                      << event << '\t'
                      << (event == "clone" ? "clone" : operation) << '\t'
                      << elapsed << '\t' << validation << '\t'
                      << before.constraints.size() << '\t' << checksum << '\n';
            ++rows;
        }
        if (rows == 0)
            throw std::runtime_error("numerical trace contained no replayable rows");
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "NumericalOperationTraceReplay: " << error.what() << '\n';
        return 1;
    }
}
