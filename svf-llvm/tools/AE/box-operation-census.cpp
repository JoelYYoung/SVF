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
#include <limits>
#include <list>
#include <optional>
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

constexpr std::array<std::size_t, 4> Capacities{{64, 256, 1024, 4096}};

void combineHash(std::size_t& seed, std::size_t value)
{
    seed ^= value + 0x9e3779b9U + (seed << 6U) + (seed >> 2U);
}

struct OperationKey
{
    AbstractOperationKind operation;
    std::uint64_t left;
    std::uint64_t right;

    bool operator==(const OperationKey& other) const
    {
        return operation == other.operation && left == other.left &&
               right == other.right;
    }
};

struct OperationKeyHash
{
    std::size_t operator()(const OperationKey& key) const
    {
        std::size_t seed =
            std::hash<unsigned>{}(static_cast<unsigned>(key.operation));
        combineHash(seed, std::hash<std::uint64_t>{}(key.left));
        combineHash(seed, std::hash<std::uint64_t>{}(key.right));
        return seed;
    }
};

class Lru
{
public:
    explicit Lru(std::size_t capacity) : capacity_(capacity) {}

    bool probe(const OperationKey& key)
    {
        const auto found = entries_.find(key);
        if (found == entries_.end())
            return false;
        order_.splice(order_.begin(), order_, found->second);
        return true;
    }

    void remember(const OperationKey& key, std::size_t resultBytes)
    {
        const auto existing = entries_.find(key);
        if (existing != entries_.end())
        {
            order_.splice(order_.begin(), order_, existing->second);
            return;
        }
        order_.push_front({key, resultBytes});
        entries_.emplace(key, order_.begin());
        resultBytes_ += resultBytes;
        peakResultBytes_ = std::max(peakResultBytes_, resultBytes_);
        if (entries_.size() <= capacity_)
            return;
        resultBytes_ -= order_.back().resultBytes;
        entries_.erase(order_.back().key);
        order_.pop_back();
    }

    std::size_t size() const
    {
        return entries_.size();
    }

    std::size_t peakResultBytes() const
    {
        return peakResultBytes_;
    }

private:
    struct Entry
    {
        OperationKey key;
        std::size_t resultBytes;
    };

    std::size_t capacity_;
    std::list<Entry> order_;
    std::unordered_map<OperationKey, std::list<Entry>::iterator,
                       OperationKeyHash>
        entries_;
    std::size_t resultBytes_ = 0;
    std::size_t peakResultBytes_ = 0;
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
    std::array<std::uint64_t, Capacities.size()> versionHits{};
    std::array<std::uint64_t, Capacities.size()> versionHitNanoseconds{};
};

struct Pending
{
    AbstractOperationKind operation;
    std::optional<OperationKey> key;
    std::array<bool, Capacities.size()> hits{};
    OperationKey versionKey;
    std::array<bool, Capacities.size()> versionHits{};
};

std::array<Stats, static_cast<std::size_t>(AbstractOperationKind::Count)> stats;
std::array<Lru, Capacities.size()> caches{
    {Lru(64), Lru(256), Lru(1024), Lru(4096)}};
std::array<Lru, Capacities.size()> versionCaches{
    {Lru(64), Lru(256), Lru(1024), Lru(4096)}};
std::vector<Pending> pending;
std::unordered_map<std::string, std::uint64_t> stateIds;
std::unordered_map<std::string, std::uint64_t> valueStateIds;
std::unordered_map<std::uint64_t, std::uint64_t> versionStates;
std::uint64_t nextStateId = 1;
std::uint64_t nextValueStateId = 1;
std::size_t stateBytes = 0;
std::uint64_t versionCollisions = 0;

std::size_t stateBudget()
{
    static const std::size_t budget = [] {
        constexpr std::size_t DefaultBudget = 256U * 1024U * 1024U;
        const char* value = std::getenv("BOX_OPERATION_STATE_BUDGET_BYTES");
        if (!value || !*value)
            return DefaultBudget;
        char* end = nullptr;
        const unsigned long long parsed = std::strtoull(value, &end, 10);
        if (*end != '\0' || value[0] == '-' ||
            parsed > std::numeric_limits<std::size_t>::max())
            throw std::runtime_error(
                "invalid BOX_OPERATION_STATE_BUDGET_BYTES");
        return static_cast<std::size_t>(parsed);
    }();
    return budget;
}

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

std::optional<std::uint64_t> intern(std::string state)
{
    const auto existing = stateIds.find(state);
    if (existing != stateIds.end())
        return existing->second;
    if (state.size() > stateBudget() - std::min(stateBudget(), stateBytes))
        return std::nullopt;
    stateBytes += state.size();
    const std::uint64_t id = nextStateId++;
    stateIds.emplace(std::move(state), id);
    return id;
}

