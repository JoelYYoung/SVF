//===- box-storage-observer.cpp -- Box storage census -------------------//
//
//                     SVF: Static Value-Flow Analysis
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
//===----------------------------------------------------------------------===//

#include "AE/Svfexe/AbstractInterpretation.h"
#include "SVF-LLVM/SVFIRBuilder.h"
#include "Util/CommandLine.h"
#include "Util/Options.h"
#include "WPA/Andersen.h"

#ifdef SVF_BOX_STORAGE_TELEMETRY
#include "box-operation-census.h"
#endif

#include <algorithm>
#include <cstdlib>
#include <stdexcept>
#include <string_view>
#ifdef SVF_BOX_STORAGE_TELEMETRY
#include <atomic>
#include <array>
#include <chrono>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <sys/resource.h>
#include <unistd.h>
#endif
#include <vector>

using namespace SVF;
using namespace SVFUtil;

#ifdef SVF_BOX_STORAGE_TELEMETRY
namespace
{
using namespace SVF::AbstractDomain;

using OccupancyHistogram = std::map<std::size_t, std::uint64_t>;

struct CowriteCensus
{
    std::ofstream trace;
    std::uint64_t epochs = 0;
    std::uint64_t noops = 0;
    std::uint64_t changedVariables = 0;
    std::uint64_t touchedVariables = 0;
    std::uint64_t copyConstructs = 0;
    std::uint64_t copyAssignments = 0;
    std::uint64_t moveConstructs = 0;
    std::uint64_t moveAssignments = 0;
    std::uint64_t stateCreates = 0;
    std::uint64_t stateDestroys = 0;
    std::uint64_t contextualDetaches = 0;
    std::uint64_t contextualClonedSlots = 0;
    std::size_t maxChangedVariables = 0;
    std::map<std::size_t, std::uint64_t> changedHistogram;

    void open(const char *path)
    {
        trace.open(path, std::ios::out | std::ios::trunc);
        if (!trace)
            throw std::runtime_error(
                std::string("cannot open Box co-write trace: ") + path);
        trace << "# box-cowrite-trace-v2\n"
              << "# M sequence epoch kind state related_state before_bottom "
                 "after_bottom changed_count +/-id:type:exponent:significand... "
                 "T actual_touch_count id:type:exponent:significand...\n"
              << "# S sequence kind state source_state mutation_epoch bottom\n"
              << "# D sequence epoch state page_index occupied_slots\n"
              << "# W epoch state copied_slots occupied_slots\n";
    }

    bool enabled() const
    {
        return trace.is_open();
    }

    void mutation(const BoxMutationEvent &event)
    {
        if (event.phase != BoxMutationPhase::End)
            return;
        ++epochs;
        noops += event.changedVariableCount == 0 &&
                 event.beforeBottom == event.afterBottom;
        changedVariables += event.changedVariableCount;
        touchedVariables += event.touchedVariableCount;
        maxChangedVariables =
            std::max(maxChangedVariables, event.changedVariableCount);
        ++changedHistogram[event.changedVariableCount];
        if (!enabled())
            return;
        trace << "M " << event.sequence << ' ' << event.epoch << ' '
              << static_cast<unsigned>(event.kind) << ' ' << event.stateId
              << ' ' << event.relatedStateId << ' ' << event.beforeBottom
              << ' ' << event.afterBottom << ' ' << event.changedVariableCount;
        for (std::size_t index = 0; index < event.changedVariableCount; ++index)
        {
            const Variable variable = event.changedVariables[index];
            const NumericType &type = variable.type();
            trace << ' '
                  << (event.changedVariablesConstrainedAfter[index] ? '+' : '-')
                  << variable.id() << ':'
                  << static_cast<unsigned>(type.kind) << ':'
                  << type.floatFormat.exponentBits << ':'
                  << type.floatFormat.significandBits;
        }
        trace << " T " << event.touchedVariableCount;
        for (std::size_t index = 0; index < event.touchedVariableCount; ++index)
        {
            const Variable variable = event.touchedVariables[index];
            const NumericType &type = variable.type();
            trace << ' ' << variable.id() << ':'
                  << static_cast<unsigned>(type.kind) << ':'
                  << type.floatFormat.exponentBits << ':'
                  << type.floatFormat.significandBits;
        }
        trace << '\n';
    }

    void stateEvent(const BoxStateEvent &event)
    {
        stateCreates += event.kind == BoxStateEventKind::Create;
        copyConstructs += event.kind == BoxStateEventKind::CopyConstruct;
        moveConstructs += event.kind == BoxStateEventKind::MoveConstruct;
        copyAssignments += event.kind == BoxStateEventKind::CopyAssign;
        moveAssignments += event.kind == BoxStateEventKind::MoveAssign;
        stateDestroys += event.kind == BoxStateEventKind::Destroy;
        if (enabled())
            trace << "S " << event.sequence << ' '
                  << static_cast<unsigned>(event.kind) << ' '
                  << event.stateId << ' ' << event.sourceStateId << ' '
                  << event.mutationEpoch << ' ' << event.bottom
                  << '\n';
    }

    void storage(const BoxStorageEvent &event)
    {
        if (event.mutationEpoch == 0 ||
                (event.kind != BoxStorageEventKind::PageDetach &&
                 event.kind != BoxStorageEventKind::JoinMaterializedPage))
            return;
        ++contextualDetaches;
        if (enabled())
            trace << "D " << event.sequence << ' ' << event.mutationEpoch
                  << ' ' << event.stateId << ' ' << event.pageIndex << ' '
                  << event.occupiedSlots << '\n';
    }

