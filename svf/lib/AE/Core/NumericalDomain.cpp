//===- NumericalDomain.cpp -- Numerical-state serialization ------------===//

#include "AE/Core/NumericalDomain.h"

#include "AE/Core/BoxDomain.h"
#include "AE/Core/ConvexPolyhedraDomain.h"
#include "AE/Core/OctagonDomain.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace SVF::AbstractDomain;

namespace
{

constexpr std::array<std::uint8_t, 8> RawMagic{'S', 'V', 'F', 'A',
                                               'D', 'R', 'A', 'W'};
constexpr std::uint16_t RawVersion = 1;
constexpr std::uint32_t MaxCollectionEntries = 1U << 20;
constexpr std::uint64_t FnvOffset = 14695981039346656037ULL;
constexpr std::uint64_t FnvPrime = 1099511628211ULL;

enum class DomainTag : std::uint8_t
{
    Box = 1,
    Octagon = 2,
    ConvexPolyhedra = 3
};

std::uint64_t fnv1a(const std::uint8_t* data, std::size_t size)
{
    std::uint64_t result = FnvOffset;
    for (std::size_t index = 0; index < size; ++index)
    {
        result ^= data[index];
        result *= FnvPrime;
    }
    return result;
}

class Writer
{
public:
    void writeByte(std::uint8_t value)
    {
        bytes_.push_back(value);
    }

    void writeU16(std::uint16_t value)
    {
        for (unsigned shift = 0; shift < 16; shift += 8)
            writeByte(static_cast<std::uint8_t>(value >> shift));
    }

    void writeU32(std::uint32_t value)
    {
        for (unsigned shift = 0; shift < 32; shift += 8)
            writeByte(static_cast<std::uint8_t>(value >> shift));
    }

    void writeU64(std::uint64_t value)
    {
        for (unsigned shift = 0; shift < 64; shift += 8)
            writeByte(static_cast<std::uint8_t>(value >> shift));
    }

    void writeString(const std::string& value)
    {
        if (value.size() > std::numeric_limits<std::uint32_t>::max())
            throw std::length_error("raw state string is too large");
        writeU32(static_cast<std::uint32_t>(value.size()));
        bytes_.insert(bytes_.end(), value.begin(), value.end());
    }

    void writeMagic()
    {
        bytes_.insert(bytes_.end(), RawMagic.begin(), RawMagic.end());
    }

    NumericalState::RawBuffer finish()
    {
        const std::uint64_t checksum = fnv1a(bytes_.data(), bytes_.size());
        writeU64(checksum);
        return std::move(bytes_);
    }

private:
    NumericalState::RawBuffer bytes_;
};

std::uint64_t readTrailingU64(const NumericalState::RawBuffer& buffer)
{
    if (buffer.size() < sizeof(std::uint64_t))
        throw std::invalid_argument("raw state buffer is truncated");
    std::uint64_t value = 0;
    const std::size_t offset = buffer.size() - sizeof(std::uint64_t);
    for (unsigned index = 0; index < sizeof(std::uint64_t); ++index)
        value |= static_cast<std::uint64_t>(buffer[offset + index])
                 << (8 * index);
    return value;
}

class Reader
{
public:
    explicit Reader(const NumericalState::RawBuffer& bytes)
        : bytes_(bytes), limit_(checkedLimit(bytes))
    {
        const std::uint64_t expected = readTrailingU64(bytes_);
        const std::uint64_t actual = fnv1a(bytes_.data(), limit_);
        if (actual != expected)
            throw std::invalid_argument("raw state checksum mismatch");
    }

    void readMagic()
    {
        for (std::uint8_t expected : RawMagic)
        {
            if (readByte() != expected)
                throw std::invalid_argument("raw state has invalid magic");
        }
    }

    std::uint8_t readByte()
    {
        require(1);
        return bytes_[position_++];
    }

    std::uint16_t readU16()
    {
        std::uint16_t value = 0;
        for (unsigned index = 0; index < sizeof(value); ++index)
            value |= static_cast<std::uint16_t>(readByte()) << (8 * index);
        return value;
    }

    std::uint32_t readU32()
    {
        std::uint32_t value = 0;
        for (unsigned index = 0; index < sizeof(value); ++index)
            value |= static_cast<std::uint32_t>(readByte()) << (8 * index);
        return value;
    }

    std::string readString()
    {
        const std::uint32_t size = readU32();
        require(size);
        const auto begin =
            bytes_.begin() + static_cast<std::ptrdiff_t>(position_);
        position_ += size;
        return std::string(begin, begin + size);
    }

