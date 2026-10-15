//===- NumericalOperationTrace.cpp -- Opt-in workload trace -------------===//

#include "AE/Core/NumericalOperationTrace.h"

#include "AE/Core/Expression.h"

#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace SVF::AbstractDomain
{
namespace
{

using Bytes = std::string;

void appendByte(Bytes& bytes, std::uint8_t value)
{
    bytes.push_back(static_cast<char>(value));
}

void appendU32(Bytes& bytes, std::uint32_t value)
{
    for (unsigned index = 0; index < 4; ++index)
        appendByte(bytes, static_cast<std::uint8_t>(value >> (8 * index)));
}

void appendString(Bytes& bytes, const std::string& value)
{
    if (value.size() > std::numeric_limits<std::uint32_t>::max())
        throw std::length_error("numerical trace field is too large");
    appendU32(bytes, static_cast<std::uint32_t>(value.size()));
    bytes.append(value);
}

void appendType(Bytes& bytes, const NumericType& type)
{
    appendByte(bytes, static_cast<std::uint8_t>(type.kind));
    appendU32(bytes, type.floatFormat.exponentBits);
    appendU32(bytes, type.floatFormat.significandBits);
}

void appendVariable(Bytes& bytes, Variable variable)
{
    appendU32(bytes, variable.id());
    appendType(bytes, variable.type());
}

void appendExpression(Bytes& bytes, const LinearExpression& expression)
{
    appendString(bytes, expression.constant().toString());
    appendU32(bytes, static_cast<std::uint32_t>(expression.terms().size()));
    for (const auto& [variable, coefficient] : expression.terms())
    {
        appendVariable(bytes, variable);
        appendString(bytes, coefficient.toString());
    }
}

void appendConstraint(Bytes& bytes, const LinearConstraint& constraint)
{
    appendByte(bytes, static_cast<std::uint8_t>(constraint.kind()));
    appendExpression(bytes, constraint.expression());
}

void appendConstraints(Bytes& bytes, const LinearConstraintSet& constraints)
{
    appendU32(bytes, static_cast<std::uint32_t>(constraints.size()));
    for (const LinearConstraint& constraint : constraints)
        appendConstraint(bytes, constraint);
}

void appendTree(Bytes& bytes, const TreeExpression& expression)
{
    appendByte(bytes, static_cast<std::uint8_t>(expression.kind()));
    appendType(bytes, expression.type());
    appendByte(bytes, static_cast<std::uint8_t>(expression.roundingMode()));
    switch (expression.kind())
    {
    case TreeExpression::Kind::Constant:
        appendString(bytes, expression.constant().toString());
        return;
    case TreeExpression::Kind::Variable:
        appendVariable(bytes, expression.variable());
        return;
    case TreeExpression::Kind::Unary:
        appendByte(bytes,
                   static_cast<std::uint8_t>(expression.unaryOperator()));
        appendTree(bytes, expression.lhs());
        return;
    case TreeExpression::Kind::Binary:
        appendByte(bytes,
                   static_cast<std::uint8_t>(expression.binaryOperator()));
        appendTree(bytes, expression.lhs());
        appendTree(bytes, expression.rhs());
        return;
    }
}

void appendBound(Bytes& bytes, const Bound& bound)
{
    appendByte(bytes, static_cast<std::uint8_t>(bound.kind()));
    appendByte(bytes, bound.isStrict() ? 1U : 0U);
    appendString(bytes, bound.isFinite() ? bound.value().toString() : "");
}

void appendInterval(Bytes& bytes, const Interval& interval)
{
    appendBound(bytes, interval.lower());
    appendBound(bytes, interval.upper());
}

Bytes encodeVariable(Variable variable)
{
    Bytes bytes;
    appendVariable(bytes, variable);
    return bytes;
}

Bytes encodeVariables(const std::vector<Variable>& variables)
{
    Bytes bytes;
    appendU32(bytes, static_cast<std::uint32_t>(variables.size()));
    for (Variable variable : variables)
        appendVariable(bytes, variable);
    return bytes;
}

Bytes encodeLinear(Variable target, const LinearExpression& expression)
{
    Bytes bytes;
    appendVariable(bytes, target);
    appendExpression(bytes, expression);
    return bytes;
}

Bytes encodeTree(Variable target, const TreeExpression& expression)
{
    Bytes bytes;
    appendVariable(bytes, target);
    appendTree(bytes, expression);
    return bytes;
}

Bytes encodeLinearAssignments(const LinearAssignmentList& assignments)
{
    Bytes bytes;
    appendU32(bytes, static_cast<std::uint32_t>(assignments.size()));
    for (const LinearAssignment& assignment : assignments)
    {
        appendVariable(bytes, assignment.target);
        appendExpression(bytes, assignment.expression);
    }
    return bytes;
}

Bytes encodeTreeAssignments(const TreeAssignmentList& assignments)
{
    Bytes bytes;
    appendU32(bytes, static_cast<std::uint32_t>(assignments.size()));
    for (const TreeAssignment& assignment : assignments)
    {
        appendVariable(bytes, assignment.target);
        appendTree(bytes, assignment.expression);
    }
    return bytes;
}

Bytes encodeConstraint(const LinearConstraint& constraint)
{
    Bytes bytes;
    appendConstraint(bytes, constraint);
    return bytes;
}

Bytes encodeTreeConstraint(const TreeConstraint& constraint)
{
    Bytes bytes;
    appendByte(bytes, static_cast<std::uint8_t>(constraint.kind()));
    appendTree(bytes, constraint.expression());
    return bytes;
}

Bytes encodeConstraints(const LinearConstraintSet& constraints)
{
    Bytes bytes;
    appendConstraints(bytes, constraints);
    return bytes;
}

Bytes encodeExpand(Variable source, const std::vector<Variable>& copies)
{
    Bytes bytes;
    appendVariable(bytes, source);
    const Bytes variables = encodeVariables(copies);
    bytes.append(variables);
    return bytes;
}

Bytes encodeInterval(Variable target, const Interval& interval)
{
    Bytes bytes;
    appendVariable(bytes, target);
    appendInterval(bytes, interval);
    return bytes;
}

Bytes snapshot(const NumericalDomain& state)
{
    Bytes bytes;
    appendByte(bytes, static_cast<std::uint8_t>(state.kind()));
    appendByte(bytes, state.isBottom() ? 1U : 0U);
    appendConstraints(bytes, state.isBottom() ? LinearConstraintSet{}
                                              : state.toConstraints());
    return bytes;
}

std::string hexadecimal(const Bytes& bytes)
{
    static constexpr char Digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(bytes.size() * 2);
    for (unsigned char byte : bytes)
    {
        result.push_back(Digits[byte >> 4]);
        result.push_back(Digits[byte & 0xf]);
    }
    return result;
}

std::unique_ptr<NumericalDomain> cloneNumerical(const NumericalDomain& state)
{
    std::unique_ptr<AbstractDomain> clone = state.clone();
    return std::unique_ptr<NumericalDomain>(
        static_cast<NumericalDomain*>(clone.release()));
}

const char* checkResultName(CheckResult result)
{
    switch (result)
    {
    case CheckResult::False:
        return "false";
    case CheckResult::True:
        return "true";
    case CheckResult::Unknown:
        return "unknown";
    }
    return "unknown";
}

} // namespace

class TracingNumericalDomain final : public NumericalDomain
{
public:
    TracingNumericalDomain(
        std::unique_ptr<NumericalDomain> underlying,
        std::shared_ptr<NumericalOperationTraceWriter> writer)
        : underlying_(std::move(underlying)), writer_(std::move(writer)),
          state_(writer_->allocateState())
    {
        if (!underlying_)
            throw std::invalid_argument("cannot trace a null numerical domain");
        writer_->recordCreate(state_, *underlying_);
    }

    TracingNumericalDomain(const TracingNumericalDomain& other)
        : underlying_(cloneNumerical(*other.underlying_)),
          writer_(other.writer_), state_(writer_->allocateState())
    {
        writer_->recordClone(other.state_, state_, *underlying_);
        mirrorMetadata();
    }

    ~TracingNumericalDomain() override
    {
        writer_->recordDestroy(state_);
    }

    DomainKind kind() const noexcept override
    {
        return underlying_->kind();
    }

    std::unique_ptr<AbstractDomain> clone() const override
    {
        return std::make_unique<TracingNumericalDomain>(*this);
    }

    void assign(Variable target, const LinearExpression& expression) override
    {
        mutate(
            "assign-linear", encodeLinear(target, expression),
            [&](NumericalDomain& state) { state.assign(target, expression); });
    }

    void assign(Variable target, const TreeExpression& expression) override
    {
        mutate(
            "assign-tree", encodeTree(target, expression),
            [&](NumericalDomain& state) { state.assign(target, expression); });
    }

    void assignParallel(const LinearAssignmentList& assignments) override
    {
        mutate(
            "assign-parallel-linear", encodeLinearAssignments(assignments),
            [&](NumericalDomain& state) { state.assignParallel(assignments); });
    }

    void assignParallel(const TreeAssignmentList& assignments) override
    {
        mutate(
            "assign-parallel-tree", encodeTreeAssignments(assignments),
            [&](NumericalDomain& state) { state.assignParallel(assignments); });
    }

    void substitute(Variable target,
                    const LinearExpression& expression) override
    {
        mutate("substitute-linear", encodeLinear(target, expression),
               [&](NumericalDomain& state) {
                   state.substitute(target, expression);
               });
    }

    void substituteParallel(const LinearAssignmentList& assignments) override
    {
        mutate("substitute-parallel-linear",
               encodeLinearAssignments(assignments),
               [&](NumericalDomain& state) {
                   state.substituteParallel(assignments);
               });
    }

    void assume(const LinearConstraint& constraint) override
    {
        mutate("assume-linear", encodeConstraint(constraint),
               [&](NumericalDomain& state) { state.assume(constraint); });
    }

    void assume(const TreeConstraint& constraint) override
    {
        mutate("assume-tree", encodeTreeConstraint(constraint),
               [&](NumericalDomain& state) { state.assume(constraint); });
    }

    void assumeAll(const LinearConstraintSet& constraints) override
    {
        mutate("assume-all", encodeConstraints(constraints),
               [&](NumericalDomain& state) { state.assumeAll(constraints); });
    }

    void forget(Variable variable) override
    {
        mutate("forget", encodeVariable(variable),
               [&](NumericalDomain& state) { state.forget(variable); });
    }

    void project(const std::vector<Variable>& retained) override
    {
        mutate("project", encodeVariables(retained),
               [&](NumericalDomain& state) { state.project(retained); });
    }

    void expand(Variable source, const std::vector<Variable>& copies) override
    {
        mutate("expand", encodeExpand(source, copies),
               [&](NumericalDomain& state) { state.expand(source, copies); });
    }

    void fold(Variable target, const std::vector<Variable>& folded) override
    {
        mutate("fold", encodeExpand(target, folded),
               [&](NumericalDomain& state) { state.fold(target, folded); });
    }

    CheckResult entails(const LinearConstraint& constraint) const override
    {
        const CheckResult result = underlying_->entails(constraint);
        query("entails", encodeConstraint(constraint), checkResultName(result));
        return result;
    }

    Interval bound(Variable variable) const override
    {
        const Interval result = underlying_->bound(variable);
        Bytes encoded;
        appendInterval(encoded, result);
        query("bound-variable", encodeVariable(variable), hexadecimal(encoded));
        return result;
    }

    Interval bound(const LinearExpression& expression) const override
    {
        const Interval result = underlying_->bound(expression);
        Bytes payload;
        appendExpression(payload, expression);
        Bytes encoded;
        appendInterval(encoded, result);
        query("bound-linear", payload, hexadecimal(encoded));
        return result;
    }

    std::vector<Variable> supportVariables() const override
    {
        const std::vector<Variable> result = underlying_->supportVariables();
        query("support", {}, hexadecimal(encodeVariables(result)));
        return result;
    }

    LinearConstraintSet toConstraints() const override
    {
        const LinearConstraintSet result = underlying_->toConstraints();
        query("export-constraints", {}, hexadecimal(encodeConstraints(result)));
        return result;
    }

    void close() override
    {
        mutate("topological-close", {},
               [](NumericalDomain& state) { state.close(); });
    }

    void canonicalize() override
    {
        mutate("canonicalize", {},
               [](NumericalDomain& state) { state.canonicalize(); });
    }

protected:
    std::vector<Variable> relationalClosureState(
        const std::vector<Variable>& seeds) const override
    {
        const std::vector<Variable> result =
            underlying_->relationalClosure(seeds);
        query("relational-closure", encodeVariables(seeds),
              hexadecimal(encodeVariables(result)));
        return result;
    }

    void assignInterval(Variable target, const Interval& value) override
    {
        mutate(
            "assign-interval", encodeInterval(target, value),
            [&](NumericalDomain& state) { state.assignBound(target, value); });
    }

private:
    const void* dynamicTypeToken() const noexcept override
    {
        return staticTypeToken<TracingNumericalDomain>();
    }

    bool hasCompatibleDomain(const AbstractDomain& other) const override
    {
        if (!other.isDomain<TracingNumericalDomain>())
            return false;
        const auto& traced = static_cast<const TracingNumericalDomain&>(other);
        return underlying_->isCompatibleWith(*traced.underlying_);
    }

    void joinDomain(const AbstractDomain& other) override
    {
        lattice("join", other,
                [](NumericalDomain& left, const NumericalDomain& right) {
                    left.joinWith(right);
                });
    }

    void meetDomain(const AbstractDomain& other) override
    {
        lattice("meet", other,
                [](NumericalDomain& left, const NumericalDomain& right) {
                    left.meetWith(right);
                });
    }

    void widenDomain(const AbstractDomain& other) override
    {
        lattice("widen", other,
                [](NumericalDomain& left, const NumericalDomain& right) {
                    left.widenWith(right);
                });
    }

    void narrowDomain(const AbstractDomain& other) override
    {
        lattice("narrow", other,
                [](NumericalDomain& left, const NumericalDomain& right) {
                    left.narrowWith(right);
                });
    }

    bool isBottomDomain() const override
    {
        return underlying_->isBottom();
    }

    bool isTopDomain() const override
    {
        return underlying_->isTop();
    }

    bool leqDomain(const AbstractDomain& other) const override
    {
        const TracingNumericalDomain& rhs = requireTrace(other);
        const CheckResult result = underlying_->isSubsetOf(*rhs.underlying_);
        writer_->recordOperation("subset", state_, rhs.state_, {},
                                 underlying_.get(), rhs.underlying_.get(),
                                 nullptr, checkResultName(result), nullptr);
        return result == CheckResult::True;
    }

    std::string domainToString() const override
    {
        return underlying_->toString();
    }

    const TracingNumericalDomain& requireTrace(
        const AbstractDomain& other) const
    {
        requireCompatible(other);
        return static_cast<const TracingNumericalDomain&>(other);
    }

    template <typename Function>
    void mutate(const char* operation, const Bytes& payload, Function function)
    {
        std::unique_ptr<NumericalDomain> before = cloneNumerical(*underlying_);
        function(*underlying_);
        mirrorMetadata();
        const OperationMetadata& metadata = underlying_->lastOperation();
        writer_->recordOperation(operation, state_, 0, payload, before.get(),
                                 nullptr, underlying_.get(), {}, &metadata);
    }

    template <typename Function>
    void lattice(const char* operation, const AbstractDomain& other,
                 Function function)
    {
        const TracingNumericalDomain& rhs = requireTrace(other);
        std::unique_ptr<NumericalDomain> before = cloneNumerical(*underlying_);
        function(*underlying_, *rhs.underlying_);
        mirrorMetadata();
        const OperationMetadata& metadata = underlying_->lastOperation();
        writer_->recordOperation(operation, state_, rhs.state_, {},
                                 before.get(), rhs.underlying_.get(),
                                 underlying_.get(), {}, &metadata);
    }

    void query(const char* operation, const Bytes& payload,
               const std::string& result) const
    {
        writer_->recordOperation(operation, state_, 0, payload,
                                 underlying_.get(), nullptr, nullptr, result,
                                 nullptr);
    }

    void mirrorMetadata()
    {
        const OperationMetadata& metadata = underlying_->lastOperation();
        recordOperation(metadata.operation, metadata.approximation,
                        metadata.best, metadata.reason);
    }

    std::unique_ptr<NumericalDomain> underlying_;
    std::shared_ptr<NumericalOperationTraceWriter> writer_;
    std::uint64_t state_ = 0;
};

NumericalOperationTraceWriter::NumericalOperationTraceWriter(
    const std::string& path)
    : output_(path, std::ios::out | std::ios::trunc)
{
    if (!output_)
        throw std::runtime_error("cannot open numerical operation trace: " +
                                 path);
    output_ << "SVF-AE-NUMERICAL-TRACE\t1\n"
            << "sequence\tevent\tstate\trhs\toperation\tpayload_hex"
               "\tbefore_hex\trhs_hex\tafter_hex\tresult\tmetadata_operation"
               "\tapproximation\texact\tbest\treason_hex\n";
    output_.flush();
}

NumericalOperationTraceWriter::~NumericalOperationTraceWriter()
{
    std::lock_guard<std::mutex> lock(mutex_);
    output_.flush();
}

void NumericalOperationTraceWriter::suspend()
{
    std::lock_guard<std::mutex> lock(mutex_);
    ++suspensionDepth_;
}

void NumericalOperationTraceWriter::resume()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (suspensionDepth_ == 0)
        throw std::logic_error("numerical operation trace is not suspended");
    --suspensionDepth_;
}