std::size_t index(AbstractOperationKind operation)
{
    return static_cast<std::size_t>(operation);
}

bool commutative(AbstractOperationKind operation)
{
    return operation == AbstractOperationKind::Join ||
           operation == AbstractOperationKind::Meet ||
           operation == AbstractOperationKind::Equivalent;
}

const char* name(AbstractOperationKind operation)
{
    static const std::array<const char*, 6> names{
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
        const auto validateVersion = [](std::uint64_t version,
                                        const std::string& state)
        {
            const std::size_t marker = state.find(";state=");
            if (marker == std::string::npos)
                throw std::runtime_error("invalid canonical Product state");
            // MemoryLayout is a shared monotone schema: extending it adds
            // implicit-Top coordinates to every copy and does not mutate the
            // Product value represented by this version identity.
            const std::string value = state.substr(marker);
            const auto [valueIterator, valueInserted] =
                valueStateIds.emplace(value, nextValueStateId);
            if (valueInserted)
                ++nextValueStateId;
            const auto [iterator, inserted] =
                versionStates.emplace(version, valueIterator->second);
            if (!inserted && iterator->second != valueIterator->second)
                ++versionCollisions;
        };
        validateVersion(left.operationVersion(), leftCanonical);
        validateVersion(right.operationVersion(), rightCanonical);
        const auto leftId = intern(std::move(leftCanonical));
        const auto rightId = intern(std::move(rightCanonical));
        current.normalizationNanoseconds += static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - start)
                .count());
        OperationKey versionKey{event.operation, left.operationVersion(),
                                right.operationVersion()};
        if (commutative(event.operation) && versionKey.right < versionKey.left)
            std::swap(versionKey.left, versionKey.right);
        Pending entry{event.operation, std::nullopt, {}, versionKey, {}};
        for (std::size_t cache = 0; cache < versionCaches.size(); ++cache)
            entry.versionHits[cache] =
                versionCaches[cache].probe(versionKey);
        if (leftId && rightId)
        {
            OperationKey key{event.operation, *leftId, *rightId};
            if (commutative(event.operation) && key.right < key.left)
                std::swap(key.left, key.right);
            entry.key = key;
            ++current.tracked;
            for (std::size_t cache = 0; cache < caches.size(); ++cache)
                entry.hits[cache] = caches[cache].probe(key);
        }
        else
            ++current.dropped;
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
    if (!entry.key)
        return;
    for (std::size_t cache = 0; cache < caches.size(); ++cache)
    {
        if (entry.hits[cache])
        {
            ++current.hits[cache];
            current.hitNanoseconds[cache] += event.elapsedNanoseconds;
        }
        else
            caches[cache].remember(*entry.key, resultBytes);
    }
    for (std::size_t cache = 0; cache < versionCaches.size(); ++cache)
    {
        if (entry.versionHits[cache])
        {
            ++current.versionHits[cache];
            current.versionHitNanoseconds[cache] += event.elapsedNanoseconds;
        }
        else
            versionCaches[cache].remember(entry.versionKey, resultBytes);
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
            SVFUtil::outs()
                << "BOX_OPERATION_VERSION_LRU op=" << name(kind)
                << " capacity=" << Capacities[cache]
                << " hits=" << current.versionHits[cache]
                << " hit_elapsed_ns="
                << current.versionHitNanoseconds[cache] << '\n';
        }
    }
    SVFUtil::outs() << "BOX_OPERATION_STATES unique=" << stateIds.size()
                    << " canonical_bytes=" << stateBytes
                    << " canonical_budget_bytes=" << stateBudget()
                    << " entry_shallow_bytes="
                    << stateIds.size() *
                           (sizeof(std::string) + sizeof(std::uint64_t))
                    << " pending=" << pending.size() << '\n';
    SVFUtil::outs() << "BOX_OPERATION_VERSION_STATES identities="
                    << versionStates.size()
                    << " values=" << valueStateIds.size()
                    << " collisions=" << versionCollisions << '\n';
    for (std::size_t cache = 0; cache < caches.size(); ++cache)
    {
        SVFUtil::outs() << "BOX_OPERATION_CACHE capacity=" << Capacities[cache]
                        << " entries=" << caches[cache].size()
                        << " peak_result_canonical_bytes="
                        << caches[cache].peakResultBytes()
                        << " key_shallow_bytes="
                        << caches[cache].size() * sizeof(OperationKey) << '\n';
        SVFUtil::outs()
            << "BOX_OPERATION_VERSION_CACHE capacity=" << Capacities[cache]
            << " entries=" << versionCaches[cache].size()
            << " peak_result_canonical_bytes="
            << versionCaches[cache].peakResultBytes()
            << " key_shallow_bytes="
            << versionCaches[cache].size() * sizeof(OperationKey) << '\n';
    }
}

} // namespace SVF::BoxOperationCensus
