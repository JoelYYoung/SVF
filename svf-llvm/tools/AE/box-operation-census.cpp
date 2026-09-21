//===- box-operation-census.cpp -- Box operation reuse census -----------===//
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
//===----------------------------------------------------------------------===//

#include "box-operation-census.h"

#include "AE/Core/BoxAddressDomain.h"
#include "Util/SVFUtil.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <iterator>
#include <list>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace SVF::BoxOperationCensus
{
namespace
{
using namespace AbstractDomain;

constexpr std::array<std::size_t, 2> Capacities{{64, 256}};

void combineHash(std::size_t& seed, std::size_t value)
{
    seed ^= value + 0x9e3779b9U + (seed << 6U) + (seed >> 2U);
}

struct OperationKey
{
    AbstractOperationKind operation;
    struct Operands
    {
        std::string left;
        std::string right;
    };
    std::shared_ptr<const Operands> operands;
    std::size_t digest;

    bool sameValue(const OperationKey& other) const
    {
        return operation == other.operation &&
               operands->left == other.operands->left &&
               operands->right == other.operands->right;
    }

    std::size_t canonicalBytes() const
    {
        return operands->left.size() + operands->right.size();
    }
};

bool commutative(AbstractOperationKind operation)
{
    return operation == AbstractOperationKind::Join ||
           operation == AbstractOperationKind::Meet ||
           operation == AbstractOperationKind::Equivalent;
}

OperationKey makeKey(AbstractOperationKind operation, std::string left,
                     std::string right)
{
    if (commutative(operation) && right < left)
        std::swap(left, right);
    std::size_t digest =
        std::hash<unsigned> {}(static_cast<unsigned>(operation));
    combineHash(digest, std::hash<std::string> {}(left));
    combineHash(digest, std::hash<std::string> {}(right));
    return {operation,
            std::make_shared<OperationKey::Operands>(
                OperationKey::Operands{std::move(left), std::move(right)}),
            digest};
}

OperationKey makeKeyForTest(AbstractOperationKind operation, std::string left,
                            std::string right, std::size_t digest)
{
    OperationKey key = makeKey(operation, std::move(left), std::move(right));
    key.digest = digest;
    return key;
}

class Lru
{
public:
    explicit Lru(std::size_t capacity) : capacity_(capacity) {}

    bool probe(const OperationKey& key)
    {
        // The digest selects candidates only. A hit always compares the full
        // canonical operands, so a hash collision cannot create reuse.
        const auto range = entries_.equal_range(key.digest);
        for (auto found = range.first; found != range.second; ++found)
        {
            if (!found->second->key.sameValue(key))
            {
                ++exactMismatchCount_;
                continue;
            }
            order_.splice(order_.begin(), order_, found->second);
            return true;
        }
        return false;
    }

    void remember(OperationKey key, std::size_t resultBytes)
    {
        order_.push_front({std::move(key), resultBytes});
        entries_.emplace(order_.front().key.digest, order_.begin());
        operandBytes_ += order_.front().key.canonicalBytes();
        resultBytes_ += resultBytes;
        peakOperandBytes_ = std::max(peakOperandBytes_, operandBytes_);
        peakResultBytes_ = std::max(peakResultBytes_, resultBytes_);
        if (entries_.size() <= capacity_)
            return;
        const auto removed = std::prev(order_.end());
        const auto range = entries_.equal_range(removed->key.digest);
        const auto indexed = std::find_if(
                                 range.first, range.second,
                                 [removed](const auto& entry)
        {
            return entry.second == removed;
        });
        if (indexed == range.second)
            throw std::runtime_error("missing Box operation LRU index");
        entries_.erase(indexed);
        operandBytes_ -= removed->key.canonicalBytes();
        resultBytes_ -= removed->resultBytes;
        order_.erase(removed);
    }

    std::size_t size() const
    {
        return entries_.size();
    }

    std::size_t peakResultBytes() const
    {
        return peakResultBytes_;
    }

    std::size_t peakOperandBytes() const
    {
        return peakOperandBytes_;
    }

    std::uint64_t exactMismatchCount() const
    {
        return exactMismatchCount_;
    }

private:
    struct Entry
    {
        OperationKey key;
        std::size_t resultBytes;
    };

    std::size_t capacity_;
    std::list<Entry> order_;
    std::unordered_multimap<std::size_t, std::list<Entry>::iterator> entries_;
    std::size_t operandBytes_ = 0;
    std::size_t resultBytes_ = 0;
    std::size_t peakOperandBytes_ = 0;
    std::size_t peakResultBytes_ = 0;
    std::uint64_t exactMismatchCount_ = 0;
};

struct Stats
{
    std::uint64_t calls = 0;
    std::uint64_t tracked = 0;
    std::uint64_t dropped = 0;
    std::uint64_t elapsedNanoseconds = 0;
    std::uint64_t normalizationNanoseconds = 0;
    std::uint64_t resultCanonicalBytes = 0;
    std::array<std::uint64_t, Capacities.size()> hits{};
    std::array<std::uint64_t, Capacities.size()> hitNanoseconds{};
};

struct Pending
{
    AbstractOperationKind operation;
    OperationKey key;
    std::array<bool, Capacities.size()> hits{};
};

std::array<Stats, static_cast<std::size_t>(AbstractOperationKind::Count)> stats;
std::array<Lru, Capacities.size()> caches
{
    {Lru(64), Lru(256)}};
std::vector<Pending> pending;

std::string canonical(const BoxAddressDomain& product)
{
    std::ostringstream output;
    output << "integer_tightening="
           << product.numerical().config().integerTightening << ";layout=";
    for (const auto& [location, content] : product.memoryLayout().cells())
        output << location.id() << ':' << content.id() << ',';
    output << ";state=" << product.toString();
    return output.str();
}

std::size_t index(AbstractOperationKind operation)
{
    return static_cast<std::size_t>(operation);
}

const char* name(AbstractOperationKind operation)
{
    static const std::array<const char*, 6> names
    {
        {"join", "meet", "widen", "narrow", "subset", "equivalent"}};
    return names.at(index(operation));
}
} // namespace

void collect(const AbstractOperationEvent& event)
{
    if (!event.left || !event.right ||
            event.left->kind() != DomainKind::Product)
        return;
    Stats& current = stats[index(event.operation)];
    if (event.phase == AbstractOperationPhase::Begin)
    {
        ++current.calls;
        const auto start = std::chrono::steady_clock::now();
        const auto& left = static_cast<const BoxAddressDomain&>(*event.left);
        const auto& right = static_cast<const BoxAddressDomain&>(*event.right);
        std::string leftCanonical = canonical(left);
        std::string rightCanonical = canonical(right);
        OperationKey key = makeKey(event.operation, std::move(leftCanonical),
                                   std::move(rightCanonical));
        current.normalizationNanoseconds += static_cast<std::uint64_t>(
                                                std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                        std::chrono::steady_clock::now() - start)
                                                .count());
        Pending entry{event.operation, std::move(key), {}};
        ++current.tracked;
        for (std::size_t cache = 0; cache < caches.size(); ++cache)
            entry.hits[cache] = caches[cache].probe(entry.key);
        pending.push_back(std::move(entry));
        return;
    }

    if (pending.empty() || pending.back().operation != event.operation)
        throw std::runtime_error("unbalanced Box operation telemetry");
    Pending entry = std::move(pending.back());
    pending.pop_back();
    current.elapsedNanoseconds += event.elapsedNanoseconds;
    const auto start = std::chrono::steady_clock::now();
    std::size_t resultBytes = 1;
    if (event.operation == AbstractOperationKind::Subset ||
            event.operation == AbstractOperationKind::Equivalent)
        ++current.resultCanonicalBytes;
    else
    {
        resultBytes =
            canonical(static_cast<const BoxAddressDomain&>(*event.left)).size();
        current.resultCanonicalBytes += resultBytes;
    }
    current.normalizationNanoseconds += static_cast<std::uint64_t>(
                                            std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                    std::chrono::steady_clock::now() - start)
                                            .count());
    for (std::size_t cache = 0; cache < caches.size(); ++cache)
    {
        if (entry.hits[cache])
        {
            ++current.hits[cache];
            current.hitNanoseconds[cache] += event.elapsedNanoseconds;
        }
        else
            caches[cache].remember(entry.key, resultBytes);
    }
}

