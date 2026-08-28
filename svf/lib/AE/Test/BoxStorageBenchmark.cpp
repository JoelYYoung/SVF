//===- BoxStorageBenchmark.cpp -- Box layout experiment ----------------===//

#include "AE/Core/NumericPrimitives.h"
#include "AE/Core/VariableEnvironment.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <sys/resource.h>

using namespace SVF::AbstractDomain;

namespace
{

using Clock = std::chrono::steady_clock;

struct Options
{
    std::string scheme = "dimension-page";
    std::string workload = "read";
    std::size_t pageSize = 64;
    std::size_t variables = 16384;
    double density = 0.1;
    std::uint32_t idStride = 1;
    std::size_t operations = 0;
    std::size_t repetitions = 9;
};

struct EnvironmentPair
{
    VariableEnvironment first;
    VariableEnvironment second;
    VariableEnvironment extended;
    std::vector<Variable> common;
    std::vector<Variable> firstOnly;
    std::vector<Variable> secondOnly;
    std::vector<Variable> temporaries;
};

std::uint64_t mix(std::uint64_t value)
{
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
}

Interval valueFor(Variable variable, std::uint64_t salt = 0)
{
    const std::int64_t value =
        static_cast<std::int64_t>(
            mix(static_cast<std::uint64_t>(variable.id()) + salt) %
            2000001ULL) -
        1000000;
    return Interval::singleton(Rational(value));
}

std::int64_t finiteValue(const Interval& interval)
{
    if (interval.isTop())
        return 0;
    if (!interval.lower().isFinite() || interval.lower().isStrict() ||
        interval.lower() != interval.upper())
        throw std::logic_error("benchmark expected a singleton interval");
    return interval.lower().value().value().get_num().get_si();
}

Interval intervalHull(const Interval& lhs, const Interval& rhs)
{
    const Bound lower = lhs.lower() <= rhs.lower() ? lhs.lower() : rhs.lower();
    const Bound upper = lhs.upper() <= rhs.upper() ? rhs.upper() : lhs.upper();
    return Interval(lower, upper);
}

bool intervalSubset(const Interval& lhs, const Interval& rhs)
{
    return rhs.lower() <= lhs.lower() && lhs.upper() <= rhs.upper();
}

VariableDeclaration declaration(Variable variable)
{
    return {variable, NumericType::integer(),
            "v" + std::to_string(variable.id())};
}

EnvironmentPair makeEnvironments(std::size_t variables, std::uint32_t stride)
{
    if (variables < 8)
        throw std::invalid_argument("variables must be at least eight");
    if (stride == 0)
        throw std::invalid_argument("id stride must be positive");

    EnvironmentPair result;
    const std::size_t commonCount = variables / 4;
    const std::size_t localCount = variables - commonCount;
    std::vector<VariableDeclaration> firstDeclarations;
    std::vector<VariableDeclaration> secondDeclarations;
    firstDeclarations.reserve(variables);
    secondDeclarations.reserve(variables);

    std::uint64_t ordinal = 0;
    const auto allocate = [&]() {
        const std::uint64_t id = 1 + ordinal++ * stride;
        if (id > std::numeric_limits<std::uint32_t>::max())
            throw std::overflow_error("generated Variable ID exceeds uint32");
        return Variable(static_cast<std::uint32_t>(id));
    };

    for (std::size_t index = 0; index < commonCount; ++index)
    {
        const Variable common = allocate();
        const Variable firstLocal = allocate();
        const Variable secondLocal = allocate();
        result.common.push_back(common);
        result.firstOnly.push_back(firstLocal);
        result.secondOnly.push_back(secondLocal);
        firstDeclarations.push_back(declaration(common));
        firstDeclarations.push_back(declaration(firstLocal));
        secondDeclarations.push_back(declaration(common));
        secondDeclarations.push_back(declaration(secondLocal));
    }
    while (result.firstOnly.size() < localCount)
    {
        const Variable firstLocal = allocate();
        const Variable secondLocal = allocate();
        result.firstOnly.push_back(firstLocal);
        result.secondOnly.push_back(secondLocal);
        firstDeclarations.push_back(declaration(firstLocal));
        secondDeclarations.push_back(declaration(secondLocal));
    }

    result.first = VariableEnvironment(std::move(firstDeclarations));
    result.second = VariableEnvironment(std::move(secondDeclarations));

    std::vector<VariableDeclaration> extended = result.first.variables();
    const std::size_t temporaryCount = std::max<std::size_t>(4, variables / 32);
    extended.reserve(extended.size() + temporaryCount);
    for (std::size_t index = 0; index < temporaryCount; ++index)
    {
        const Variable temporary = allocate();
        result.temporaries.push_back(temporary);
        extended.push_back(declaration(temporary));
    }
    result.extended = VariableEnvironment(std::move(extended));
    return result;
}

std::vector<Variable> activeVariables(const VariableEnvironment& environment,
                                      double density)
{
    if (!(density > 0.0 && density <= 1.0))
        throw std::invalid_argument("density must be in (0,1]");
    std::vector<std::pair<std::uint64_t, Variable>> ranked;
    ranked.reserve(environment.size());
    for (const VariableDeclaration& item : environment.variables())
        ranked.emplace_back(mix(item.variable.id()), item.variable);
    std::sort(ranked.begin(), ranked.end(),
              [](const auto& lhs, const auto& rhs) {
                  if (lhs.first != rhs.first)
                      return lhs.first < rhs.first;
                  return lhs.second < rhs.second;
              });
    const std::size_t count = std::max<std::size_t>(
        1, static_cast<std::size_t>(std::llround(
               density * static_cast<double>(environment.size()))));
    std::vector<Variable> result;
    result.reserve(count);
    for (std::size_t index = 0; index < count; ++index)
        result.push_back(ranked[index].second);
    return result;
}

template <std::size_t PageSize, bool VariableKeyed> class PagedStorage
{
public:
    explicit PagedStorage(VariableEnvironment environment)
        : environment_(std::move(environment))
    {
    }

    const VariableEnvironment& environment() const
    {
        return environment_;
    }

    const Interval& bound(Variable variable) const
    {
        if (!environment_.contains(variable))
            throw std::invalid_argument("variable is outside the environment");
        return boundAt(storageKey(variable));
    }

    void set(Variable variable, Interval interval)
    {
        if (!environment_.contains(variable))
            throw std::invalid_argument("variable is outside the environment");
        setAt(storageKey(variable), std::move(interval));
    }

    void changeEnvironment(const VariableEnvironment& nextEnvironment,
                           bool initializeNewVariablesToZero = false)
    {
        if (environment_ == nextEnvironment)
            return;
        validateTypes(nextEnvironment);
        if constexpr (VariableKeyed)
        {
            const std::vector<Variable> active = boundedVariables();
            for (Variable variable : active)
            {
                if (!nextEnvironment.contains(variable))
                    eraseAt(variable.id());
            }
            if (initializeNewVariablesToZero)
            {
                for (const VariableDeclaration& item :
                     nextEnvironment.variables())
                {
                    if (!environment_.contains(item.variable))
                        setAt(item.variable.id(),
                              Interval::singleton(Rational()));
                }
            }
            environment_ = nextEnvironment;
            return;
        }

        PagedStorage next(nextEnvironment);
        for (std::size_t oldKey : boundedKeys())
        {
            const Variable variable = environment_.variableOf(oldKey);
            if (nextEnvironment.contains(variable))
                next.setAt(nextEnvironment.dimensionOf(variable),
                           boundAt(oldKey));
        }
        if (initializeNewVariablesToZero)
        {
            for (const VariableDeclaration& item : nextEnvironment.variables())
            {
                if (!environment_.contains(item.variable))
                    next.setAt(nextEnvironment.dimensionOf(item.variable),
                               Interval::singleton(Rational()));
            }
        }
        *this = std::move(next);
    }

    std::size_t storageUnits() const
    {
        return pages_.size();
    }

    std::size_t materializedValues() const
    {
        std::size_t result = 0;
        for (const Entry& entry : pages_)
        {
            result += static_cast<std::size_t>(std::count_if(
                entry.page->values.begin(), entry.page->values.end(),
                [](const auto& value) { return value.has_value(); }));
        }
        return result;
    }

    std::size_t allocatedPageSlots() const
    {
        return pages_.size() * PageSize;
    }

private:
    struct Page
    {
        std::array<std::optional<Interval>, PageSize> values;
    };

    struct Entry
    {
        std::size_t index;
        std::shared_ptr<Page> page;
    };

    std::size_t storageKey(Variable variable) const
    {
        if constexpr (VariableKeyed)
            return variable.id();
        return environment_.dimensionOf(variable);
    }

    const Interval& boundAt(std::size_t key) const
    {
        static const Interval top = Interval::top();
        const std::size_t pageIndex = key / PageSize;
        const auto iterator =
            std::lower_bound(pages_.begin(), pages_.end(), pageIndex,
                             [](const Entry& entry, std::size_t index) {
                                 return entry.index < index;
                             });
        if (iterator == pages_.end() || iterator->index != pageIndex)
            return top;
        const auto& value = iterator->page->values[key % PageSize];
        return value ? *value : top;
    }

    Page& writablePage(std::size_t pageIndex)
    {
        auto iterator =
            std::lower_bound(pages_.begin(), pages_.end(), pageIndex,
                             [](const Entry& entry, std::size_t index) {
                                 return entry.index < index;
                             });
        if (iterator == pages_.end() || iterator->index != pageIndex)
            iterator =
                pages_.insert(iterator, {pageIndex, std::make_shared<Page>()});
        else if (iterator->page.use_count() != 1)
            iterator->page = std::make_shared<Page>(*iterator->page);
        return *iterator->page;
    }

    void setAt(std::size_t key, Interval interval)
    {
        if (interval.isTop())
        {
            eraseAt(key);
            return;
        }
        writablePage(key / PageSize).values[key % PageSize] =
            std::move(interval);
    }

    void eraseAt(std::size_t key)
    {
        const std::size_t pageIndex = key / PageSize;
        auto iterator =
            std::lower_bound(pages_.begin(), pages_.end(), pageIndex,
                             [](const Entry& entry, std::size_t index) {
                                 return entry.index < index;
                             });
        if (iterator == pages_.end() || iterator->index != pageIndex)
            return;
        auto& value = iterator->page->values[key % PageSize];
        if (!value)
            return;
        if (iterator->page.use_count() != 1)
            iterator->page = std::make_shared<Page>(*iterator->page);
        iterator->page->values[key % PageSize].reset();
        if (std::none_of(iterator->page->values.begin(),
                         iterator->page->values.end(),
                         [](const auto& item) { return item.has_value(); }))
            pages_.erase(iterator);
    }

    std::vector<std::size_t> boundedKeys() const
    {
        std::vector<std::size_t> result;
        for (const Entry& entry : pages_)
        {
            for (std::size_t offset = 0; offset < PageSize; ++offset)
            {
                if (entry.page->values[offset])
                    result.push_back(entry.index * PageSize + offset);
            }
        }
        return result;
    }

    std::vector<Variable> boundedVariables() const
    {
        std::vector<Variable> result;
        for (std::size_t key : boundedKeys())
            result.emplace_back(static_cast<std::uint32_t>(key));
        return result;
    }

    void validateTypes(const VariableEnvironment& nextEnvironment) const
    {
        for (const VariableDeclaration& item : nextEnvironment.variables())
        {
            if (environment_.contains(item.variable) &&
                environment_.typeOf(item.variable) != item.type)
                throw std::invalid_argument(
                    "environment change modifies a variable type");
        }
    }

    VariableEnvironment environment_;
    std::vector<Entry> pages_;
};

/// Same page payload and page-level COW as PagedStorage, but with a hash-table
/// directory. The directory is held by value so state copies measure the real
/// std::unordered_map copy cost while Interval payloads remain page-shared.
template <std::size_t PageSize, bool VariableKeyed> class HashPagedStorage
{
public:
    explicit HashPagedStorage(VariableEnvironment environment)
        : environment_(std::move(environment))
    {
    }

    const VariableEnvironment& environment() const
    {
        return environment_;
    }

    const Interval& bound(Variable variable) const
    {
        if (!environment_.contains(variable))
            throw std::invalid_argument("variable is outside the environment");
        return boundAt(storageKey(variable));
    }

    void set(Variable variable, Interval interval)
    {
        if (!environment_.contains(variable))
            throw std::invalid_argument("variable is outside the environment");
        setAt(storageKey(variable), std::move(interval));
    }

    void changeEnvironment(const VariableEnvironment& nextEnvironment,
                           bool initializeNewVariablesToZero = false)
    {
        if (environment_ == nextEnvironment)
            return;
        validateTypes(nextEnvironment);
        if constexpr (VariableKeyed)
        {
            for (Variable variable : boundedVariables())
            {
                if (!nextEnvironment.contains(variable))
                    eraseAt(variable.id());
            }
            if (initializeNewVariablesToZero)
            {
                for (const VariableDeclaration& item :
                     nextEnvironment.variables())
                {
                    if (!environment_.contains(item.variable))
                        setAt(item.variable.id(),
                              Interval::singleton(Rational()));
                }
            }
            environment_ = nextEnvironment;
            return;
        }

        HashPagedStorage next(nextEnvironment);
        next.pages_.reserve(pages_.size());
        for (std::size_t oldKey : boundedKeys())
        {
            const Variable variable = environment_.variableOf(oldKey);
            if (nextEnvironment.contains(variable))
                next.setAt(nextEnvironment.dimensionOf(variable),
                           boundAt(oldKey));
        }
        if (initializeNewVariablesToZero)
        {
            for (const VariableDeclaration& item : nextEnvironment.variables())
            {
                if (!environment_.contains(item.variable))
                    next.setAt(nextEnvironment.dimensionOf(item.variable),
                               Interval::singleton(Rational()));
            }
        }
        *this = std::move(next);
    }

    std::size_t storageUnits() const
    {
        return pages_.bucket_count();
    }

    std::size_t materializedValues() const
    {
        std::size_t result = 0;
        for (const auto& [index, page] : pages_)
        {
            (void)index;
            result += static_cast<std::size_t>(std::count_if(
                page->values.begin(), page->values.end(),
                [](const auto& value) { return value.has_value(); }));
        }
        return result;
    }

    std::size_t allocatedPageSlots() const
    {
        return pages_.size() * PageSize;
    }

private:
    struct Page
    {
        std::array<std::optional<Interval>, PageSize> values;
    };

    std::size_t storageKey(Variable variable) const
    {
        if constexpr (VariableKeyed)
            return variable.id();
        return environment_.dimensionOf(variable);
    }

    const Interval& boundAt(std::size_t key) const
    {
        static const Interval top = Interval::top();
        const auto iterator = pages_.find(key / PageSize);
        if (iterator == pages_.end())
            return top;
        const auto& value = iterator->second->values[key % PageSize];
        return value ? *value : top;
    }

    Page& writablePage(std::size_t pageIndex)
    {
        auto [iterator, inserted] =
            pages_.try_emplace(pageIndex, std::make_shared<Page>());
        if (!inserted && iterator->second.use_count() != 1)
            iterator->second = std::make_shared<Page>(*iterator->second);
        return *iterator->second;
    }

    void setAt(std::size_t key, Interval interval)
    {
        if (interval.isTop())
        {
            eraseAt(key);
            return;
        }
        writablePage(key / PageSize).values[key % PageSize] =
            std::move(interval);
    }

    void eraseAt(std::size_t key)
    {
        const std::size_t pageIndex = key / PageSize;
        auto iterator = pages_.find(pageIndex);
        if (iterator == pages_.end() ||
            !iterator->second->values[key % PageSize])
            return;
        if (iterator->second.use_count() != 1)
            iterator->second = std::make_shared<Page>(*iterator->second);
        iterator->second->values[key % PageSize].reset();
        if (std::none_of(iterator->second->values.begin(),
                         iterator->second->values.end(),
                         [](const auto& item) { return item.has_value(); }))
            pages_.erase(iterator);
    }

    std::vector<std::size_t> boundedKeys() const
    {
        std::vector<std::size_t> result;
        for (const auto& [pageIndex, page] : pages_)
        {
            for (std::size_t offset = 0; offset < PageSize; ++offset)
            {
                if (page->values[offset])
                    result.push_back(pageIndex * PageSize + offset);
            }
        }
        return result;
    }

    std::vector<Variable> boundedVariables() const
    {
        std::vector<Variable> result;
        for (std::size_t key : boundedKeys())
            result.emplace_back(static_cast<std::uint32_t>(key));
        return result;
    }

    void validateTypes(const VariableEnvironment& nextEnvironment) const
    {
        for (const VariableDeclaration& item : nextEnvironment.variables())
        {
            if (environment_.contains(item.variable) &&
                environment_.typeOf(item.variable) != item.type)
                throw std::invalid_argument(
                    "environment change modifies a variable type");
        }
    }

    VariableEnvironment environment_;
    std::unordered_map<std::size_t, std::shared_ptr<Page>> pages_;
};

/// Page payloads indexed by a persistent radix directory. Both the directory
/// and untouched pages are shared by state copies; changing one slot copies
/// one page plus one fixed-depth directory path.
template <std::size_t PageSize, bool VariableKeyed> class RadixPagedStorage
{
public:
    explicit RadixPagedStorage(VariableEnvironment environment)
        : environment_(std::move(environment))
    {
    }

    const VariableEnvironment& environment() const
    {
        return environment_;
    }

    const Interval& bound(Variable variable) const
    {
        if (!environment_.contains(variable))
            throw std::invalid_argument("variable is outside the environment");
        return boundAt(storageKey(variable));
    }

    void set(Variable variable, Interval interval)
    {
        if (!environment_.contains(variable))
            throw std::invalid_argument("variable is outside the environment");
        setAt(storageKey(variable), std::move(interval));
    }

    void changeEnvironment(const VariableEnvironment& nextEnvironment,
                           bool initializeNewVariablesToZero = false)
    {
        if (environment_ == nextEnvironment)
            return;
        validateTypes(nextEnvironment);
        if constexpr (VariableKeyed)
        {
            for (Variable variable : boundedVariables())
            {
                if (!nextEnvironment.contains(variable))
                    eraseAt(variable.id());
            }
            if (initializeNewVariablesToZero)
            {
                for (const VariableDeclaration& item :
                     nextEnvironment.variables())
                {
                    if (!environment_.contains(item.variable))
                        setAt(item.variable.id(),
                              Interval::singleton(Rational()));
                }
            }
            environment_ = nextEnvironment;
            return;
        }

        RadixPagedStorage next(nextEnvironment);
        for (std::size_t oldKey : boundedKeys())
        {
            const Variable variable = environment_.variableOf(oldKey);
            if (nextEnvironment.contains(variable))
                next.setAt(nextEnvironment.dimensionOf(variable),
                           boundAt(oldKey));
        }
        if (initializeNewVariablesToZero)
        {
            for (const VariableDeclaration& item : nextEnvironment.variables())
            {
                if (!environment_.contains(item.variable))
                    next.setAt(nextEnvironment.dimensionOf(item.variable),
                               Interval::singleton(Rational()));
            }
        }
        *this = std::move(next);
    }

    std::size_t storageUnits() const
    {
        return countNodes(root_);
    }

    std::size_t materializedValues() const
    {
        std::size_t result = 0;
        for (const auto& [index, page] : pages())
        {
            (void)index;
            result += static_cast<std::size_t>(std::count_if(
                page->values.begin(), page->values.end(),
                [](const auto& value) { return value.has_value(); }));
        }
        return result;
    }

    std::size_t allocatedPageSlots() const
    {
        return pageCount_ * PageSize;
    }

private:
    static constexpr unsigned BitsPerLevel = 2;
    static constexpr unsigned Fanout = 1U << BitsPerLevel;
    static constexpr unsigned Levels = 32 / BitsPerLevel;

    struct Page
    {
        std::array<std::optional<Interval>, PageSize> values;
    };

    struct Node
    {
        std::array<std::shared_ptr<const Node>, Fanout> children;
        std::shared_ptr<Page> page;
    };

    static unsigned branch(std::uint32_t key, unsigned level)
    {
        const unsigned shift = 32 - BitsPerLevel * (level + 1);
        return (key >> shift) & (Fanout - 1);
    }

    static bool empty(const Node& node)
    {
        return !node.page &&
               std::none_of(
                   node.children.begin(), node.children.end(),
                   [](const auto& child) { return static_cast<bool>(child); });
    }

    static const std::shared_ptr<Page>& lookup(
        const std::shared_ptr<const Node>& root, std::uint32_t key)
    {
        static const std::shared_ptr<Page> missing;
        const Node* node = root.get();
        for (unsigned level = 0; level < Levels && node; ++level)
            node = node->children[branch(key, level)].get();
        return node ? node->page : missing;
    }

    static std::shared_ptr<const Node> update(
        const std::shared_ptr<const Node>& current, std::uint32_t key,
        unsigned level, std::shared_ptr<Page> page)
    {
        auto next = current ? std::make_shared<Node>(*current)
                            : std::make_shared<Node>();
        if (level == Levels)
        {
            next->page = std::move(page);
            return next;
        }
        const unsigned index = branch(key, level);
        next->children[index] =
            update(next->children[index], key, level + 1, std::move(page));
        return next;
    }

    static std::shared_ptr<const Node> erase(
        const std::shared_ptr<const Node>& current, std::uint32_t key,
        unsigned level)
    {
        if (!current)
            return nullptr;
        auto next = std::make_shared<Node>(*current);
        if (level == Levels)
            next->page.reset();
        else
        {
            const unsigned index = branch(key, level);
            next->children[index] =
                erase(next->children[index], key, level + 1);
        }
        return empty(*next) ? nullptr : next;
    }

    static void collectPages(
        const std::shared_ptr<const Node>& node, unsigned level,
        std::uint32_t prefix,
        std::vector<std::pair<std::uint32_t, std::shared_ptr<Page>>>& result)
    {
        if (!node)
            return;
        if (level == Levels)
        {
            if (node->page)
                result.emplace_back(prefix, node->page);
            return;
        }
        const unsigned shift = 32 - BitsPerLevel * (level + 1);
        for (unsigned index = 0; index < Fanout; ++index)
            collectPages(node->children[index], level + 1,
                         prefix | (static_cast<std::uint32_t>(index) << shift),
                         result);
    }

    static std::size_t countNodes(const std::shared_ptr<const Node>& node)
    {
        if (!node)
            return 0;
        std::size_t result = 1;
        for (const auto& child : node->children)
            result += countNodes(child);
        return result;
    }

    std::vector<std::pair<std::uint32_t, std::shared_ptr<Page>>> pages() const
    {
        std::vector<std::pair<std::uint32_t, std::shared_ptr<Page>>> result;
        result.reserve(pageCount_);
        collectPages(root_, 0, 0, result);
        return result;
    }

    std::size_t storageKey(Variable variable) const
    {
        if constexpr (VariableKeyed)
            return variable.id();
        return environment_.dimensionOf(variable);
    }

    const Interval& boundAt(std::size_t key) const
    {
        static const Interval top = Interval::top();
        const auto& page = lookup(root_, checkedPageIndex(key));
        if (!page)
            return top;
        const auto& value = page->values[key % PageSize];
        return value ? *value : top;
    }

    void setAt(std::size_t key, Interval interval)
    {
        if (interval.isTop())
        {
            eraseAt(key);
            return;
        }
        const std::uint32_t pageIndex = checkedPageIndex(key);
        const auto& current = lookup(root_, pageIndex);
        auto page = current ? std::make_shared<Page>(*current)
                            : std::make_shared<Page>();
        if (!current)
            ++pageCount_;
        page->values[key % PageSize] = std::move(interval);
        root_ = update(root_, pageIndex, 0, std::move(page));
    }

    void eraseAt(std::size_t key)
    {
        const std::uint32_t pageIndex = checkedPageIndex(key);
        const auto& current = lookup(root_, pageIndex);
        if (!current || !current->values[key % PageSize])
            return;
        auto page = std::make_shared<Page>(*current);
        page->values[key % PageSize].reset();
        if (std::none_of(page->values.begin(), page->values.end(),
                         [](const auto& item) { return item.has_value(); }))
        {
            root_ = erase(root_, pageIndex, 0);
            --pageCount_;
        }
        else
        {
            root_ = update(root_, pageIndex, 0, std::move(page));
        }
    }

    std::vector<std::size_t> boundedKeys() const
    {
        std::vector<std::size_t> result;
        for (const auto& [pageIndex, page] : pages())
        {
            for (std::size_t offset = 0; offset < PageSize; ++offset)
            {
                if (page->values[offset])
                    result.push_back(static_cast<std::size_t>(pageIndex) *
                                         PageSize +
                                     offset);
            }
        }
        return result;
    }

    std::vector<Variable> boundedVariables() const
    {
        std::vector<Variable> result;
        for (std::size_t key : boundedKeys())
            result.emplace_back(static_cast<std::uint32_t>(key));
        return result;
    }

    static std::uint32_t checkedPageIndex(std::size_t key)
    {
        const std::size_t pageIndex = key / PageSize;
        if (pageIndex > std::numeric_limits<std::uint32_t>::max())
            throw std::overflow_error(
                "page index exceeds persistent radix key");
        return static_cast<std::uint32_t>(pageIndex);
    }

    void validateTypes(const VariableEnvironment& nextEnvironment) const
    {
        for (const VariableDeclaration& item : nextEnvironment.variables())
        {
            if (environment_.contains(item.variable) &&
                environment_.typeOf(item.variable) != item.type)
                throw std::invalid_argument(
                    "environment change modifies a variable type");
        }
    }

    VariableEnvironment environment_;
    std::shared_ptr<const Node> root_;
    std::size_t pageCount_ = 0;
};

/// Persistent sparse radix map keyed by the 32-bit Variable ID. A state copy
/// shares the root; a write path-copies 16 four-way nodes, leaving every other
/// value and index path shared.
class VariableRadixCOWStorage
{
public:
    explicit VariableRadixCOWStorage(VariableEnvironment environment)
        : environment_(std::move(environment))
    {
    }