    bool empty() const
    {
        return position_ == limit_;
    }

private:
    static std::size_t checkedLimit(const NumericalState::RawBuffer& bytes)
    {
        if (bytes.size() <
            RawMagic.size() + sizeof(std::uint16_t) + 2 + sizeof(std::uint64_t))
            throw std::invalid_argument("raw state buffer is truncated");
        return bytes.size() - sizeof(std::uint64_t);
    }

    void require(std::size_t size) const
    {
        if (size > limit_ - position_)
            throw std::invalid_argument("raw state buffer is truncated");
    }

    const NumericalState::RawBuffer& bytes_;
    std::size_t limit_;
    std::size_t position_ = 0;
};

std::uint8_t encodeKind(NumericKind kind)
{
    switch (kind)
    {
    case NumericKind::Integer:
        return 0;
    case NumericKind::Real:
        return 1;
    case NumericKind::IEEEFloat:
        return 2;
    }
    throw std::logic_error("unknown numerical kind");
}

NumericKind decodeKind(std::uint8_t value)
{
    switch (value)
    {
    case 0:
        return NumericKind::Integer;
    case 1:
        return NumericKind::Real;
    case 2:
        return NumericKind::IEEEFloat;
    default:
        throw std::invalid_argument("raw state has invalid numerical kind");
    }
}

std::uint8_t encodeConstraintKind(ConstraintKind kind)
{
    switch (kind)
    {
    case ConstraintKind::Equal:
        return 0;
    case ConstraintKind::NotEqual:
        return 1;
    case ConstraintKind::LessThan:
        return 2;
    case ConstraintKind::LessEqual:
        return 3;
    case ConstraintKind::GreaterThan:
        return 4;
    case ConstraintKind::GreaterEqual:
        return 5;
    }
    throw std::logic_error("unknown linear constraint kind");
}

ConstraintKind decodeConstraintKind(std::uint8_t value)
{
    switch (value)
    {
    case 0:
        return ConstraintKind::Equal;
    case 1:
        return ConstraintKind::NotEqual;
    case 2:
        return ConstraintKind::LessThan;
    case 3:
        return ConstraintKind::LessEqual;
    case 4:
        return ConstraintKind::GreaterThan;
    case 5:
        return ConstraintKind::GreaterEqual;
    default:
        throw std::invalid_argument("raw state has invalid constraint kind");
    }
}

DomainTag domainTag(const NumericalState& state)
{
    if (dynamic_cast<const BoxState*>(&state))
        return DomainTag::Box;
    if (dynamic_cast<const OctagonState*>(&state))
        return DomainTag::Octagon;
    if (dynamic_cast<const ConvexPolyhedraState*>(&state))
        return DomainTag::ConvexPolyhedra;
    throw std::invalid_argument(
        "raw serialization does not support this domain");
}

std::uint8_t configurationFlags(const NumericalState& state, DomainTag tag)
{
    switch (tag)
    {
    case DomainTag::Box: {
        const auto& box = static_cast<const BoxState&>(state);
        return box.config().integerTightening ? 1U : 0U;
    }
    case DomainTag::Octagon: {
        const auto& octagon = static_cast<const OctagonState&>(state);
        return (octagon.config().integerTightening ? 1U : 0U) |
               (octagon.config().strongClosure ? 2U : 0U);
    }
    case DomainTag::ConvexPolyhedra:
        return static_cast<const ConvexPolyhedraState&>(state)
                       .config()
                       .integerTightening
                   ? 1U
                   : 0U;
    }
    throw std::logic_error("unknown raw state domain tag");
}

void writeEnvironment(Writer& writer, const VariableEnvironment& environment)
{
    if (environment.size() > std::numeric_limits<std::uint32_t>::max())
        throw std::length_error("raw state environment is too large");
    writer.writeU32(static_cast<std::uint32_t>(environment.size()));
    for (const VariableDeclaration& declaration : environment.variables())
    {
        writer.writeU32(declaration.variable.id());
        writer.writeByte(encodeKind(declaration.type.kind));
        writer.writeU32(declaration.type.floatFormat.exponentBits);
        writer.writeU32(declaration.type.floatFormat.significandBits);
        writer.writeString(declaration.name);
    }
}

VariableEnvironment readEnvironment(Reader& reader)
{
    const std::uint32_t count = reader.readU32();
    if (count > MaxCollectionEntries)
        throw std::invalid_argument("raw state environment is too large");
    std::vector<VariableDeclaration> declarations;
    declarations.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index)
    {
        const Variable variable(reader.readU32());
        NumericType type;
        type.kind = decodeKind(reader.readByte());
        type.floatFormat.exponentBits = reader.readU32();
        type.floatFormat.significandBits = reader.readU32();
        declarations.push_back({variable, type, reader.readString()});
    }
    return VariableEnvironment(std::move(declarations));
}