    void work(const BoxStorageWorkEvent &event)
    {
        if (event.mutationEpoch == 0 ||
                event.kind != BoxStorageWorkKind::Clone)
            return;
        contextualClonedSlots += event.copiedSlots;
        if (enabled())
            trace << "W " << event.mutationEpoch << ' ' << event.stateId
                  << ' ' << event.copiedSlots << ' ' << event.occupiedSlots
                  << '\n';
    }

    void print() const
    {
        SVFUtil::outs() << "BOX_COWRITE_CENSUS epochs=" << epochs
                        << " noops=" << noops
                        << " changed_variables=" << changedVariables
                        << " touched_variables=" << touchedVariables
                        << " max_changed_variables=" << maxChangedVariables
                        << " copy_constructs=" << copyConstructs
                        << " copy_assignments=" << copyAssignments
                        << " move_constructs=" << moveConstructs
                        << " move_assignments=" << moveAssignments
                        << " state_creates=" << stateCreates
                        << " state_destroys=" << stateDestroys
                        << " contextual_detaches=" << contextualDetaches
                        << " contextual_cloned_slots="
                        << contextualClonedSlots;
        for (const auto &[changed, count] : changedHistogram)
            SVFUtil::outs() << " changed" << changed << '=' << count;
        SVFUtil::outs() << '\n';
    }
} cowriteCensus;

void collectMutationEvent(const BoxMutationEvent &event)
{
    cowriteCensus.mutation(event);
}

void collectStateEvent(const BoxStateEvent &event)
{
    cowriteCensus.stateEvent(event);
}

void printOccupancy(const char *scope, const char *name,
                    const OccupancyHistogram &histogram)
{
    SVFUtil::outs() << "BOX_STORAGE_OCCUPANCY scope=" << scope
                    << " name=" << name;
    for (const auto &bin : histogram)
        SVFUtil::outs() << " used" << bin.first << '=' << bin.second;
    SVFUtil::outs() << '\n';
}

struct StorageEvents
{
    std::array<std::uint64_t,
        static_cast<std::size_t>(BoxStorageEventKind::Count)>
        counts{};
    std::array<OccupancyHistogram,
        static_cast<std::size_t>(BoxStorageEventKind::Count)> occupancy;
    std::unordered_set<std::uint64_t> livePages;
    std::size_t peakLivePages = 0;
    std::size_t directoryEntriesCopied = 0;
    std::size_t directoryChunkEntriesCopied = 0;
} storageEvents;

class PeakMemoryCensus
{
    struct Page
    {
        std::size_t occupiedSlots = 0;
        std::size_t shallowBytes = 0;
        std::size_t rationalUsedLimbBytes = 0;
        std::size_t emptySlotShallowBytes = 0;
    };