    const VariableEnvironment& environment() const
    {
        return environment_;
    }

    const Interval& bound(Variable variable) const
    {
        static const Interval top = Interval::top();
        if (!environment_.contains(variable))
            throw std::invalid_argument("variable is outside the environment");
        const Node* node = root_.get();
        for (unsigned level = 0; level < Levels && node; ++level)
            node = node->children[branch(variable.id(), level)].get();
        return node && node->value ? *node->value : top;
    }

    void set(Variable variable, Interval interval)
    {
        if (!environment_.contains(variable))
            throw std::invalid_argument("variable is outside the environment");
        const bool existed = !bound(variable).isTop();
        if (interval.isTop())
        {
            if (existed)
            {
                root_ = erase(root_, variable.id(), 0);
                --size_;
            }
            return;
        }
        root_ = update(root_, variable.id(), 0, std::move(interval));
        if (!existed)
            ++size_;
    }

    void changeEnvironment(const VariableEnvironment& nextEnvironment,
                           bool initializeNewVariablesToZero = false)
    {
        if (environment_ == nextEnvironment)
            return;
        validateTypes(nextEnvironment);
        std::vector<std::pair<std::uint32_t, Interval>> entries;
        entries.reserve(size_);
        collect(root_, 0, 0, entries);
        for (const auto& [id, value] : entries)
        {
            (void)value;
            if (!nextEnvironment.contains(Variable(id)))
            {
                root_ = erase(root_, id, 0);
                --size_;
            }
        }
        if (initializeNewVariablesToZero)
        {
            for (const VariableDeclaration& item : nextEnvironment.variables())
            {
                if (!environment_.contains(item.variable))
                {
                    root_ = update(root_, item.variable.id(), 0,
                                   Interval::singleton(Rational()));
                    ++size_;
                }
            }
        }
        environment_ = nextEnvironment;
    }