void print()
{
    for (std::size_t operation = 0; operation < stats.size(); ++operation)
    {
        const auto kind = static_cast<AbstractOperationKind>(operation);
        const Stats& current = stats[operation];
        SVFUtil::outs() << "BOX_OPERATION_SUMMARY op=" << name(kind)
                        << " calls=" << current.calls
                        << " tracked=" << current.tracked
                        << " dropped=" << current.dropped
                        << " elapsed_ns=" << current.elapsedNanoseconds
                        << " normalization_ns="
                        << current.normalizationNanoseconds
                        << " result_canonical_bytes="
                        << current.resultCanonicalBytes << '\n';
        for (std::size_t cache = 0; cache < caches.size(); ++cache)
        {
            SVFUtil::outs()
                    << "BOX_OPERATION_LRU op=" << name(kind)
                    << " capacity=" << Capacities[cache]
                    << " hits=" << current.hits[cache]
                    << " hit_elapsed_ns=" << current.hitNanoseconds[cache] << '\n';
        }
    }
    SVFUtil::outs() << "BOX_OPERATION_WINDOW exact=1 pending="
                    << pending.size() << '\n';
    for (std::size_t cache = 0; cache < caches.size(); ++cache)
    {
        SVFUtil::outs() << "BOX_OPERATION_CACHE capacity=" << Capacities[cache]
                        << " entries=" << caches[cache].size()
                        << " peak_operand_canonical_bytes="
                        << caches[cache].peakOperandBytes()
                        << " peak_result_canonical_bytes="
                        << caches[cache].peakResultBytes()
                        << " key_shallow_bytes="
                        << caches[cache].size() * sizeof(OperationKey)
                        << " exact_mismatches="
                        << caches[cache].exactMismatchCount() << '\n';
    }
}

