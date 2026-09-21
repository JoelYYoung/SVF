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

#ifdef SVF_BOX_OPERATION_MEMOIZATION
#    include "AE/Core/BoxAddressDomain.h"
#endif

#ifdef SVF_BOX_STORAGE_TELEMETRY
#    include <atomic>
#    include <chrono>
#endif
#ifdef SVF_BOX_OPERATION_MEMOIZATION
#    include <algorithm>
#    include <list>
#    include <unordered_map>
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

#ifdef SVF_BOX_OPERATION_MEMOIZATION
namespace
{
constexpr std::size_t OperationCacheCapacity = 256;

enum class MemoOperation : unsigned char
{
    Join,
    Meet,
    Widen,
    Narrow
};

bool isCommutative(MemoOperation operation)
{
    return operation == MemoOperation::Join ||
           operation == MemoOperation::Meet;
}

void combineMemoHash(std::uint64_t& seed, std::uint64_t value)
{
    seed ^= value + 0x9e3779b97f4a7c15ULL + (seed << 6U) + (seed >> 2U);
}

std::uint64_t memoDigest(MemoOperation operation,
                         const BoxAddressDomain& left,
                         const BoxAddressDomain& right)
{
    std::uint64_t leftHash = left.semanticHash();
    std::uint64_t rightHash = right.semanticHash();
    if (isCommutative(operation) && rightHash < leftHash)
        std::swap(leftHash, rightHash);
    std::uint64_t seed = static_cast<unsigned>(operation);
    combineMemoHash(seed, leftHash);
    combineMemoHash(seed, rightHash);
    return seed;
}

class ProductOperationCache
{
public:
    bool probe(MemoOperation operation, const BoxAddressDomain& left,
               const BoxAddressDomain& right, BoxAddressDomain& destination)
    {
        ++lookups_;
        const std::uint64_t digest = memoDigest(operation, left, right);
        const auto range = index_.equal_range(digest);
        for (auto found = range.first; found != range.second; ++found)
        {
            const auto entry = found->second;
            const bool sameOrder =
                left.semanticEquivalent(entry->left) &&
                right.semanticEquivalent(entry->right);
            const bool reverseOrder = isCommutative(operation) &&
                                      left.semanticEquivalent(entry->right) &&
                                      right.semanticEquivalent(entry->left);
            if (entry->operation != operation ||
                    (!sameOrder && !reverseOrder))
            {
                ++exactMismatches_;
                continue;
            }
            order_.splice(order_.begin(), order_, entry);
            destination = entry->result;
            ++hits_;
            return true;
        }
        return false;
    }

    void remember(MemoOperation operation, BoxAddressDomain left,
                  BoxAddressDomain right, BoxAddressDomain result)
    {
        const std::uint64_t digest = memoDigest(operation, left, right);
        (void)result.semanticHash();
        order_.push_front({operation, digest, std::move(left),
                           std::move(right), std::move(result)});
        index_.emplace(digest, order_.begin());
        if (order_.size() <= OperationCacheCapacity)
            return;
        const auto removed = std::prev(order_.end());
        const auto range = index_.equal_range(removed->digest);
        const auto indexed = std::find_if(
                                 range.first, range.second,
                                 [removed](const auto& item)
        {
            return item.second == removed;
        });
        if (indexed == range.second)
            throw std::runtime_error("missing Box operation cache index");
        index_.erase(indexed);
        order_.erase(removed);
    }

    OperationMemoizationStats stats() const noexcept
    {
        return {lookups_, hits_, exactMismatches_, order_.size()};
    }

    void reset() noexcept
    {
        index_.clear();
        order_.clear();
        lookups_ = 0;
        hits_ = 0;
        exactMismatches_ = 0;
    }

private:
    struct Entry
    {
        MemoOperation operation;
        std::uint64_t digest;
        BoxAddressDomain left;
        BoxAddressDomain right;
        BoxAddressDomain result;
    };

    std::list<Entry> order_;
    std::unordered_multimap<std::uint64_t,
        std::list<Entry>::iterator> index_;
    std::uint64_t lookups_ = 0;
    std::uint64_t hits_ = 0;
    std::uint64_t exactMismatches_ = 0;
};

thread_local ProductOperationCache productOperationCache;

template <typename Operation>
void applyMemoized(MemoOperation operation, AbstractDomain& left,
                   const AbstractDomain& right, Operation&& apply)
{
    if (!left.isDomain<BoxAddressDomain>())
    {
        apply();
        return;
    }
    auto& leftProduct = static_cast<BoxAddressDomain&>(left);
    const auto& rightProduct = static_cast<const BoxAddressDomain&>(right);
    if (productOperationCache.probe(operation, leftProduct, rightProduct,
                                    leftProduct))
        return;
    BoxAddressDomain leftSnapshot(leftProduct);
    BoxAddressDomain rightSnapshot(rightProduct);
    apply();
    productOperationCache.remember(operation, std::move(leftSnapshot),
                                   std::move(rightSnapshot), leftProduct);
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

#ifdef SVF_BOX_OPERATION_MEMOIZATION
OperationMemoizationStats AbstractDomain::operationMemoizationStats() noexcept
{
    return productOperationCache.stats();
}

void AbstractDomain::resetOperationMemoization() noexcept
{
    productOperationCache.reset();
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
#ifdef SVF_BOX_OPERATION_MEMOIZATION
    applyMemoized(MemoOperation::Join, *this, other,
                  [this, &other]()
    {
        joinDomain(other);
    });
#else
    joinDomain(other);
#endif
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
#ifdef SVF_BOX_OPERATION_MEMOIZATION
    applyMemoized(MemoOperation::Meet, *this, other,
                  [this, &other]()
    {
        meetDomain(other);
    });
#else
    meetDomain(other);
#endif
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
#ifdef SVF_BOX_OPERATION_MEMOIZATION
    applyMemoized(MemoOperation::Widen, *this, next,
                  [this, &next]()
    {
        widenDomain(next);
    });
#else
    widenDomain(next);
#endif
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
#ifdef SVF_BOX_OPERATION_MEMOIZATION
    applyMemoized(MemoOperation::Narrow, *this, next,
                  [this, &next]()
    {
        narrowDomain(next);
    });
#else
    narrowDomain(next);
#endif
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