    std::size_t storageUnits() const
    {
        return countNodes(root_);
    }
    std::size_t materializedValues() const
    {
        return size_;
    }
    std::size_t allocatedPageSlots() const
    {
        return 0;
    }

private:
    static constexpr unsigned BitsPerLevel = 2;
    static constexpr unsigned Fanout = 1U << BitsPerLevel;
    static constexpr unsigned Levels = 32 / BitsPerLevel;

    struct Node
    {
        std::array<std::shared_ptr<const Node>, Fanout> children;
        std::optional<Interval> value;
    };

    static unsigned branch(std::uint32_t id, unsigned level)
    {
        const unsigned shift = 32 - BitsPerLevel * (level + 1);
        return (id >> shift) & (Fanout - 1);
    }

    static bool empty(const Node& node)
    {
        return !node.value &&
               std::none_of(
                   node.children.begin(), node.children.end(),
                   [](const auto& child) { return static_cast<bool>(child); });
    }

    static std::shared_ptr<const Node> update(
        const std::shared_ptr<const Node>& current, std::uint32_t id,
        unsigned level, Interval value)
    {
        auto next = current ? std::make_shared<Node>(*current)
                            : std::make_shared<Node>();
        if (level == Levels)
        {
            next->value = std::move(value);
            return next;
        }
        const unsigned index = branch(id, level);
        next->children[index] =
            update(next->children[index], id, level + 1, std::move(value));
        return next;
    }