std::uint64_t NumericalOperationTraceWriter::allocateState()
{
    std::lock_guard<std::mutex> lock(mutex_);
    return nextState_++;
}

void NumericalOperationTraceWriter::recordCreate(std::uint64_t state,
                                                 const NumericalDomain& value)
{
    writeRow("create", state, 0, {}, {}, hexadecimal(snapshot(value)), {}, {},
             {}, nullptr);
}

void NumericalOperationTraceWriter::recordClone(std::uint64_t source,
                                                std::uint64_t destination,
                                                const NumericalDomain& value)
{
    writeRow("clone", destination, source, {}, {}, hexadecimal(snapshot(value)),
             {}, {}, {}, nullptr);
}

void NumericalOperationTraceWriter::recordDestroy(std::uint64_t state)
{
    writeRow("destroy", state, 0, {}, {}, {}, {}, {}, {}, nullptr);
}

void NumericalOperationTraceWriter::recordOperation(
    const char* operation, std::uint64_t state, std::uint64_t rhs,
    const std::string& payload, const NumericalDomain* before,
    const NumericalDomain* rhsValue, const NumericalDomain* after,
    const std::string& result, const OperationMetadata* metadata)
{
    writeRow("operation", state, rhs, operation, hexadecimal(payload),
             before ? hexadecimal(snapshot(*before)) : std::string{},
             rhsValue ? hexadecimal(snapshot(*rhsValue)) : std::string{},
             after ? hexadecimal(snapshot(*after)) : std::string{}, result,
             metadata);
}