    struct Sample
    {
        std::uint64_t elapsedMicros = 0;
        std::uint64_t rssBytes = 0;
        std::uint64_t livePages = 0;
        std::uint64_t occupiedSlots = 0;
        std::uint64_t pageShallowBytes = 0;
        std::uint64_t rationalUsedLimbBytes = 0;
        std::uint64_t emptySlotShallowBytes = 0;
    };

public:
    void start()
    {
        const char* value = std::getenv("BOX_PEAK_MEMORY_CENSUS");
        enabled_ = value && std::string_view(value) != "0";
        if (!enabled_)
            return;
        started_ = std::chrono::steady_clock::now();
        stop_.store(false, std::memory_order_release);
        sampler_ = std::thread(
            [this]
        {
            while (!stop_.load(std::memory_order_acquire))
            {
                sample();
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            sample();
        });
    }

    void storage(const BoxStorageEvent& event)
    {
        if (!enabled_)
            return;
        const bool creates = event.kind == BoxStorageEventKind::PageAllocate ||
                             event.kind == BoxStorageEventKind::PageDetach ||
                             event.kind == BoxStorageEventKind::JoinMaterializedPage;
        const bool updates = event.kind == BoxStorageEventKind::PageContentUpdate;
        const bool releases = event.kind == BoxStorageEventKind::PageRelease;
        if (!creates && !updates && !releases)
            return;

        generation_.fetch_add(1, std::memory_order_acq_rel);
        if (releases)
        {
            const auto found = pages_.find(event.pageId);
            if (found != pages_.end())
            {
                subtract(found->second);
                pages_.erase(found);
            }
        }
        else
        {
            const Page next{event.occupiedSlots, event.pageShallowBytes,
                            event.rationalUsedLimbBytes,
                            event.emptySlotShallowBytes};
            const auto [found, inserted] = pages_.try_emplace(event.pageId, next);
            if (!inserted)
            {
                subtract(found->second);
                found->second = next;
            }
            add(next);
        }
        publish();
        generation_.fetch_add(1, std::memory_order_release);
    }

    void stopAndPrint()
    {
        if (!enabled_)
            return;
        stop_.store(true, std::memory_order_release);
        sampler_.join();
        const Sample final = readSample();
        struct rusage usage {};
        getrusage(RUSAGE_SELF, &usage);
        printSample("rss_peak", rssPeak_);
        printSample("page_peak", pagePeak_);
        printSample("final", final);
        std::uint64_t kernelPeakRssBytes =
            static_cast<std::uint64_t>(usage.ru_maxrss);
#ifndef __APPLE__
        kernelPeakRssBytes *= 1024;
#endif
        SVFUtil::outs() << "BOX_PEAK_MEMORY_SUMMARY samples=" << samples_
                        << " interval_us=5000"
                        << " kernel_peak_rss_bytes=" << kernelPeakRssBytes
                        << '\n';
        enabled_ = false;
    }

private:
    static std::uint64_t currentRssBytes()
    {
        std::ifstream input("/proc/self/statm");
        std::uint64_t virtualPages = 0, residentPages = 0;
        if (!(input >> virtualPages >> residentPages))
            return 0;
        (void)virtualPages;
        return residentPages * static_cast<std::uint64_t>(sysconf(_SC_PAGESIZE));
    }

    void add(const Page& page)
    {
        ++livePages_;
        occupiedSlots_ += page.occupiedSlots;
        pageShallowBytes_ += page.shallowBytes;
        rationalUsedLimbBytes_ += page.rationalUsedLimbBytes;
        emptySlotShallowBytes_ += page.emptySlotShallowBytes;
    }

    void subtract(const Page& page)
    {
        --livePages_;
        occupiedSlots_ -= page.occupiedSlots;
        pageShallowBytes_ -= page.shallowBytes;
        rationalUsedLimbBytes_ -= page.rationalUsedLimbBytes;
        emptySlotShallowBytes_ -= page.emptySlotShallowBytes;
    }

    void publish()
    {
        publishedLivePages_.store(livePages_, std::memory_order_relaxed);
        publishedOccupiedSlots_.store(occupiedSlots_, std::memory_order_relaxed);
        publishedPageShallowBytes_.store(pageShallowBytes_,
                                         std::memory_order_relaxed);
        publishedRationalUsedLimbBytes_.store(
            rationalUsedLimbBytes_, std::memory_order_relaxed);
        publishedEmptySlotShallowBytes_.store(
            emptySlotShallowBytes_, std::memory_order_relaxed);
    }

    Sample readSample() const
    {
        Sample result;
        std::uint64_t before = 0, after = 0;
        do
        {
            before = generation_.load(std::memory_order_acquire);
            if (before & 1U)
                continue;
            result.livePages = publishedLivePages_.load(std::memory_order_relaxed);
            result.occupiedSlots = publishedOccupiedSlots_.load(std::memory_order_relaxed);
            result.pageShallowBytes = publishedPageShallowBytes_.load(
                                          std::memory_order_relaxed);
            result.rationalUsedLimbBytes = publishedRationalUsedLimbBytes_.load(
                                               std::memory_order_relaxed);
            result.emptySlotShallowBytes = publishedEmptySlotShallowBytes_.load(
                                               std::memory_order_relaxed);
            after = generation_.load(std::memory_order_acquire);
        }
        while (before != after || (after & 1U));
        result.rssBytes = currentRssBytes();
        result.elapsedMicros = static_cast<std::uint64_t>(
                                   std::chrono::duration_cast<std::chrono::microseconds>(
                                       std::chrono::steady_clock::now() - started_).count());
        return result;
    }

    void sample()
    {
        const Sample current = readSample();
        ++samples_;
        if (current.rssBytes > rssPeak_.rssBytes)
            rssPeak_ = current;
        if (current.pageShallowBytes > pagePeak_.pageShallowBytes)
            pagePeak_ = current;
    }

    static void printSample(const char* name, const Sample& sample)
    {
        const std::uint64_t capacitySlots = sample.livePages * 8;
        const std::uint64_t emptySlots = capacitySlots >= sample.occupiedSlots
                                         ? capacitySlots - sample.occupiedSlots : 0;
        SVFUtil::outs() << "BOX_PEAK_MEMORY_SAMPLE name=" << name
                        << " elapsed_us=" << sample.elapsedMicros
                        << " rss_bytes=" << sample.rssBytes
                        << " live_pages=" << sample.livePages
                        << " occupied_slots=" << sample.occupiedSlots
                        << " empty_slots=" << emptySlots
                        << " page_shallow_bytes=" << sample.pageShallowBytes
                        << " rational_used_limb_bytes="
                        << sample.rationalUsedLimbBytes
                        << " empty_slot_shallow_bytes="
                        << sample.emptySlotShallowBytes
                        << '\n';
    }

    bool enabled_ = false;
    std::atomic<bool> stop_{false};
    std::thread sampler_;
    std::chrono::steady_clock::time_point started_;
    std::unordered_map<std::uint64_t, Page> pages_;
    std::uint64_t livePages_ = 0;
    std::uint64_t occupiedSlots_ = 0;
    std::uint64_t pageShallowBytes_ = 0;
    std::uint64_t rationalUsedLimbBytes_ = 0;
    std::uint64_t emptySlotShallowBytes_ = 0;
    std::atomic<std::uint64_t> generation_{0};
    std::atomic<std::uint64_t> publishedLivePages_{0};
    std::atomic<std::uint64_t> publishedOccupiedSlots_{0};
    std::atomic<std::uint64_t> publishedPageShallowBytes_{0};
    std::atomic<std::uint64_t> publishedRationalUsedLimbBytes_{0};
    std::atomic<std::uint64_t> publishedEmptySlotShallowBytes_{0};
    Sample rssPeak_;
    Sample pagePeak_;
    std::uint64_t samples_ = 0;
} peakMemoryCensus;

struct StorageWork
{
    std::uint64_t count = 0;
    std::uint64_t direct = 0;
    std::uint64_t occupied = 0;
    std::uint64_t copied = 0;
    std::uint64_t relocated = 0;
    std::uint64_t allocated = 0;
    std::size_t peakOverlap = 0;
};
std::array<StorageWork, static_cast<std::size_t>(BoxStorageWorkKind::Count)> storageWork;

void collectStorageWork(const BoxStorageWorkEvent &event)
{
    cowriteCensus.work(event);
    auto &work = storageWork[static_cast<std::size_t>(event.kind)];
    ++work.count;
    work.direct += event.directIndexed;
    work.occupied += event.occupiedSlots;
    work.copied += event.copiedSlots;
    work.relocated += event.relocatedSlots;
    work.allocated += event.allocatedSlotBytes;
    work.peakOverlap = std::max(work.peakOverlap, event.overlappingSlotBytes);
}

void printStorageWork()
{
    const std::array<const char *, 8> names{{
            "clone", "grow", "shrink", "promote", "demote", "insert", "update", "erase"
        }};
    static_assert(names.size() == static_cast<std::size_t>(BoxStorageWorkKind::Count),
                  "update physical work names");
    for (std::size_t index = 0; index < names.size(); ++index)
    {
        const auto &work = storageWork[index];
        SVFUtil::outs() << "BOX_STORAGE_WORK kind=" << names[index]
                        << " count=" << work.count << " direct=" << work.direct
                        << " occupied_slots=" << work.occupied
                        << " copied_slots=" << work.copied
                        << " relocated_slots=" << work.relocated
                        << " allocated_slot_bytes=" << work.allocated
                        << " peak_operation_overlap_slot_bytes=" << work.peakOverlap << '\n';
    }
}

std::size_t eventIndex(BoxStorageEventKind kind)
{
    return static_cast<std::size_t>(kind);
}

void collectStorageEvent(const BoxStorageEvent &event)
{
    peakMemoryCensus.storage(event);
    cowriteCensus.storage(event);
    ++storageEvents.counts[eventIndex(event.kind)];
    if (event.kind != BoxStorageEventKind::DirectoryDetach &&
            event.kind != BoxStorageEventKind::DirectoryChunkDetach)
        ++storageEvents.occupancy[eventIndex(event.kind)][event.occupiedSlots];
    if (event.kind == BoxStorageEventKind::PageAllocate ||
            event.kind == BoxStorageEventKind::PageDetach ||
            event.kind == BoxStorageEventKind::JoinMaterializedPage)
    {
        storageEvents.livePages.insert(event.pageId);
        storageEvents.peakLivePages =
            std::max(storageEvents.peakLivePages, storageEvents.livePages.size());
    }
    else if (event.kind == BoxStorageEventKind::PageRelease)
        storageEvents.livePages.erase(event.pageId);
    else if (event.kind == BoxStorageEventKind::DirectoryDetach)
        storageEvents.directoryEntriesCopied += event.directoryEntries;
    else if (event.kind == BoxStorageEventKind::DirectoryChunkDetach)
        storageEvents.directoryChunkEntriesCopied += event.directoryEntries;
}

constexpr std::size_t DirectoryChunkEntries = 8;

void combineHash(std::size_t &seed, std::size_t value)
{
    seed ^= value + 0x9e3779b9U + (seed << 6U) + (seed >> 2U);
}

struct DirectoryToken
{
    std::size_t pageIndex = 0;
    std::uint64_t contentClass = 0;

