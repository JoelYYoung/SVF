//===- DomainRegistry.h -- Registered abstract-domain kinds ----*- C++ -*-===//

#ifndef SVF_AE_DOMAIN_REGISTRY_H
#define SVF_AE_DOMAIN_REGISTRY_H

#include "AE/Core/AbstractDomain.h"

#include <optional>
#include <string_view>

namespace SVF::AbstractDomain
{

struct DomainDescriptor
{
    DomainKind kind;
    std::string_view name;
    bool numerical;
};

/// Closed registry for the domain implementations available in this build.
/// Analysis entities and program points are deliberately absent: those belong
/// to AnalysisSchema, not to domain identity.
class DomainRegistry final
{
public:
    static std::optional<DomainDescriptor> lookup(DomainKind kind) noexcept
    {
        switch (kind)
        {
        case DomainKind::Box:
            return DomainDescriptor{DomainKind::Box, "Box", true};
        case DomainKind::Address:
            return DomainDescriptor{DomainKind::Address, "Address", false};
        case DomainKind::Lifetime:
        case DomainKind::ValueKind:
        case DomainKind::Product:
            return std::nullopt;
        }
        return std::nullopt;
    }

    static bool isRegistered(DomainKind kind) noexcept
    {
        return lookup(kind).has_value();
    }
};

} // namespace SVF::AbstractDomain

#endif // SVF_AE_DOMAIN_REGISTRY_H