void writeConstraints(Writer& writer, const LinearConstraintSet& constraints)
{
    if (constraints.size() > std::numeric_limits<std::uint32_t>::max())
        throw std::length_error("raw state has too many constraints");
    writer.writeU32(static_cast<std::uint32_t>(constraints.size()));
    for (const LinearConstraint& constraint : constraints)
    {
        writer.writeByte(encodeConstraintKind(constraint.kind()));
        writer.writeString(constraint.expression().constant().toString());
        const auto& terms = constraint.expression().terms();
        if (terms.size() > std::numeric_limits<std::uint32_t>::max())
            throw std::length_error("raw state constraint has too many terms");
        writer.writeU32(static_cast<std::uint32_t>(terms.size()));
        for (const auto& [variable, coefficient] : terms)
        {
            writer.writeU32(variable.id());
            writer.writeString(coefficient.toString());
        }
    }
}

Rational readRational(Reader& reader)
{
    const std::string encoded = reader.readString();
    if (encoded.empty())
        throw std::invalid_argument("raw state contains an empty rational");
    try
    {
        return Rational(encoded);
    }
    catch (const std::exception&)
    {
        throw std::invalid_argument("raw state contains an invalid rational");
    }
}

LinearConstraintSet readConstraints(Reader& reader,
                                    const VariableEnvironment& environment)
{
    const std::uint32_t count = reader.readU32();
    if (count > MaxCollectionEntries)
        throw std::invalid_argument("raw state has too many constraints");
    LinearConstraintSet constraints;
    constraints.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index)
    {
        const ConstraintKind kind = decodeConstraintKind(reader.readByte());
        LinearExpression expression(readRational(reader));
        const std::uint32_t termCount = reader.readU32();
        if (termCount > environment.size())
            throw std::invalid_argument(
                "raw state constraint has too many terms");
        std::set<Variable> seen;
        for (std::uint32_t term = 0; term < termCount; ++term)
        {
            const Variable variable(reader.readU32());
            if (!environment.contains(variable))
                throw std::invalid_argument(
                    "raw state constraint uses an unknown variable");
            if (!seen.insert(variable).second)
                throw std::invalid_argument(
                    "raw state constraint repeats a variable");
            expression.setCoefficient(variable, readRational(reader));
        }
        constraints.emplace_back(std::move(expression), kind);
    }
    return constraints;
}

DomainTag decodeDomainTag(std::uint8_t value)
{
    switch (value)
    {
    case static_cast<std::uint8_t>(DomainTag::Box):
        return DomainTag::Box;
    case static_cast<std::uint8_t>(DomainTag::Octagon):
        return DomainTag::Octagon;
    case static_cast<std::uint8_t>(DomainTag::ConvexPolyhedra):
        return DomainTag::ConvexPolyhedra;
    default:
        throw std::invalid_argument("raw state has an unknown domain tag");
    }
}

std::unique_ptr<NumericalState> restore(DomainTag tag, std::uint8_t flags,
                                        const VariableEnvironment& environment,
                                        bool bottom,
                                        const LinearConstraintSet& constraints)
{
    switch (tag)
    {
    case DomainTag::Box: {
        if ((flags & ~1U) != 0)
            throw std::invalid_argument("raw Box state has invalid flags");
        BoxConfig config;
        config.integerTightening = (flags & 1U) != 0;
        BoxState state =
            bottom
                ? BoxState::bottom(environment, config)
                : BoxState::fromConstraints(environment, constraints, config);
        return std::make_unique<BoxState>(std::move(state));
    }
    case DomainTag::Octagon: {
        if ((flags & ~3U) != 0)
            throw std::invalid_argument("raw Octagon state has invalid flags");
        OctagonConfig config;
        config.integerTightening = (flags & 1U) != 0;
        config.strongClosure = (flags & 2U) != 0;
        OctagonState state = bottom ? OctagonState::bottom(environment, config)
                                    : OctagonState::fromConstraints(
                                          environment, constraints, config);
        return std::make_unique<OctagonState>(std::move(state));
    }
    case DomainTag::ConvexPolyhedra: {
        if ((flags & ~1U) != 0)
            throw std::invalid_argument(
                "raw Convex Polyhedra state has invalid flags");
        ConvexPolyhedraConfig config;
        config.integerTightening = (flags & 1U) != 0;
        ConvexPolyhedraState state =
            bottom ? ConvexPolyhedraState::bottom(environment, config)
                   : ConvexPolyhedraState::fromConstraints(environment,
                                                           constraints, config);
        return std::make_unique<ConvexPolyhedraState>(std::move(state));
    }
    }
    throw std::logic_error("unknown raw state domain tag");
}

} // namespace