    bool operator==(const DirectoryToken &other) const
    {
        return pageIndex == other.pageIndex &&
               contentClass == other.contentClass;
    }
};

struct DirectoryTokenHash
{
    std::size_t operator()(const DirectoryToken &token) const
    {
        std::size_t seed = std::hash<std::size_t> {}(token.pageIndex);
        combineHash(seed, std::hash<std::uint64_t> {}(token.contentClass));
        return seed;
    }
};

struct DirectoryContent
{
    bool bottom = false;
    std::vector<DirectoryToken> entries;

    bool operator==(const DirectoryContent &other) const
    {
        return bottom == other.bottom && entries == other.entries;
    }
};

struct DirectoryContentHash
{
    std::size_t operator()(const DirectoryContent &directory) const
    {
        std::size_t seed = std::hash<bool> {}(directory.bottom);
        const DirectoryTokenHash tokenHash;
        for (const DirectoryToken &token : directory.entries)
            combineHash(seed, tokenHash(token));
        return seed;
    }
};

std::size_t directoryEditDistance(const DirectoryContent &left,
                                  const DirectoryContent &right)
{
    std::size_t distance = left.bottom == right.bottom ? 0 : 1;
    auto leftEntry = left.entries.begin();
    auto rightEntry = right.entries.begin();
    while (leftEntry != left.entries.end() &&
            rightEntry != right.entries.end())
    {
        if (leftEntry->pageIndex < rightEntry->pageIndex)
        {
            ++distance;
            ++leftEntry;
        }
        else if (rightEntry->pageIndex < leftEntry->pageIndex)
        {
            ++distance;
            ++rightEntry;
        }
        else
        {
            distance += leftEntry->contentClass != rightEntry->contentClass;
            ++leftEntry;
            ++rightEntry;
        }
    }
    distance += static_cast<std::size_t>(
                    std::distance(leftEntry, left.entries.end()));
    distance += static_cast<std::size_t>(
                    std::distance(rightEntry, right.entries.end()));
    return distance;
}

struct CarrierStorage
{
    std::size_t states = 0;
    std::size_t bottomStates = 0;
    std::size_t logicalPageReferences = 0;
    std::size_t directoryAllocatedBytes = 0;
    std::size_t slotsPerPage = 0;
    std::size_t logicalDirectoryChunks = 0;
    std::size_t logicalPhysicalDirectoryChunks = 0;
    std::size_t uniqueDirectoryContentEntries = 0;
    std::size_t uniqueDirectoryChunkEntries = 0;
    std::unordered_map<std::uint64_t, BoxStoragePageSnapshot> pages;
    std::unordered_map<std::uintptr_t, std::size_t> directories;
    std::unordered_map<std::uintptr_t, BoxStorageDirectoryChunkSnapshot>
    physicalDirectoryChunks;
    std::unordered_map<std::string, std::uint64_t> pageContentClasses;
    std::unordered_set<DirectoryToken, DirectoryTokenHash>
    directoryEntryClasses;
    std::unordered_set<DirectoryContent, DirectoryContentHash>
    directoryContents;
    std::unordered_set<DirectoryContent, DirectoryContentHash>
    directoryChunkContents;

