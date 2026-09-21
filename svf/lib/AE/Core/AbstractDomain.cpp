//===- AbstractDomain.cpp -- Common abstract-property lattice API ------===//
//
//                     SVF: Static Value-Flow Analysis
//
// Copyright (C) <2013->  <Yulei Sui>
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU Affero General Public License for more details.
//
// You should have received a copy of the GNU Affero General Public License
// along with this program. If not, see <http://www.gnu.org/licenses/>.
//
// Contributors: Xiao Cheng, Jiawei Wang, Jiawei Yang
//
//===----------------------------------------------------------------------===//

#include "AE/Core/AbstractDomain.h"

#ifdef SVF_BOX_STORAGE_TELEMETRY
#    include <atomic>
#    include <chrono>
#endif
#include <stdexcept>

namespace SVF::AbstractDomain
{

#ifdef SVF_BOX_STORAGE_TELEMETRY
namespace
{
std::atomic<AbstractOperationEventSink> operationEventSink{nullptr};

using OperationClock = std::chrono::steady_clock;

std::uint64_t elapsedNanoseconds(OperationClock::time_point start)
{
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            OperationClock::now() - start)
            .count());
}

AbstractOperationEventSink enabledOperationSink(const AbstractDomain& domain)
{
    const AbstractOperationEventSink sink =
        operationEventSink.load(std::memory_order_relaxed);
    return sink && domain.kind() == DomainKind::Product ? sink : nullptr;
}

void emitOperation(AbstractOperationEventSink sink,
                   AbstractOperationKind operation,
                   AbstractOperationPhase phase, const AbstractDomain& left,
                   const AbstractDomain& right, std::uint64_t elapsed = 0,
                   CheckResult result = CheckResult::Unknown)
{
    if (sink)
        sink({operation, phase, &left, &right, elapsed, result});
}
} // namespace
#endif

const char* toString(CheckResult result)
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

AbstractDomain::~AbstractDomain() = default;

#ifdef SVF_BOX_STORAGE_TELEMETRY
void AbstractDomain::setOperationEventSink(
    AbstractOperationEventSink sink) noexcept
{
    operationEventSink.store(sink, std::memory_order_relaxed);
}
#endif

void AbstractDomain::requireCompatible(const AbstractDomain& other) const
{
    if (!hasCompatibleDomain(other))
        throw std::invalid_argument(
            "abstract properties use incompatible domains or configurations");
}

void AbstractDomain::joinWith(const AbstractDomain& other)
{
#ifdef SVF_BOX_STORAGE_TELEMETRY
    const AbstractOperationEventSink sink = enabledOperationSink(*this);
    if (sink)
        emitOperation(sink, AbstractOperationKind::Join,
                      AbstractOperationPhase::Begin, *this, other);
    const auto start =
        sink ? OperationClock::now() : OperationClock::time_point{};
#endif
    requireCompatible(other);
    joinDomain(other);
#ifdef SVF_BOX_STORAGE_TELEMETRY
    if (sink)
        emitOperation(sink, AbstractOperationKind::Join,
                      AbstractOperationPhase::End, *this, other,
                      elapsedNanoseconds(start));
#endif
}

void AbstractDomain::meetWith(const AbstractDomain& other)
{
#ifdef SVF_BOX_STORAGE_TELEMETRY
    const AbstractOperationEventSink sink = enabledOperationSink(*this);
    if (sink)
        emitOperation(sink, AbstractOperationKind::Meet,
                      AbstractOperationPhase::Begin, *this, other);
    const auto start =
        sink ? OperationClock::now() : OperationClock::time_point{};
#endif
    requireCompatible(other);
    meetDomain(other);
#ifdef SVF_BOX_STORAGE_TELEMETRY
    if (sink)
        emitOperation(sink, AbstractOperationKind::Meet,
                      AbstractOperationPhase::End, *this, other,
                      elapsedNanoseconds(start));
#endif
}

void AbstractDomain::widenWith(const AbstractDomain& next)
{
#ifdef SVF_BOX_STORAGE_TELEMETRY
    const AbstractOperationEventSink sink = enabledOperationSink(*this);
    if (sink)
        emitOperation(sink, AbstractOperationKind::Widen,
                      AbstractOperationPhase::Begin, *this, next);
    const auto start =
        sink ? OperationClock::now() : OperationClock::time_point{};
#endif
    requireCompatible(next);
    widenDomain(next);
#ifdef SVF_BOX_STORAGE_TELEMETRY
    if (sink)
        emitOperation(sink, AbstractOperationKind::Widen,
                      AbstractOperationPhase::End, *this, next,
                      elapsedNanoseconds(start));
#endif
}

void AbstractDomain::narrowWith(const AbstractDomain& next)
{
#ifdef SVF_BOX_STORAGE_TELEMETRY
    const AbstractOperationEventSink sink = enabledOperationSink(*this);
    if (sink)
        emitOperation(sink, AbstractOperationKind::Narrow,
                      AbstractOperationPhase::Begin, *this, next);
    const auto start =
        sink ? OperationClock::now() : OperationClock::time_point{};
#endif
    requireCompatible(next);
    if (!next.leqDomain(*this))
        throw std::invalid_argument(
            "narrowing requires next to be included in current");
    narrowDomain(next);
#ifdef SVF_BOX_STORAGE_TELEMETRY
    if (sink)
        emitOperation(sink, AbstractOperationKind::Narrow,
                      AbstractOperationPhase::End, *this, next,
                      elapsedNanoseconds(start));
#endif
}

bool AbstractDomain::isBottom() const
{
    return isBottomDomain();
}

bool AbstractDomain::isTop() const
{
    return isTopDomain();
}

CheckResult AbstractDomain::isSubsetOf(const AbstractDomain& other) const
{
#ifdef SVF_BOX_STORAGE_TELEMETRY
    const AbstractOperationEventSink sink = enabledOperationSink(*this);
    if (sink)
        emitOperation(sink, AbstractOperationKind::Subset,
                      AbstractOperationPhase::Begin, *this, other);
    const auto start =
        sink ? OperationClock::now() : OperationClock::time_point{};
#endif
    requireCompatible(other);
    const CheckResult result =
        leqDomain(other) ? CheckResult::True : CheckResult::False;
#ifdef SVF_BOX_STORAGE_TELEMETRY
    if (sink)
        emitOperation(sink, AbstractOperationKind::Subset,
                      AbstractOperationPhase::End, *this, other,
                      elapsedNanoseconds(start), result);
#endif
    return result;
}

CheckResult AbstractDomain::isEquivalentTo(const AbstractDomain& other) const
{
#ifdef SVF_BOX_STORAGE_TELEMETRY
    const AbstractOperationEventSink sink = enabledOperationSink(*this);
    if (sink)
        emitOperation(sink, AbstractOperationKind::Equivalent,
                      AbstractOperationPhase::Begin, *this, other);
    const auto start =
        sink ? OperationClock::now() : OperationClock::time_point{};
#endif
    requireCompatible(other);
    const CheckResult result = leqDomain(other) && other.leqDomain(*this)
                                   ? CheckResult::True
                                   : CheckResult::False;
#ifdef SVF_BOX_STORAGE_TELEMETRY
    if (sink)
        emitOperation(sink, AbstractOperationKind::Equivalent,
                      AbstractOperationPhase::End, *this, other,
                      elapsedNanoseconds(start), result);
#endif
    return result;
}

std::string AbstractDomain::toString() const
{
    return domainToString();
}

} // namespace SVF::AbstractDomain