    static std::shared_ptr<const Node> erase(
        const std::shared_ptr<const Node>& current, std::uint32_t id,
        unsigned level)
    {
        if (!current)
            return nullptr;
        auto next = std::make_shared<Node>(*current);
        if (level == Levels)
            next->value.reset();
        else
        {
            const unsigned index = branch(id, level);
            next->children[index] = erase(next->children[index], id, level + 1);
        }
        return empty(*next) ? nullptr : next;
    }

    static void collect(
        const std::shared_ptr<const Node>& node, unsigned level,
        std::uint32_t prefix,
        std::vector<std::pair<std::uint32_t, Interval>>& entries)
    {
        if (!node)
            return;
        if (level == Levels)
        {
            if (node->value)
                entries.emplace_back(prefix, *node->value);
            return;
        }
        const unsigned shift = 32 - BitsPerLevel * (level + 1);
        for (unsigned index = 0; index < Fanout; ++index)
            collect(node->children[index], level + 1,
                    prefix | (static_cast<std::uint32_t>(index) << shift),
                    entries);
    }

    static std::size_t countNodes(const std::shared_ptr<const Node>& node)
    {
        if (!node)
            return 0;
        std::size_t result = 1;
        for (const auto& child : node->children)
            result += countNodes(child);
        return result;
    }

