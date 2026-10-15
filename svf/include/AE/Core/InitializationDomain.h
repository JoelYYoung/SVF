//===- InitializationDomain.h -- Initialization facts ---------*- C++ -*-===//
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
// Contributors: Jiawei Yang
//
//===----------------------------------------------------------------------===//

#ifndef SVF_AE_INITIALIZATION_DOMAIN_H
#define SVF_AE_INITIALIZATION_DOMAIN_H

#include "AE/Core/AbstractDomain.h"
#include "AE/Core/Variable.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace SVF::AbstractDomain
{

/// The powerset of {uninitialized, initialized}. This describes initialization
/// only: Initialized does not imply a precise numerical or pointer value.
enum class InitializationState : unsigned char
{
    Bottom = 0,
    Uninitialized = 1,
    Initialized = 2,
    Top = 3
};

/// Independent, total initialization property. Omitted variables have the
/// explicit defaultState(), not a storage-dependent interpretation. A uniform
/// default permits both sparse unknown states and sparse fresh-frame states.
/// This domain does not decide how an interpreter handles an uninitialized read.
class InitializationDomain final : public AbstractDomain
{
public:
    static InitializationDomain top();
    static InitializationDomain bottom();
    static InitializationDomain uniform(InitializationState state);

    DomainKind kind() const noexcept override
    {
        return DomainKind::Initialization;
    }
    std::unique_ptr<AbstractDomain> clone() const override;

    InitializationState value(Variable variable) const;
    InitializationState defaultState() const;
    void assign(Variable variable, InitializationState state);
    void forget(Variable variable);
    std::vector<Variable> nonDefaultVariables() const;
    std::vector<Variable> nonDefaultVariablesBefore(Variable upperBound) const;

private:
    static constexpr unsigned ValuesPerPage = 64;

    /// Two bit planes encode non-default states. A zero pair is an omitted
    /// slot, not coordinate Bottom (which collapses the entire domain).
    struct Page
    {
        std::uint64_t uninitialized = 0;
        std::uint64_t initialized = 0;
    };
    struct PageEntry
    {
        // The ID is a page number. Type remains part of variable identity.
        Variable key;
        std::shared_ptr<Page> page;
    };
    using Directory = std::vector<PageEntry>;
    explicit InitializationDomain(InitializationState state);
    static std::shared_ptr<Directory> emptyPages();
    static Page expanded(const Page* page, InitializationState defaultState);
    static Page exceptions(Page values, InitializationState defaultState);
    std::vector<Variable> nonDefaultVariables(const Variable* upperBound) const;
    const void* dynamicTypeToken() const noexcept override
    {
        return staticTypeToken<InitializationDomain>();
    }
    bool hasCompatibleDomain(const AbstractDomain& other) const override;
    void joinDomain(const AbstractDomain& other) override;
    void meetDomain(const AbstractDomain& other) override;
    void widenDomain(const AbstractDomain& next) override;
    void narrowDomain(const AbstractDomain& next) override;
    bool isBottomDomain() const override;
    bool isTopDomain() const override;
    bool leqDomain(const AbstractDomain& other) const override;
    std::string domainToString() const override;
    void combine(const InitializationDomain& other, bool join);

    InitializationState default_;
    // Copies share the directory and pages. A first write detaches the small
    // directory; only the modified page's bits are copied, not all facts.
    std::shared_ptr<Directory> pages_;
};

} // namespace SVF::AbstractDomain

#endif // SVF_AE_INITIALIZATION_DOMAIN_H