NumericalState::RawBuffer NumericalState::serializeRaw() const
{
    // Transfers are allowed to retain an exact but non-minimal H cache so
    // clients that only need constraints do not pay a global redundancy pass
    // after every update. Serialization and hashing are the representation-
    // independent boundary, so canonicalize an isolated copy here.
    const NumericalState* canonical = this;
    std::unique_ptr<AbstractState> cloned;
    if (dynamic_cast<const ConvexPolyhedraState*>(this) != nullptr)
    {
        cloned = clone();
        auto* numerical = dynamic_cast<NumericalState*>(cloned.get());
        if (numerical == nullptr)
            throw std::logic_error("numerical clone has an incompatible type");
        numerical->canonicalize();
        canonical = numerical;
    }

    Writer writer;
    writer.writeMagic();
    writer.writeU16(RawVersion);
    const DomainTag tag = domainTag(*canonical);
    writer.writeByte(static_cast<std::uint8_t>(tag));
    writer.writeByte(configurationFlags(*canonical, tag));
    writeEnvironment(writer, canonical->environment());
    writer.writeByte(canonical->isBottom() ? 1U : 0U);
    writeConstraints(writer,
                     canonical->isBottom() ? LinearConstraintSet{}
                                           : canonical->toConstraints());
    return writer.finish();
}

void NumericalState::substitute(Variable target,
                                const TreeExpression& expression)
{
    if (const std::optional<LinearExpression> linear = expression.asLinear())
    {
        substitute(target, *linear);
        return;
    }
    // Existentially eliminate the unknown post-value. No fact involving that
    // value can soundly constrain the pre-state without nonlinear/machine
    // semantics for the right-hand side.
    forget(target);
}

void NumericalState::substituteParallel(
    const TreeAssignmentList& assignments)
{
    std::set<Variable> targets;
    LinearAssignmentList affine;
    std::vector<Variable> unsupported;
    affine.reserve(assignments.size());
    unsupported.reserve(assignments.size());
    for (const TreeAssignment& assignment : assignments)
    {
        if (!environment().contains(assignment.target))
            throw std::invalid_argument(
                "parallel substitution target is not in environment");
        if (!targets.insert(assignment.target).second)
            throw std::invalid_argument(
                "parallel substitution contains a duplicate target");
        if (const std::optional<LinearExpression> linear =
                assignment.expression.asLinear())
            affine.push_back({assignment.target, *linear});
        else
            unsupported.push_back(assignment.target);
    }

    // Unknown output dimensions are existentially projected before the
    // remaining simultaneous affine preimage is formed.
    for (Variable target : unsupported)
        forget(target);
    substituteParallel(affine);
}

Interval NumericalState::bound(const TreeExpression& expression) const
{
    if (const std::optional<LinearExpression> linear = expression.asLinear())
        return bound(*linear);
    if (isBottom())
        return Interval(Bound::plusInfinity(), Bound::minusInfinity());
    return Interval::top();
}

VariableEnvironment NumericalState::unifyEnvironmentWith(
    NumericalState& other, bool initializeNewVariablesToZero)
{
    const VariableEnvironment merged = environment().merge(other.environment());
    changeEnvironment(merged, initializeNewVariablesToZero);
    other.changeEnvironment(merged, initializeNewVariablesToZero);
    return merged;
}

std::uint64_t NumericalState::hash() const
{
    const RawBuffer raw = serializeRaw();
    return readTrailingU64(raw);
}

std::unique_ptr<NumericalState> NumericalState::deserializeRaw(
    const RawBuffer& buffer)
{
    Reader reader(buffer);
    reader.readMagic();
    if (reader.readU16() != RawVersion)
        throw std::invalid_argument("raw state has an unsupported version");
    const DomainTag tag = decodeDomainTag(reader.readByte());
    const std::uint8_t flags = reader.readByte();
    const VariableEnvironment environment = readEnvironment(reader);
    const std::uint8_t bottomByte = reader.readByte();
    if (bottomByte > 1)
        throw std::invalid_argument("raw state has an invalid bottom flag");
    const LinearConstraintSet constraints =
        readConstraints(reader, environment);
    if (!reader.empty())
        throw std::invalid_argument("raw state has trailing data");
    if (bottomByte != 0 && !constraints.empty())
        throw std::invalid_argument(
            "raw bottom state unexpectedly contains constraints");
    return restore(tag, flags, environment, bottomByte != 0, constraints);
}