    void validateTypes(const VariableEnvironment& nextEnvironment) const
    {
        for (const VariableDeclaration& item : nextEnvironment.variables())
        {
            if (environment_.contains(item.variable) &&
                environment_.typeOf(item.variable) != item.type)
                throw std::invalid_argument(
                    "environment change modifies a variable type");
        }
    }

    VariableEnvironment environment_;
    std::shared_ptr<const Node> root_;
    std::size_t size_ = 0;
};

class VariableHashStorage
{
public:
    explicit VariableHashStorage(VariableEnvironment environment)
        : environment_(std::move(environment))
    {
    }

    const VariableEnvironment& environment() const
    {
        return environment_;
    }

    const Interval& bound(Variable variable) const
    {
        static const Interval top = Interval::top();
        if (!environment_.contains(variable))
            throw std::invalid_argument("variable is outside the environment");
        const auto iterator = bounds_.find(variable.id());
        return iterator == bounds_.end() ? top : iterator->second;
    }

    void set(Variable variable, Interval interval)
    {
        if (!environment_.contains(variable))
            throw std::invalid_argument("variable is outside the environment");
        if (interval.isTop())
            bounds_.erase(variable.id());
        else
            bounds_.insert_or_assign(variable.id(), std::move(interval));
    }

    void changeEnvironment(const VariableEnvironment& nextEnvironment,
                           bool initializeNewVariablesToZero = false)
    {
        if (environment_ == nextEnvironment)
            return;
        for (const VariableDeclaration& item : nextEnvironment.variables())
        {
            if (environment_.contains(item.variable) &&
                environment_.typeOf(item.variable) != item.type)
                throw std::invalid_argument(
                    "environment change modifies a variable type");
        }
        for (auto iterator = bounds_.begin(); iterator != bounds_.end();)
        {
            if (!nextEnvironment.contains(Variable(iterator->first)))
                iterator = bounds_.erase(iterator);
            else
                ++iterator;
        }
        if (initializeNewVariablesToZero)
        {
            for (const VariableDeclaration& item : nextEnvironment.variables())
            {
                if (!environment_.contains(item.variable))
                    bounds_.insert_or_assign(item.variable.id(),
                                             Interval::singleton(Rational()));
            }
        }
        environment_ = nextEnvironment;
    }

    std::size_t storageUnits() const
    {
        return bounds_.bucket_count();
    }

    std::size_t materializedValues() const
    {
        return bounds_.size();
    }

    std::size_t allocatedPageSlots() const
    {
        return 0;
    }

private:
    VariableEnvironment environment_;
    std::unordered_map<std::uint32_t, Interval> bounds_;
};

/// Conventional whole-container COW applied to the legacy-style hash layout.
/// Copies are O(1), but the first mutation must still clone every hash entry;
/// this distinguishes container COW from fine-grained persistent sharing.
class VariableCOWHashStorage
{
public:
    explicit VariableCOWHashStorage(VariableEnvironment environment)
        : environment_(std::move(environment)),
          bounds_(std::make_shared<Bounds>())
    {
    }

    const VariableEnvironment& environment() const
    {
        return environment_;
    }

    const Interval& bound(Variable variable) const
    {
        static const Interval top = Interval::top();
        if (!environment_.contains(variable))
            throw std::invalid_argument("variable is outside the environment");
        const auto iterator = bounds_->find(variable.id());
        return iterator == bounds_->end() ? top : iterator->second;
    }