void selfTest()
{
    constexpr std::size_t SameDigest = 7;
    Lru cache(2);
    OperationKey first = makeKeyForTest(AbstractOperationKind::Join, "left",
                                        "right", SameDigest);
    OperationKey swapped = makeKeyForTest(AbstractOperationKind::Join, "right",
                                          "left", SameDigest);
    OperationKey collision = makeKeyForTest(AbstractOperationKind::Join, "left",
                                            "other", SameDigest);
    if (cache.probe(first))
        throw std::runtime_error("empty Box operation LRU hit");
    cache.remember(first, 4);
    if (!cache.probe(swapped) || cache.probe(collision) ||
            cache.exactMismatchCount() != 1)
        throw std::runtime_error("Box operation LRU exact-key failure");
    cache.remember(collision, 5);
    OperationKey third = makeKeyForTest(AbstractOperationKind::Join, "third",
                                        "value", 11);
    cache.remember(third, 6);
    OperationKey forward = makeKeyForTest(AbstractOperationKind::Subset,
                                          "left", "right", 13);
    OperationKey reverse = makeKeyForTest(AbstractOperationKind::Subset,
                                          "right", "left", 13);
    if (cache.size() != 2 || cache.probe(first) || !cache.probe(collision) ||
            !cache.probe(third) || cache.peakOperandBytes() == 0 ||
            cache.peakResultBytes() != 15 || forward.sameValue(reverse))
        throw std::runtime_error("Box operation LRU eviction failure");
    SVFUtil::outs() << "Box operation census contract passed\n";
}

} // namespace SVF::BoxOperationCensus
