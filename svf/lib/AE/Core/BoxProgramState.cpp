//===- BoxProgramState.cpp -- Complete Box AE state -----------------===//

#include "AE/Core/BoxProgramState.h"

#include <algorithm>
#include <sstream>

namespace SVF::AbstractDomain
{

namespace
{

template <typename Key, typename Value>
std::set<Key> combinedKeys(const std::map<Key, Value>& lhs,
                           const std::map<Key, Value>& rhs)
{
    std::set<Key> keys;
    for (const auto& entry : lhs)
        keys.insert(entry.first);
    for (const auto& entry : rhs)
        keys.insert(entry.first);
    return keys;
}

Lifetime joinLifetime(Lifetime lhs, Lifetime rhs)
{
    if (lhs == Lifetime::Bottom)
        return rhs;
    if (rhs == Lifetime::Bottom || lhs == rhs)
        return lhs;
    return Lifetime::MaybeFreed;
}

Lifetime meetLifetime(Lifetime lhs, Lifetime rhs)
{
    if (lhs == Lifetime::MaybeFreed)
        return rhs;
    if (rhs == Lifetime::MaybeFreed || lhs == rhs)
        return lhs;
    return Lifetime::Bottom;
}

bool lifetimeIsSubsetOf(Lifetime lhs, Lifetime rhs)
{
    return lhs == Lifetime::Bottom || rhs == Lifetime::MaybeFreed || lhs == rhs;
}

const char* lifetimeToString(Lifetime lifetime)
{
    switch (lifetime)
    {
    case Lifetime::Bottom:
        return "bottom";
    case Lifetime::Alive:
        return "alive";
    case Lifetime::Freed:
        return "freed";
    case Lifetime::MaybeFreed:
        return "maybe-freed";
    }
    return "invalid";
}

} // namespace

LifetimeDomain LifetimeDomain::top()
{
    return LifetimeDomain(Lifetime::MaybeFreed);
}

LifetimeDomain LifetimeDomain::bottom()
{
    return LifetimeDomain(Lifetime::Bottom);
}

ValueKindDomain ValueKindDomain::top()
{
    return ValueKindDomain(true, true);
}

ValueKindDomain ValueKindDomain::bottom()
{
    return ValueKindDomain(false, false);
}

std::unique_ptr<AbstractDomain> ValueKindDomain::clone() const
{
    return std::make_unique<ValueKindDomain>(*this);
}

ValueKindDomain::Shape ValueKindDomain::shapeOf(Variable variable) const
{
    return decode(encodedShapeOf(variable));
}

bool ValueKindDomain::isDefined(Variable variable) const
{
    return shapeOf(variable).defined;
}

bool ValueKindDomain::hasNumeric(Variable variable) const
{
    return shapeOf(variable).numeric;
}

std::vector<Variable> ValueKindDomain::definedVariables(
    const VariableEnvironment& environment) const
{
    std::vector<Variable> result;
    if (decode(default_).defined)
    {
        result.reserve(environment.size());
        for (const VariableDeclaration& declaration : environment.variables())
        {
            if (isDefined(declaration.variable))
                result.push_back(declaration.variable);
        }
        return result;
    }

    for (const ShapePageEntry& entry : pages_)
    {
        for (std::size_t offset = 0; offset < ShapesPerPage; ++offset)
        {
            if (!decode(entry.page->shapes[offset]).defined)
                continue;
            const Variable variable(static_cast<std::uint32_t>(
                entry.index * ShapesPerPage + offset));
            if (environment.contains(variable))
                result.push_back(variable);
        }
    }
    return result;
}

void ValueKindDomain::assign(Variable variable, bool numeric)
{
    setEncodedShape(variable, encode({true, numeric}));
}

void ValueKindDomain::forget(Variable variable)
{
    setEncodedShape(variable, encode({false, false}));
}

void ValueKindDomain::changeEnvironment(const VariableEnvironment& environment)
{
    std::vector<ShapePageEntry> next;
    next.reserve(pages_.size());
    for (const ShapePageEntry& entry : pages_)
    {
        std::shared_ptr<ShapePage> page = entry.page;
        bool changed = false;
        for (std::size_t offset = 0; offset < ShapesPerPage; ++offset)
        {
            if (page->shapes[offset] == default_)
                continue;
            const Variable variable(static_cast<std::uint32_t>(
                entry.index * ShapesPerPage + offset));
            if (!environment.contains(variable))
            {
                if (!changed)
                    page = std::make_shared<ShapePage>(*page);
                page->shapes[offset] = default_;
                changed = true;
            }
        }
        if (!pageIsDefault(*page, default_))
            next.push_back({entry.index, std::move(page)});
    }
    pages_ = std::move(next);
}

bool ValueKindDomain::hasCompatibleDomain(const AbstractDomain& other) const
{
    return other.isDomain<ValueKindDomain>();
}

void ValueKindDomain::joinDomain(const AbstractDomain& other)
{
    const auto& state = static_cast<const ValueKindDomain&>(other);
    if (state.isBottomDomain())
        return;
    if (isBottomDomain())
    {
        *this = state;
        return;
    }
    const std::uint8_t nextDefault = default_ | state.default_;
    std::vector<ShapePageEntry> next;
    next.reserve(pages_.size() + state.pages_.size());
    std::size_t lhs = 0;
    std::size_t rhs = 0;
    while (lhs < pages_.size() || rhs < state.pages_.size())
    {
        const std::size_t pageIndex =
            rhs == state.pages_.size() ||
                    (lhs < pages_.size() &&
                     pages_[lhs].index < state.pages_[rhs].index)
                ? pages_[lhs].index
                : state.pages_[rhs].index;
        const ShapePage* lhsPage =
            lhs < pages_.size() && pages_[lhs].index == pageIndex
                ? pages_[lhs++].page.get()
                : nullptr;
        const ShapePage* rhsPage =
            rhs < state.pages_.size() && state.pages_[rhs].index == pageIndex
                ? state.pages_[rhs++].page.get()
                : nullptr;
        auto page = std::make_shared<ShapePage>();
        for (std::size_t offset = 0; offset < ShapesPerPage; ++offset)
            page->shapes[offset] =
                (lhsPage ? lhsPage->shapes[offset] : default_) |
                (rhsPage ? rhsPage->shapes[offset] : state.default_);
        if (!pageIsDefault(*page, nextDefault))
            next.push_back({pageIndex, std::move(page)});
    }
    default_ = nextDefault;
    pages_ = std::move(next);
}

void ValueKindDomain::meetDomain(const AbstractDomain& other)
{
    const auto& state = static_cast<const ValueKindDomain&>(other);
    if (state.isTopDomain())
        return;
    if (isTopDomain())
    {
        *this = state;
        return;
    }
    const std::uint8_t nextDefault = default_ & state.default_;
    std::vector<ShapePageEntry> next;
    next.reserve(pages_.size() + state.pages_.size());
    std::size_t lhs = 0;
    std::size_t rhs = 0;
    while (lhs < pages_.size() || rhs < state.pages_.size())
    {
        const std::size_t pageIndex =
            rhs == state.pages_.size() ||
                    (lhs < pages_.size() &&
                     pages_[lhs].index < state.pages_[rhs].index)
                ? pages_[lhs].index
                : state.pages_[rhs].index;
        const ShapePage* lhsPage =
            lhs < pages_.size() && pages_[lhs].index == pageIndex
                ? pages_[lhs++].page.get()
                : nullptr;
        const ShapePage* rhsPage =
            rhs < state.pages_.size() && state.pages_[rhs].index == pageIndex
                ? state.pages_[rhs++].page.get()
                : nullptr;
        auto page = std::make_shared<ShapePage>();
        for (std::size_t offset = 0; offset < ShapesPerPage; ++offset)
            page->shapes[offset] =
                (lhsPage ? lhsPage->shapes[offset] : default_) &
                (rhsPage ? rhsPage->shapes[offset] : state.default_);
        if (!pageIsDefault(*page, nextDefault))
            next.push_back({pageIndex, std::move(page)});
    }
    default_ = nextDefault;
    pages_ = std::move(next);
}

void ValueKindDomain::widenDomain(const AbstractDomain& next)
{
    joinDomain(next);
}

void ValueKindDomain::narrowDomain(const AbstractDomain& next)
{
    meetDomain(next);
}

bool ValueKindDomain::isBottomDomain() const
{
    return default_ == encode({false, false}) && pages_.empty();
}

bool ValueKindDomain::isTopDomain() const
{
    return default_ == encode({true, true}) && pages_.empty();
}

bool ValueKindDomain::leqDomain(const AbstractDomain& other) const
{
    const auto& state = static_cast<const ValueKindDomain&>(other);
    if (default_ == state.default_ && pages_.size() == state.pages_.size())
    {
        bool equal = true;
        for (std::size_t index = 0; index < pages_.size(); ++index)
        {
            if (pages_[index].index != state.pages_[index].index ||
                (pages_[index].page != state.pages_[index].page &&
                 pages_[index].page->shapes !=
                     state.pages_[index].page->shapes))
            {
                equal = false;
                break;
            }
        }
        if (equal)
            return true;
    }
    if ((default_ & ~state.default_) != 0)
        return false;
    std::size_t lhs = 0;
    std::size_t rhs = 0;
    while (lhs < pages_.size() || rhs < state.pages_.size())
    {
        const std::size_t pageIndex =
            rhs == state.pages_.size() ||
                    (lhs < pages_.size() &&
                     pages_[lhs].index < state.pages_[rhs].index)
                ? pages_[lhs].index
                : state.pages_[rhs].index;
        const ShapePage* lhsPage =
            lhs < pages_.size() && pages_[lhs].index == pageIndex
                ? pages_[lhs++].page.get()
                : nullptr;
        const ShapePage* rhsPage =
            rhs < state.pages_.size() && state.pages_[rhs].index == pageIndex
                ? state.pages_[rhs++].page.get()
                : nullptr;
        for (std::size_t offset = 0; offset < ShapesPerPage; ++offset)
        {
            const std::uint8_t lhsShape =
                lhsPage ? lhsPage->shapes[offset] : default_;
            const std::uint8_t rhsShape =
                rhsPage ? rhsPage->shapes[offset] : state.default_;
            if ((lhsShape & ~rhsShape) != 0)
                return false;
        }
    }
    return true;
}

std::string ValueKindDomain::domainToString() const
{
    std::ostringstream output;
    const Shape defaultShape = decode(default_);
    output << "default=(defined=" << defaultShape.defined
           << ",numeric=" << defaultShape.numeric << ") {";
    bool first = true;
    for (const ShapePageEntry& entry : pages_)
    {
        for (std::size_t offset = 0; offset < ShapesPerPage; ++offset)
        {
            if (entry.page->shapes[offset] == default_)
                continue;
            if (!first)
                output << ", ";
            first = false;
            const Shape shape = decode(entry.page->shapes[offset]);
            output << entry.index * ShapesPerPage + offset
                   << "=(defined=" << shape.defined
                   << ",numeric=" << shape.numeric << ")";
        }
    }
    output << "}";
    return output.str();
}

std::uint8_t ValueKindDomain::encode(Shape shape)
{
    return static_cast<std::uint8_t>((shape.defined ? 1U : 0U) |
                                     (shape.numeric ? 2U : 0U));
}

ValueKindDomain::Shape ValueKindDomain::decode(std::uint8_t shape)
{
    return {(shape & 1U) != 0, (shape & 2U) != 0};
}

std::uint8_t ValueKindDomain::encodedShapeOf(Variable variable) const
{
    const std::size_t pageIndex = variable.id() / ShapesPerPage;
    const auto iterator =
        std::lower_bound(pages_.begin(), pages_.end(), pageIndex,
                         [](const ShapePageEntry& entry, std::size_t index) {
                             return entry.index < index;
                         });
    if (iterator == pages_.end() || iterator->index != pageIndex)
        return default_;
    return iterator->page->shapes[variable.id() % ShapesPerPage];
}

void ValueKindDomain::setEncodedShape(Variable variable, std::uint8_t shape)
{
    const std::size_t pageIndex = variable.id() / ShapesPerPage;
    auto iterator =
        std::lower_bound(pages_.begin(), pages_.end(), pageIndex,
                         [](const ShapePageEntry& entry, std::size_t index) {
                             return entry.index < index;
                         });
    if (iterator == pages_.end() || iterator->index != pageIndex)
    {
        if (shape == default_)
            return;
        auto page = std::make_shared<ShapePage>();
        page->shapes.fill(default_);
        iterator = pages_.insert(iterator, {pageIndex, std::move(page)});
    }
    else if (iterator->page.use_count() != 1)
    {
        iterator->page = std::make_shared<ShapePage>(*iterator->page);
    }
    iterator->page->shapes[variable.id() % ShapesPerPage] = shape;
    if (pageIsDefault(*iterator->page, default_))
        pages_.erase(iterator);
}

bool ValueKindDomain::pageIsDefault(const ShapePage& page,
                                    std::uint8_t defaultShape)
{
    return std::all_of(
        page.shapes.begin(), page.shapes.end(),
        [defaultShape](std::uint8_t shape) { return shape == defaultShape; });
}

std::unique_ptr<AbstractDomain> LifetimeDomain::clone() const
{
    return std::make_unique<LifetimeDomain>(*this);
}

Lifetime LifetimeDomain::statusOf(Location location) const
{
    const auto it = values_->find(location);
    return it == values_->end() ? defaultValue_ : it->second;
}

void LifetimeDomain::allocate(Location location)
{
    set(location, Lifetime::Alive);
}

void LifetimeDomain::release(Location location)
{
    const Lifetime current = statusOf(location);
    set(location, current == Lifetime::Alive || current == Lifetime::Freed
                      ? Lifetime::Freed
                      : Lifetime::MaybeFreed);
}

bool LifetimeDomain::mayBeFreed(Location location) const
{
    const Lifetime lifetime = statusOf(location);
    return lifetime == Lifetime::Freed || lifetime == Lifetime::MaybeFreed;
}

bool LifetimeDomain::mustBeFreed(Location location) const
{
    return statusOf(location) == Lifetime::Freed;
}

bool LifetimeDomain::hasCompatibleDomain(const AbstractDomain& other) const
{
    return other.isDomain<LifetimeDomain>();
}

void LifetimeDomain::joinDomain(const AbstractDomain& other)
{
    const auto& state = static_cast<const LifetimeDomain&>(other);
    if (state.isBottomDomain())
        return;
    if (isBottomDomain())
    {
        *this = state;
        return;
    }
    const std::set<Location> locations = combinedKeys(*values_, *state.values_);
    const Lifetime nextDefault =
        joinLifetime(defaultValue_, state.defaultValue_);
    std::map<Location, Lifetime> next;
    for (Location location : locations)
    {
        const Lifetime value =
            joinLifetime(statusOf(location), state.statusOf(location));
        if (value != nextDefault)
            next.emplace(location, value);
    }
    defaultValue_ = nextDefault;
    values_ = std::make_shared<Values>(std::move(next));
}

void LifetimeDomain::meetDomain(const AbstractDomain& other)
{
    const auto& state = static_cast<const LifetimeDomain&>(other);
    if (state.isTopDomain())
        return;
    if (isTopDomain())
    {
        *this = state;
        return;
    }
    const std::set<Location> locations = combinedKeys(*values_, *state.values_);
    const Lifetime nextDefault =
        meetLifetime(defaultValue_, state.defaultValue_);
    std::map<Location, Lifetime> next;
    for (Location location : locations)
    {
        const Lifetime value =
            meetLifetime(statusOf(location), state.statusOf(location));
        if (value != nextDefault)
            next.emplace(location, value);
    }
    defaultValue_ = nextDefault;
    values_ = std::make_shared<Values>(std::move(next));
}

void LifetimeDomain::widenDomain(const AbstractDomain& next)
{
    joinDomain(next);
}

void LifetimeDomain::narrowDomain(const AbstractDomain& next)
{
    meetDomain(next);
}

bool LifetimeDomain::isBottomDomain() const
{
    return defaultValue_ == Lifetime::Bottom && values_->empty();
}

bool LifetimeDomain::isTopDomain() const
{
    return defaultValue_ == Lifetime::MaybeFreed && values_->empty();
}

bool LifetimeDomain::leqDomain(const AbstractDomain& other) const
{
    const auto& state = static_cast<const LifetimeDomain&>(other);
    if (defaultValue_ == state.defaultValue_ &&
        (values_ == state.values_ || *values_ == *state.values_))
        return true;
    if (!lifetimeIsSubsetOf(defaultValue_, state.defaultValue_))
        return false;
    const std::set<Location> locations = combinedKeys(*values_, *state.values_);
    return std::all_of(locations.begin(), locations.end(),
                       [&](Location location) {
                           return lifetimeIsSubsetOf(
                               statusOf(location), state.statusOf(location));
                       });
}

std::string LifetimeDomain::domainToString() const
{
    std::ostringstream output;
    output << "default=" << lifetimeToString(defaultValue_) << " {";
    bool first = true;
    for (const auto& [location, value] : *values_)
    {
        if (!first)
            output << ", ";
        first = false;
        output << location.id() << "=" << lifetimeToString(value);
    }
    output << "}";
    return output.str();
}

void LifetimeDomain::set(Location location, Lifetime lifetime)
{
    if (lifetime == defaultValue_)
        writableValues().erase(location);
    else
        writableValues()[location] = lifetime;
}

LifetimeDomain::Values& LifetimeDomain::writableValues()
{
    if (values_.use_count() != 1)
        values_ = std::make_shared<Values>(*values_);
    return *values_;
}

Variable MemoryLayout::contentOf(Location location) const
{
    const auto it = cells_->find(location);
    if (it == cells_->end())
        throw std::out_of_range("location has no content symbol");
    return it->second;
}

} // namespace SVF::AbstractDomain