void NumericalOperationTraceWriter::writeRow(
    const char* event, std::uint64_t state, std::uint64_t rhs,
    const std::string& operation, const std::string& payload,
    const std::string& before, const std::string& rhsSnapshot,
    const std::string& after, const std::string& result,
    const OperationMetadata* metadata)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (suspensionDepth_ != 0)
        return;
    output_ << nextSequence_++ << '\t' << event << '\t' << state << '\t' << rhs
            << '\t' << operation << '\t' << payload << '\t' << before << '\t'
            << rhsSnapshot << '\t' << after << '\t' << result << '\t';
    if (metadata)
    {
        output_ << static_cast<unsigned>(metadata->operation) << '\t'
                << static_cast<unsigned>(metadata->approximation) << '\t'
                << (metadata->exact ? 1 : 0) << '\t' << (metadata->best ? 1 : 0)
                << '\t' << hexadecimal(metadata->reason);
    }
    else
        output_ << "\t\t\t\t";
    output_ << '\n';
    output_.flush();
    if (!output_)
        throw std::runtime_error("failed to write numerical operation trace");
}

std::unique_ptr<NumericalDomain> traceNumericalDomain(
    std::unique_ptr<NumericalDomain> domain,
    std::shared_ptr<NumericalOperationTraceWriter> writer)
{
    if (!writer)
        return domain;
    return std::make_unique<TracingNumericalDomain>(std::move(domain),
                                                    std::move(writer));
}

} // namespace SVF::AbstractDomain