    void set(Variable variable, Interval interval)
    {
        if (!environment_.contains(variable))
            throw std::invalid_argument("variable is outside the environment");
        ensureUnique();
        if (interval.isTop())
            bounds_->erase(variable.id());
        else
            bounds_->insert_or_assign(variable.id(), std::move(interval));
    }

    void changeEnvironment(const VariableEnvironment& nextEnvironment,
                           bool initializeNewVariablesToZero = false)
    {
        if (environment_ == nextEnvironment)
            return;
        for (const VariableDeclaration& item : nextEnvironment.variables())
        {
            if (environment_.contains(item.variable) &&
                environment_.typeOf(item.variable) != item.type)
                throw std::invalid_argument(
                    "environment change modifies a variable type");
        }
        ensureUnique();
        for (auto iterator = bounds_->begin(); iterator != bounds_->end();)
        {
            if (!nextEnvironment.contains(Variable(iterator->first)))
                iterator = bounds_->erase(iterator);
            else
                ++iterator;
        }
        if (initializeNewVariablesToZero)
        {
            for (const VariableDeclaration& item : nextEnvironment.variables())
            {
                if (!environment_.contains(item.variable))
                    bounds_->insert_or_assign(item.variable.id(),
                                              Interval::singleton(Rational()));
            }
        }
        environment_ = nextEnvironment;
    }

    std::size_t storageUnits() const
    {
        return bounds_->bucket_count();
    }
    std::size_t materializedValues() const
    {
        return bounds_->size();
    }
    std::size_t allocatedPageSlots() const
    {
        return 0;
    }

private:
    using Bounds = std::unordered_map<std::uint32_t, Interval>;

    void ensureUnique()
    {
        if (bounds_.use_count() != 1)
            bounds_ = std::make_shared<Bounds>(*bounds_);
    }