    void add(const BoxDomain &box)
    {
        const BoxStorageSnapshot snapshot = box.storageSnapshot();
        ++states;
        bottomStates += snapshot.bottom;
        logicalPageReferences += snapshot.pages.size();
        directoryAllocatedBytes += snapshot.directoryAllocatedBytes;
        directories.emplace(snapshot.directoryId,
                            snapshot.directoryRootAllocatedBytes);
        logicalPhysicalDirectoryChunks += snapshot.directoryChunks.size();
        for (const BoxStorageDirectoryChunkSnapshot &chunk :
                snapshot.directoryChunks)
            physicalDirectoryChunks.emplace(chunk.chunkId, chunk);
        if (slotsPerPage == 0)
            slotsPerPage = snapshot.slotsPerPage;
        else if (slotsPerPage != snapshot.slotsPerPage)
            throw std::runtime_error("inconsistent Box page width");

        DirectoryContent directory;
        directory.bottom = snapshot.bottom;
        directory.entries.reserve(snapshot.pages.size());
        DirectoryContent chunk;
        chunk.entries.reserve(DirectoryChunkEntries);
        for (const BoxStoragePageSnapshot &page : snapshot.pages)
        {
            const auto content = pageContentClasses.emplace(
                                     page.canonicalContent, pageContentClasses.size() + 1);
            const DirectoryToken token{page.pageIndex, content.first->second};
            directory.entries.push_back(token);
            directoryEntryClasses.insert(token);
            chunk.entries.push_back(token);
            if (chunk.entries.size() == DirectoryChunkEntries)
            {
                ++logicalDirectoryChunks;
                const std::size_t chunkEntries = chunk.entries.size();
                const auto inserted =
                    directoryChunkContents.insert(std::move(chunk));
                if (inserted.second)
                    uniqueDirectoryChunkEntries += chunkEntries;
                chunk = DirectoryContent{};
                chunk.entries.reserve(DirectoryChunkEntries);
            }
            pages.emplace(page.pageId, page);
        }
        if (!chunk.entries.empty())
        {
            ++logicalDirectoryChunks;
            const std::size_t chunkEntries = chunk.entries.size();
            const auto inserted = directoryChunkContents.insert(std::move(chunk));
            if (inserted.second)
                uniqueDirectoryChunkEntries += chunkEntries;
        }
        const std::size_t directoryEntries = directory.entries.size();
        const auto inserted = directoryContents.insert(std::move(directory));
        if (inserted.second)
            uniqueDirectoryContentEntries += directoryEntries;
    }

