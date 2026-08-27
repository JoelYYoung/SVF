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
    if (options.workload == "copy-update")
        return std::clamp<std::size_t>(500000 / activeCount, 16, 5000);
    if (options.workload == "environment-change")
        return std::clamp<std::size_t>(100000 / activeCount, 5, 500);
    if (options.workload == "extend-restore")
        return std::clamp<std::size_t>(50000 / activeCount, 5, 500);
    if (options.workload == "resident-fork")
        return 512;
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
    const std::size_t reportedPageSize =
        std::string(schemeName) == "variable-hash" ? 0 : options.pageSize;
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
        else if (options.scheme == "variable-page")
            dispatchPageSize<true>(options, "variable-page");
        else if (options.scheme == "variable-hash")
            run<VariableHashStorage>(options, "variable-hash");
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