    VariableEnvironment environment_;
    std::shared_ptr<Bounds> bounds_;
};

/// Whole-directory COW layered over a page-level-COW storage. State copies
/// share the compact page directory; the first mutation copies only directory
/// entries, after which the underlying storage detaches the changed page.
/// Unlike VariableCOWHashStorage, unchanged interval payloads remain shared.
template <typename Storage> class COWDirectoryStorage
{
public:
    explicit COWDirectoryStorage(VariableEnvironment environment)
        : storage_(std::make_shared<Storage>(std::move(environment)))
    {
    }

    const VariableEnvironment& environment() const
    {
        return storage_->environment();
    }

    const Interval& bound(Variable variable) const
    {
        return storage_->bound(variable);
    }

    void set(Variable variable, Interval interval)
    {
        ensureUnique();
        storage_->set(variable, std::move(interval));
    }

    void changeEnvironment(const VariableEnvironment& environment,
                           bool initializeNewVariablesToZero = false)
    {
        if (storage_->environment() == environment)
            return;
        ensureUnique();
        storage_->changeEnvironment(environment,
                                    initializeNewVariablesToZero);
    }

    std::size_t storageUnits() const
    {
        return storage_->storageUnits();
    }
    std::size_t materializedValues() const
    {
        return storage_->materializedValues();
    }
    std::size_t allocatedPageSlots() const
    {
        return storage_->allocatedPageSlots();
    }

private:
    void ensureUnique()
    {
        if (storage_.use_count() != 1)
            storage_ = std::make_shared<Storage>(*storage_);
    }

    std::shared_ptr<Storage> storage_;
};

template <typename Storage> void validateStorage()
{
    const Variable a(1);
    const Variable b(9);
    const Variable c(65);
    const Variable d(257);
    const VariableEnvironment first(
        {declaration(a), declaration(b), declaration(c)});
    const VariableEnvironment second(
        {declaration(a), declaration(c), declaration(d)});
    Storage original(first);
    original.set(a, valueFor(a));
    original.set(b, valueFor(b));
    original.set(c, valueFor(c));
    Storage copy = original;
    copy.set(a, valueFor(a, 17));
    if (original.bound(a) == copy.bound(a))
        throw std::logic_error("copy-on-write isolation failed");
    copy.changeEnvironment(second);
    if (copy.bound(c) != valueFor(c) || !copy.bound(d).isTop())
        throw std::logic_error("environment remap lost a shared bound");
    copy.changeEnvironment(first);
    if (!copy.bound(b).isTop())
        throw std::logic_error("removed variable resurrected after re-add");
    copy.changeEnvironment(second, true);
    if (copy.bound(d) != Interval::singleton(Rational()))
        throw std::logic_error("zero initialization failed");
}

std::size_t automaticOperations(const Options& options, std::size_t activeCount)
{
    if (options.operations != 0)
        return options.operations;
    if (options.workload == "read")
        return 500000;
    if (options.workload == "copy-only")
        return std::clamp<std::size_t>(500000 / activeCount, 16, 5000);
    if (options.workload == "copy-update")
        return std::clamp<std::size_t>(500000 / activeCount, 16, 5000);
    if (options.workload == "environment-change")
        return std::clamp<std::size_t>(100000 / activeCount, 5, 500);
    if (options.workload == "extend-restore")
        return std::clamp<std::size_t>(50000 / activeCount, 5, 500);
    if (options.workload == "resident-fork")
        return 512;
    if (options.workload == "forget-reinsert")
        return std::clamp<std::size_t>(500000 / activeCount, 16, 5000);
    if (options.workload == "join-scan")
        return std::clamp<std::size_t>(250000 / activeCount, 8, 1000);
    if (options.workload == "subset-scan")
        return std::clamp<std::size_t>(1000000 / activeCount, 16, 5000);
    throw std::invalid_argument("unknown workload: " + options.workload);
}

template <typename Storage>
void run(const Options& options, const char* schemeName)
{
    validateStorage<Storage>();
    const EnvironmentPair environments =
        makeEnvironments(options.variables, options.idStride);
    const std::vector<Variable> active =
        activeVariables(environments.first, options.density);
    Storage base(environments.first);
    for (Variable variable : active)
        base.set(variable, valueFor(variable));
    Storage alternative = base;
    for (std::size_t index = 0; index < active.size(); index += 2)
        alternative.set(active[index], valueFor(active[index], 17));

    std::vector<Variable> readableCommon;
    for (Variable variable : environments.common)
    {
        if (!base.bound(variable).isTop())
            readableCommon.push_back(variable);
    }
    if (readableCommon.empty())
        readableCommon.push_back(environments.common.front());

    const std::size_t operations = automaticOperations(options, active.size());
    std::vector<double> samples;
    samples.reserve(options.repetitions);
    std::uint64_t checksum = 0;

    // Run one unrecorded warmup before the odd-numbered measured sample set.
    for (std::size_t repetition = 0; repetition <= options.repetitions;
         ++repetition)
    {
        const auto start = Clock::now();
        std::int64_t localChecksum = 0;
        if (options.workload == "read")
        {
            for (std::size_t operation = 0; operation < operations; ++operation)
            {
                const Variable variable =
                    active[mix(operation) % active.size()];
                localChecksum += finiteValue(base.bound(variable));
            }
        }
        else if (options.workload == "copy-only")
        {
            for (std::size_t operation = 0; operation < operations; ++operation)
            {
                Storage copy = base;
                const Variable variable =
                    active[mix(operation) % active.size()];
                localChecksum += finiteValue(copy.bound(variable));
            }
        }
        else if (options.workload == "copy-update")
        {
            for (std::size_t operation = 0; operation < operations; ++operation)
            {
                Storage copy = base;
                const Variable variable =
                    active[mix(operation) % active.size()];
                copy.set(variable, valueFor(variable, operation + 1));
                localChecksum += finiteValue(copy.bound(variable));
            }
        }
        else if (options.workload == "environment-change")
        {
            for (std::size_t operation = 0; operation < operations; ++operation)
            {
                Storage copy = base;
                copy.changeEnvironment(environments.second);
                const Variable variable =
                    readableCommon[mix(operation) % readableCommon.size()];
                localChecksum += finiteValue(copy.bound(variable));
            }
        }
        else if (options.workload == "extend-restore")
        {
            for (std::size_t operation = 0; operation < operations; ++operation)
            {
                Storage copy = base;
                copy.changeEnvironment(environments.extended);
                const Variable temporary =
                    environments.temporaries[mix(operation) %
                                             environments.temporaries.size()];
                copy.set(temporary, valueFor(temporary, operation + 1));
                localChecksum += finiteValue(copy.bound(temporary));
                copy.changeEnvironment(environments.first);
            }
        }
        else if (options.workload == "resident-fork")
        {
            std::vector<Storage> residents;
            residents.reserve(operations);
            for (std::size_t operation = 0; operation < operations; ++operation)
            {
                residents.push_back(base);
                const Variable variable =
                    active[mix(operation) % active.size()];
                residents.back().set(variable,
                                     valueFor(variable, operation + 1));
                localChecksum += finiteValue(residents.back().bound(variable));
            }
        }
        else if (options.workload == "forget-reinsert")
        {
            for (std::size_t operation = 0; operation < operations; ++operation)
            {
                Storage copy = base;
                const Variable variable =
                    active[mix(operation) % active.size()];
                copy.set(variable, Interval::top());
                copy.set(variable, valueFor(variable, operation + 1));
                localChecksum += finiteValue(copy.bound(variable));
            }
        }
        else if (options.workload == "join-scan")
        {
            for (std::size_t operation = 0; operation < operations; ++operation)
            {
                Storage joined = base;
                for (Variable variable : active)
                {
                    joined.set(variable,
                               intervalHull(base.bound(variable),
                                            alternative.bound(variable)));
                }
                const Variable variable =
                    active[mix(operation) % active.size()];
                const Interval& result = joined.bound(variable);
                if (result.lower().isFinite())
                    localChecksum += result.lower().value().value()
                                         .get_num().get_si();
            }
        }
        else if (options.workload == "subset-scan")
        {
            for (std::size_t operation = 0; operation < operations; ++operation)
            {
                bool subset = true;
                for (Variable variable : active)
                {
                    const bool itemSubset =
                        intervalSubset(base.bound(variable),
                                       alternative.bound(variable));
                    subset = subset & itemSubset;
                }
                localChecksum += subset ? 1 : 0;
            }
        }
        else
        {
            throw std::invalid_argument("unknown workload: " +
                                        options.workload);
        }
        const auto finish = Clock::now();
        if (repetition != 0)
        {
            samples.push_back(
                std::chrono::duration<double, std::nano>(finish - start)
                    .count() /
                static_cast<double>(operations));
            checksum ^=
                mix(static_cast<std::uint64_t>(localChecksum) + repetition);
        }
    }

    std::sort(samples.begin(), samples.end());
    const double median = samples[samples.size() / 2];
    const std::size_t p95Index = std::min(
        samples.size() - 1,
        static_cast<std::size_t>(std::ceil(samples.size() * 0.95)) - 1);
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0)
        throw std::runtime_error("getrusage failed");
#ifdef __APPLE__
    constexpr const char* rssUnit = "bytes";
#else
    constexpr const char* rssUnit = "kibibytes";
#endif
    const std::string scheme(schemeName);
    const std::size_t reportedPageSize =
        scheme == "variable-hash" || scheme == "variable-cow-hash" ||
                scheme == "variable-radix-cow"
            ? 0
            : options.pageSize;
    const std::size_t materialized = base.materializedValues();
    const std::size_t allocatedSlots = base.allocatedPageSlots();
    const double pageUtilization =
        allocatedSlots == 0 ? 0.0
                            : static_cast<double>(materialized) /
                                  static_cast<double>(allocatedSlots);
    std::cout << schemeName << ',' << reportedPageSize << ','
              << options.workload << ',' << options.variables << ','
              << active.size() << ',' << std::setprecision(6) << options.density
              << ',' << options.idStride << ',' << operations << ','
              << options.repetitions << ',' << std::fixed
              << std::setprecision(3) << median << ',' << samples[p95Index]
              << ',' << environments.first.size() << ',' << materialized << ','
              << base.storageUnits() << ',' << allocatedSlots << ','
              << std::setprecision(9) << pageUtilization << ',' << checksum
              << ',' << usage.ru_maxrss << ',' << rssUnit << '\n';
}

template <bool VariableKeyed>
void dispatchPageSize(const Options& options, const char* schemeName)
{
    switch (options.pageSize)
    {
    case 8:
        run<PagedStorage<8, VariableKeyed>>(options, schemeName);
        return;
    case 16:
        run<PagedStorage<16, VariableKeyed>>(options, schemeName);
        return;
    case 32:
        run<PagedStorage<32, VariableKeyed>>(options, schemeName);
        return;
    case 64:
        run<PagedStorage<64, VariableKeyed>>(options, schemeName);
        return;
    case 128:
        run<PagedStorage<128, VariableKeyed>>(options, schemeName);
        return;
    case 256:
        run<PagedStorage<256, VariableKeyed>>(options, schemeName);
        return;
    default:
        throw std::invalid_argument(
            "page size must be one of 8,16,32,64,128,256");
    }
}

template <bool VariableKeyed>
void dispatchCOWPageSize(const Options& options, const char* schemeName)
{
    switch (options.pageSize)
    {
    case 8:
        run<COWDirectoryStorage<PagedStorage<8, VariableKeyed>>>(options,
                                                                 schemeName);
        return;
    case 16:
        run<COWDirectoryStorage<PagedStorage<16, VariableKeyed>>>(options,
                                                                  schemeName);
        return;
    case 32:
        run<COWDirectoryStorage<PagedStorage<32, VariableKeyed>>>(options,
                                                                  schemeName);
        return;
    case 64:
        run<COWDirectoryStorage<PagedStorage<64, VariableKeyed>>>(options,
                                                                  schemeName);
        return;
    case 128:
        run<COWDirectoryStorage<PagedStorage<128, VariableKeyed>>>(options,
                                                                   schemeName);
        return;
    case 256:
        run<COWDirectoryStorage<PagedStorage<256, VariableKeyed>>>(options,
                                                                   schemeName);
        return;
    default:
        throw std::invalid_argument(
            "page size must be one of 8,16,32,64,128,256");
    }
}

