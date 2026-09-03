//===- AbstractDomain.h -- Common abstract-property lattice API --*- C++
//-*-===//

#ifndef SVF_AE_ABSTRACT_DOMAIN_H
#define SVF_AE_ABSTRACT_DOMAIN_H

#include <memory>
#include <string>

namespace SVF::AbstractDomain
{

enum class CheckResult
{
    False,
    True,
    Unknown
};

enum class DomainKind
{
    Box,
    Address,
    Lifetime,
    Product
};

const char* toString(CheckResult result);

/// Common interface implemented by every self-contained abstract property.
///
/// It deliberately contains only lattice operations. Transfer functions live
/// on more specific interfaces such as NumericalDomain and AddressDomain.
class AbstractDomain
{
public:
    virtual ~AbstractDomain();

    virtual DomainKind kind() const noexcept = 0;
    virtual std::unique_ptr<AbstractDomain> clone() const = 0;

    void joinWith(const AbstractDomain& other);
    void meetWith(const AbstractDomain& other);
    void widenWith(const AbstractDomain& next);
    void narrowWith(const AbstractDomain& next);

    bool isBottom() const;
    bool isTop() const;
    /// Return whether every concrete state represented by this state is also
    /// represented by `other`.
    CheckResult isSubsetOf(const AbstractDomain& other) const;
    CheckResult isEquivalentTo(const AbstractDomain& other) const;
    std::string toString() const;

    /// RTTI-free concrete-state query. SVF is commonly built with -fno-rtti,
    /// so abstract domains use stable per-C++-type tokens for checked dispatch.
    template <typename DomainT> bool isDomain() const noexcept
    {
        return dynamicTypeToken() == staticTypeToken<DomainT>();
    }

protected:
    AbstractDomain() = default;
    AbstractDomain(const AbstractDomain&) = default;
    AbstractDomain(AbstractDomain&&) noexcept = default;
    AbstractDomain& operator=(const AbstractDomain&) = default;
    AbstractDomain& operator=(AbstractDomain&&) noexcept = default;

    void requireCompatible(const AbstractDomain& other) const;

    template <typename StateT> static const void* staticTypeToken() noexcept
    {
        static const char token = 0;
        return &token;
    }

private:
    virtual const void* dynamicTypeToken() const noexcept = 0;
    virtual bool hasCompatibleDomain(const AbstractDomain& other) const = 0;
    virtual void joinDomain(const AbstractDomain& other) = 0;
    virtual void meetDomain(const AbstractDomain& other) = 0;
    virtual void widenDomain(const AbstractDomain& next) = 0;
    virtual void narrowDomain(const AbstractDomain& next) = 0;
    virtual bool isBottomDomain() const = 0;
    virtual bool isTopDomain() const = 0;
    virtual bool leqDomain(const AbstractDomain& other) const = 0;
    virtual std::string domainToString() const = 0;
};

} // namespace SVF::AbstractDomain

#endif // SVF_AE_ABSTRACT_DOMAIN_H