    void print(const char *role) const
    {
        OccupancyHistogram occupancy;
        std::size_t occupied = 0;
        std::size_t rationalBytes = 0;
        std::size_t pageBytes = 0;
        std::size_t maxReferences = 0;
        std::size_t uniqueDirectoryAllocatedBytes = 0;
        std::size_t uniqueDirectoryChunkPageEntries = 0;
        for (const auto& directory : directories)
            uniqueDirectoryAllocatedBytes += directory.second;
        for (const auto& entry : physicalDirectoryChunks)
        {
            uniqueDirectoryAllocatedBytes += entry.second.shallowBytes;
            uniqueDirectoryChunkPageEntries += entry.second.pageEntries;
        }
        for (const auto &entry : pages)
        {
            const BoxStoragePageSnapshot &page = entry.second;
            ++occupancy[page.occupiedSlots];
            occupied += page.occupiedSlots;
            rationalBytes += page.rationalUsedLimbBytes;
            pageBytes += page.shallowBytes;
            maxReferences = std::max(maxReferences, page.referenceCount);
        }
        const std::size_t uniquePages = pages.size();
        std::size_t nearestEditSum = 0;
        std::size_t nearestEditMax = 0;
        if (directoryContents.size() > 1)
        {
            for (auto left = directoryContents.begin();
                    left != directoryContents.end(); ++left)
            {
                std::size_t nearest = std::numeric_limits<std::size_t>::max();
                for (auto right = directoryContents.begin();
                        right != directoryContents.end(); ++right)
                {
                    if (left == right)
                        continue;
                    nearest = std::min(
                                  nearest, directoryEditDistance(*left, *right));
                }
                nearestEditSum += nearest;
                nearestEditMax = std::max(nearestEditMax, nearest);
            }
        }
        SVFUtil::outs()
                << "BOX_STORAGE_CARRIER role=" << role << " states=" << states
                << " bottom_states=" << bottomStates
                << " logical_page_refs=" << logicalPageReferences
                << " unique_pages=" << uniquePages
                << " content_classes=" << pageContentClasses.size()
                << " cow_saved_refs="
                << (logicalPageReferences >= uniquePages
                    ? logicalPageReferences - uniquePages
                    : 0)
                << " duplicate_physical_pages="
                << (uniquePages >= pageContentClasses.size()
                    ? uniquePages - pageContentClasses.size()
                    : 0)
                << " occupied_slots=" << occupied
                << " empty_slots=" << uniquePages * slotsPerPage - occupied
                << " max_reference_count=" << maxReferences
                << " directory_allocated_bytes=" << directoryAllocatedBytes
                << " unique_directories=" << directories.size()
                << " logical_physical_directory_chunks="
                << logicalPhysicalDirectoryChunks
                << " logical_physical_directory_chunk_empty_slots="
                << logicalPhysicalDirectoryChunks * DirectoryChunkEntries -
                logicalPageReferences
                << " unique_directory_chunks="
                << physicalDirectoryChunks.size()
                << " unique_directory_chunk_page_entries="
                << uniqueDirectoryChunkPageEntries
                << " unique_directory_chunk_empty_slots="
                << physicalDirectoryChunks.size() * DirectoryChunkEntries -
                uniqueDirectoryChunkPageEntries
                << " unique_directory_allocated_bytes="
                << uniqueDirectoryAllocatedBytes
                << " unique_page_shallow_bytes=" << pageBytes
                << " unique_index_shallow_bytes=" << occupied * sizeof(Variable)
                << " unique_interval_shallow_bytes=" << occupied * sizeof(Interval)
                << " unique_rational_used_limb_bytes=" << rationalBytes
                << " directory_content_classes=" << directoryContents.size()
                << " duplicate_state_directories="
                << (states >= directoryContents.size()
                    ? states - directoryContents.size()
                    : 0)
                << " unique_directory_entry_classes="
                << directoryEntryClasses.size()
                << " unique_directory_content_entries="
                << uniqueDirectoryContentEntries
                << " directory_chunk_entries=" << DirectoryChunkEntries
                << " logical_directory_chunks=" << logicalDirectoryChunks
                << " directory_chunk_classes="
                << directoryChunkContents.size()
                << " unique_directory_chunk_entries="
                << uniqueDirectoryChunkEntries
                << " reusable_directory_chunks="
                << (logicalDirectoryChunks >= directoryChunkContents.size()
                    ? logicalDirectoryChunks - directoryChunkContents.size()
                    : 0)
                << " nearest_directory_edit_sum=" << nearestEditSum
                << " nearest_directory_edit_max=" << nearestEditMax
                << '\n';
        // Count each physical page once per carrier, not once per state reference.
        printOccupancy("retained", role, occupancy);
    }
};

void testStorageOccupancy()
{
    const auto check = [](bool condition)
    {
        if (!condition)
            throw std::runtime_error("Box occupancy event contract failed");
    };
    // Validate the observation boundary against real mutations for every density.
    const std::size_t width = BoxDomain::top().storageSnapshot().slotsPerPage;
    for (std::size_t used = 1; used <= width; ++used)
    {
        storageEvents = StorageEvents{};
        {
            BoxDomain box = BoxDomain::top();
            for (std::size_t slot = 0; slot < used; ++slot)
                box.assign(Variable(slot), LinearExpression(Rational(slot)));
            check(storageEvents.occupancy[eventIndex(BoxStorageEventKind::PageAllocate)] ==
            OccupancyHistogram{{0, 1}});
            const auto &writes = storageEvents.occupancy[
                                     eventIndex(BoxStorageEventKind::PageWriteUnique)];
            // Assignment writes once, then canonicalization writes again.
            // These count physical writable-page requests, not AE statements.
            check(writes.size() == used);
            for (std::size_t prior = 1; prior < used; ++prior)
                check(writes.at(prior) == 2);
            check(writes.at(used) == 1);
            BoxDomain copy = box;
            copy.forget(Variable(0));
            check(storageEvents.occupancy[eventIndex(BoxStorageEventKind::PageDetach)] ==
            OccupancyHistogram{{used, 1}});
            check(box.storageSnapshot().pages.front().occupiedSlots == used);
            if (used > 1)
            {
                check(copy.storageSnapshot().pages.front().occupiedSlots == used - 1);
                copy.forget(Variable(1));
                check(storageEvents.occupancy[eventIndex(BoxStorageEventKind::PageEraseUnique)] ==
                OccupancyHistogram{{used - 1, 1}});
            }
        }
        check(storageEvents.livePages.empty());
    }
    storageEvents = StorageEvents{};
    {
        BoxDomain box = BoxDomain::top();
        for (std::size_t slot = 0; slot < 6; ++slot)
            box.assign(Variable(slot), LinearExpression(Rational(slot)));
        BoxDomain copy = box;
        const std::uint64_t detaches = storageEvents.counts[
                                           eventIndex(BoxStorageEventKind::PageDetach)];
        copy.assign(Variable(7), LinearExpression(Variable(8)));
        check(storageEvents.counts[
                  eventIndex(BoxStorageEventKind::PageDetach)] == detaches);
        check(copy.bound(Variable(7)).isTop());
    }
    check(storageEvents.livePages.empty());
    storageEvents = StorageEvents{};
    SVFUtil::outs() << "Box occupancy event contract passed\n";
}

const BoxAddressDomain *
boxAddress(const SVF::AbstractDomain::AbstractDomain &domain)
{
    return dynamic_cast<const BoxAddressDomain *>(&domain);
}
} // namespace
#endif

int main(int argc, char **argv)
{
#ifdef SVF_BOX_STORAGE_TELEMETRY
    const char *operationCensusValue = std::getenv("BOX_OPERATION_CENSUS");
    const bool operationCensusEnabled = operationCensusValue &&
                                        std::string_view(operationCensusValue) != "0";
    const char *cowriteCensusPath = std::getenv("BOX_COWRITE_CENSUS_PATH");
    const bool cowriteCensusEnabled = cowriteCensusPath && *cowriteCensusPath;
    BoxDomain::setStorageEventSink(collectStorageEvent);
    BoxDomain::setStorageWorkSink(collectStorageWork);
    if (cowriteCensusEnabled)
    {
        cowriteCensus.open(cowriteCensusPath);
        BoxDomain::setMutationEventSink(collectMutationEvent);
        BoxDomain::setStateEventSink(collectStateEvent);
    }
    if (operationCensusEnabled)
        SVF::AbstractDomain::AbstractDomain::setOperationEventSink(
            BoxOperationCensus::collect);
    if (argc == 2 && std::string_view(argv[1]) == "--storage-occupancy-self-test")
    {
        BoxOperationCensus::selfTest();
        testStorageOccupancy();
        BoxDomain::setStorageEventSink(nullptr);
        BoxDomain::setStorageWorkSink(nullptr);
        BoxDomain::setMutationEventSink(nullptr);
        BoxDomain::setStateEventSink(nullptr);
        SVF::AbstractDomain::AbstractDomain::setOperationEventSink(nullptr);
        return 0;
    }
    peakMemoryCensus.start();
#endif
    std::vector<char *> arguments(argv, argv + argc);
    arguments.reserve(static_cast<std::size_t>(argc) + 3);
    const auto hasOption = [&](std::string_view option)
    {
        for (int index = 1; index < argc; ++index)
        {
            const std::string_view argument(argv[index]);
            if (argument == option ||
                    (argument.size() > option.size() &&
                     argument.compare(0, option.size(), option) == 0 &&
                     argument[option.size()] == '='))
                return true;
        }
        return false;
    };
    const auto addDefault = [&](std::string_view option, char *value)
    {
        if (!hasOption(option))
            arguments.push_back(value);
    };
    addDefault("-model-consts", const_cast<char *>("-model-consts=true"));
    addDefault("-model-arrays", const_cast<char *>("-model-arrays=true"));
    addDefault("-pre-field-sensitive",
               const_cast<char *>("-pre-field-sensitive=false"));

    const std::vector<std::string> modules = OptionBase::parseOptions(
            static_cast<int>(arguments.size()), arguments.data(),
            "Static Symbolic Execution", "[options] <input-bitcode...>");

    LLVMModuleSet::getLLVMModuleSet()->buildSVFModule(modules);
    SVFIRBuilder builder;
    SVFIR *pag = builder.build();
    AndersenWaveDiff *ander = AndersenWaveDiff::createAndersenWaveDiff(pag);
    builder.updateCallGraph(ander->getCallGraph());

    AbstractInterpretation &ae = AbstractInterpretation::getAEInstance();
    if (Options::BufferOverflowCheck())
        ae.addDetector(std::make_unique<BufOverflowDetector>());
    if (Options::NullDerefCheck())
        ae.addDetector(std::make_unique<NullptrDerefDetector>());
    ae.runOnModule();

#ifdef SVF_BOX_STORAGE_TELEMETRY
    peakMemoryCensus.stopAndPrint();
    CarrierStorage scalarStorage;
    CarrierStorage nodeStorage;
    const SVF::AbstractDomain::AbstractDomain *scalar =
        ae.getScalarAbstractState();
    if (scalar)
    {
        if (const BoxAddressDomain *product = boxAddress(*scalar))
            scalarStorage.add(product->numerical());
    }
    for (const ICFGNode *node : ae.getAnalyzedNodes())
    {
        if (!ae.hasAbsState(node))
            continue;
        if (const BoxAddressDomain *product = boxAddress(ae.getAbstractState(node)))
            nodeStorage.add(product->numerical());
    }
    scalarStorage.print("scalar");
    nodeStorage.print(scalar ? "memory" : "combined");
    std::unordered_set<std::uint64_t> retainedPages;
    for (const auto &page : scalarStorage.pages)
        retainedPages.insert(page.first);
    for (const auto &page : nodeStorage.pages)
        retainedPages.insert(page.first);
    SVFUtil::outs()
            << "BOX_STORAGE_EVENTS allocate="
            << storageEvents.counts[eventIndex(BoxStorageEventKind::PageAllocate)]
            << " detach="
            << storageEvents.counts[eventIndex(BoxStorageEventKind::PageDetach)]
            << " release="
            << storageEvents.counts[eventIndex(BoxStorageEventKind::PageRelease)]
            << " write_unique="
            << storageEvents.counts[eventIndex(BoxStorageEventKind::PageWriteUnique)]
            << " erase_unique="
            << storageEvents.counts[eventIndex(BoxStorageEventKind::PageEraseUnique)]
            << " join_shared="
            << storageEvents.counts[eventIndex(BoxStorageEventKind::JoinSharedPage)]
            << " join_materialized="
            << storageEvents
            .counts[eventIndex(BoxStorageEventKind::JoinMaterializedPage)]
            << " directory_detach="
            << storageEvents.counts[eventIndex(BoxStorageEventKind::DirectoryDetach)]
            << " directory_entries_copied="
            << storageEvents.directoryEntriesCopied
            << " directory_chunk_detach="
            << storageEvents
            .counts[eventIndex(BoxStorageEventKind::DirectoryChunkDetach)]
            << " directory_chunk_entries_copied="
            << storageEvents.directoryChunkEntriesCopied
            << " live_pages=" << storageEvents.livePages.size()
            << " peak_live_pages=" << storageEvents.peakLivePages
            << " retained_snapshot_pages=" << retainedPages.size() << '\n';
    // Mutating events are emitted before the mutation. A detach is observed
    // after cloning but before either write or erase; these are event-weighted
    // samples, not time-weighted density or a peak-byte snapshot.
    const std::array<const char *, 7> eventNames{{
            "allocate_empty", "detach_before", "release", "write_unique_before",
            "erase_unique_before", "join_shared", "join_clone_before"
        }};
    static_assert(static_cast<std::size_t>(BoxStorageEventKind::DirectoryDetach) ==
                  eventNames.size(), "update occupancy event names");
    for (std::size_t index = 0; index < eventNames.size(); ++index)
        printOccupancy("event", eventNames[index], storageEvents.occupancy[index]);
    printStorageWork();
    if (cowriteCensusEnabled)
        cowriteCensus.print();
    if (operationCensusEnabled)
        BoxOperationCensus::print();
#endif

    if (!std::getenv("BOX_STORAGE_CENSUS_ONLY"))
    {
        for (auto it = pag->getICFG()->begin(); it != pag->getICFG()->end(); ++it)
        {
            const auto *node = it->second;
            if (!ae.hasAbsState(node))
                continue;
            for (const auto *stmt : node->getSVFStmts())
            {
                const SVFVar *value = nullptr;
                const char *operation = nullptr;
                std::string source;
                if (const auto *load = SVFUtil::dyn_cast<LoadStmt>(stmt))
                {
                    value = load->getLHSVar();
                    source = load->getRHSVar()->getValueName();
                    operation = "LOAD";
                }
                else if (const auto *binary = SVFUtil::dyn_cast<BinaryOPStmt>(stmt))
                {
                    value = binary->getRes();
                    operation = "BINARY";
                }
                if (!value)
                    continue;
                SVFUtil::outs() << "AUDIT " << operation << " node=" << node->getId()
                                << " source=" << source
                                << " pointer=" << value->isPointer();
                if (value->isPointer())
                {
#if AUDIT_BOX
                    const auto addresses = ae.getAddressSet(value, node);
                    SVFUtil::outs() << " empty=" << addresses.isBottom()
                                    << " top=" << addresses.isTop()
                                    << " addresses=" << addresses.toString();
#else
                    const auto addresses = ae.getAbsValue(value, node).getAddrs();
                    SVFUtil::outs() << " empty=" << addresses.isBottom()
                                    << " count=" << addresses.size();
#endif
                }
                else
                {
#if AUDIT_BOX
                    const auto interval = ae.getInterval(value, node);
#else
                    const auto interval = ae.getAbsValue(value, node).getInterval();
#endif
                    SVFUtil::outs() << " top=" << interval.isTop()
                                    << " bottom=" << interval.isBottom()
                                    << " interval=" << interval.toString();
                }
                SVFUtil::outs() << '\n';
            }
        }
    }

#if AUDIT_BOX
    if (std::getenv("AUDIT_MEMORY_POLICY"))
    {
        bool checked = false;
        for (auto it = pag->getICFG()->begin();
                it != pag->getICFG()->end() && !checked; ++it)
        {
            const auto *node = it->second;
            if (!ae.hasAbsState(node))
                continue;
            for (const auto *stmt : node->getSVFStmts())
            {
                const auto *load = SVFUtil::dyn_cast<LoadStmt>(stmt);
                if (!load)
                    continue;
                for (NodeID id : ander->getPts(load->getRHSVar()->getId()))
                {
                    const auto *object = SVFUtil::dyn_cast<ObjVar>(pag->getSVFVar(id));
                    if (!object)
                        continue;
                    const auto savedInterval = ae.getInterval(object, node);
                    const auto savedAddresses = ae.getAddressSet(object, node);
                    ae.updateValue(object, SVF::AbstractDomain::Interval::top(),
                                   SVF::AbstractDomain::AddressSet::top(), node);
                    if (!ae.getAddressSet(object, node).isTop())
                        throw std::runtime_error(
                            "Explicit memory Top did not survive round trip");
                    ae.updateValue(object, SVF::AbstractDomain::Interval::top(),
                                   SVF::AbstractDomain::AddressSet::bottom(), node);
                    if (!ae.getAddressSet(object, node).isBottom())
                        throw std::runtime_error(
                            "Explicit memory Empty did not survive round trip");
                    ae.updateValue(object, savedInterval, savedAddresses, node);
                    checked = true;
                    break;
                }
                if (checked)
                    break;
            }
        }
        if (!checked)
            throw std::runtime_error("Memory round-trip test found no target");
        SVFUtil::outs() << "AUDIT_POLICY_PASS Top and Empty memory round trips\n";
    }
#endif

    AndersenWaveDiff::releaseAndersenWaveDiff();
    LLVMModuleSet::releaseLLVMModuleSet();
#ifdef SVF_BOX_STORAGE_TELEMETRY
    BoxDomain::setStorageEventSink(nullptr);
    BoxDomain::setStorageWorkSink(nullptr);
    BoxDomain::setMutationEventSink(nullptr);
    BoxDomain::setStateEventSink(nullptr);
    SVF::AbstractDomain::AbstractDomain::setOperationEventSink(nullptr);
#endif
    return 0;
}