template <bool VariableKeyed>
void dispatchHashPageSize(const Options& options, const char* schemeName)
{
    switch (options.pageSize)
    {
    case 8:
        run<HashPagedStorage<8, VariableKeyed>>(options, schemeName);
        return;
    case 16:
        run<HashPagedStorage<16, VariableKeyed>>(options, schemeName);
        return;
    case 32:
        run<HashPagedStorage<32, VariableKeyed>>(options, schemeName);
        return;
    case 64:
        run<HashPagedStorage<64, VariableKeyed>>(options, schemeName);
        return;
    case 128:
        run<HashPagedStorage<128, VariableKeyed>>(options, schemeName);
        return;
    case 256:
        run<HashPagedStorage<256, VariableKeyed>>(options, schemeName);
        return;
    default:
        throw std::invalid_argument(
            "page size must be one of 8,16,32,64,128,256");
    }
}

template <bool VariableKeyed>
void dispatchCOWHashPageSize(const Options& options, const char* schemeName)
{
    switch (options.pageSize)
    {
    case 8:
        run<COWDirectoryStorage<HashPagedStorage<8, VariableKeyed>>>(
            options, schemeName);
        return;
    case 16:
        run<COWDirectoryStorage<HashPagedStorage<16, VariableKeyed>>>(
            options, schemeName);
        return;
    case 32:
        run<COWDirectoryStorage<HashPagedStorage<32, VariableKeyed>>>(
            options, schemeName);
        return;
    case 64:
        run<COWDirectoryStorage<HashPagedStorage<64, VariableKeyed>>>(
            options, schemeName);
        return;
    case 128:
        run<COWDirectoryStorage<HashPagedStorage<128, VariableKeyed>>>(
            options, schemeName);
        return;
    case 256:
        run<COWDirectoryStorage<HashPagedStorage<256, VariableKeyed>>>(
            options, schemeName);
        return;
    default:
        throw std::invalid_argument(
            "page size must be one of 8,16,32,64,128,256");
    }
}

template <bool VariableKeyed>
void dispatchRadixPageSize(const Options& options, const char* schemeName)
{
    switch (options.pageSize)
    {
    case 8:
        run<RadixPagedStorage<8, VariableKeyed>>(options, schemeName);
        return;
    case 16:
        run<RadixPagedStorage<16, VariableKeyed>>(options, schemeName);
        return;
    case 32:
        run<RadixPagedStorage<32, VariableKeyed>>(options, schemeName);
        return;
    case 64:
        run<RadixPagedStorage<64, VariableKeyed>>(options, schemeName);
        return;
    case 128:
        run<RadixPagedStorage<128, VariableKeyed>>(options, schemeName);
        return;
    case 256:
        run<RadixPagedStorage<256, VariableKeyed>>(options, schemeName);
        return;
    default:
        throw std::invalid_argument(
            "page size must be one of 8,16,32,64,128,256");
    }
}

std::size_t parseSize(const char* value, const char* option)
{
    const unsigned long long parsed = std::stoull(value);
    if (parsed > std::numeric_limits<std::size_t>::max())
        throw std::out_of_range(std::string(option) + " is too large");
    return static_cast<std::size_t>(parsed);
}

Options parseOptions(int argc, char** argv)
{
    Options result;
    for (int index = 1; index < argc; ++index)
    {
        const std::string option = argv[index];
        if (option == "--header")
        {
            std::cout << "scheme,page_size,workload,variables,active,density,"
                         "id_stride,operations,repetitions,median_ns_per_op,"
                         "p95_ns_per_op,logical_dimensions,"
                         "materialized_values,storage_units,"
                         "allocated_page_slots,page_utilization,checksum,"
                         "peak_rss_raw,peak_rss_unit\n";
            std::exit(EXIT_SUCCESS);
        }
        if (index + 1 >= argc)
            throw std::invalid_argument("missing value for " + option);
        const char* value = argv[++index];
        if (option == "--scheme")
            result.scheme = value;
        else if (option == "--workload")
            result.workload = value;
        else if (option == "--page-size")
            result.pageSize = parseSize(value, "page size");
        else if (option == "--variables")
            result.variables = parseSize(value, "variables");
        else if (option == "--density")
            result.density = std::stod(value);
        else if (option == "--id-stride")
        {
            const std::size_t stride = parseSize(value, "id stride");
            if (stride > std::numeric_limits<std::uint32_t>::max())
                throw std::out_of_range("id stride exceeds uint32");
            result.idStride = static_cast<std::uint32_t>(stride);
        }
        else if (option == "--operations")
            result.operations = parseSize(value, "operations");
        else if (option == "--repetitions")
            result.repetitions = parseSize(value, "repetitions");
        else
            throw std::invalid_argument("unknown option: " + option);
    }
    if (result.repetitions < 3 || result.repetitions % 2 == 0)
        throw std::invalid_argument(
            "repetitions must be an odd number of at least three");
    return result;
}

} // namespace

int main(int argc, char** argv)
{
    try
    {
        const Options options = parseOptions(argc, argv);
        if (options.scheme == "dimension-page")
            dispatchPageSize<false>(options, "dimension-page");
        else if (options.scheme == "dimension-cow-page")
            dispatchCOWPageSize<false>(options, "dimension-cow-page");
        else if (options.scheme == "variable-page")
            dispatchPageSize<true>(options, "variable-page");
        else if (options.scheme == "dimension-hash-page")
            dispatchHashPageSize<false>(options, "dimension-hash-page");
        else if (options.scheme == "dimension-cow-hash-page")
            dispatchCOWHashPageSize<false>(options,
                                           "dimension-cow-hash-page");
        else if (options.scheme == "variable-hash-page")
            dispatchHashPageSize<true>(options, "variable-hash-page");
        else if (options.scheme == "dimension-radix-page")
            dispatchRadixPageSize<false>(options, "dimension-radix-page");
        else if (options.scheme == "variable-radix-page")
            dispatchRadixPageSize<true>(options, "variable-radix-page");
        else if (options.scheme == "variable-hash")
            run<VariableHashStorage>(options, "variable-hash");
        else if (options.scheme == "variable-cow-hash")
            run<VariableCOWHashStorage>(options, "variable-cow-hash");
        else if (options.scheme == "variable-radix-cow")
            run<VariableRadixCOWStorage>(options, "variable-radix-cow");
        else
            throw std::invalid_argument("unknown scheme: " + options.scheme);
        return EXIT_SUCCESS;
    }
    catch (const std::exception& error)
    {
        std::cerr << "Box storage benchmark failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
