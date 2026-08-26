//===- IntervalState.h ---- SVF interval/address program state ---------//
//
//                     SVF: Static Value-Flow Analysis
//
// Copyright (C) <2013-2022>  <Yulei Sui>
//

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Affero General Public License for more details.

// You should have received a copy of the GNU Affero General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
//===----------------------------------------------------------------------===//
/*
 * IntervalState.h
 *
 *  Created on: Jul 9, 2022
 *      Author: Xiao Cheng, Jiawei Wang
 *
 *                         [-oo,+oo]
 *          /           /            \           \
 *       [-oo,1] ... [-oo,10] ... [-1,+oo] ... [0,+oo]
 *          \           \           /          /
 *           \            [-1,10]            /
 *            \        /         \         /
 *       ...   [-1,1]      ...     [0,10]      ...
 *           \    |    \         /       \    /
 *       ...   [-1,0]    [0,1]    ...     [1,9]  ...
 *           \    |   \    |   \        /
 *       ...  [-1,-1]  [0,0]     [1,1]  ...
 *         \    \        \        /      /
 *                          ⊥
 */
// The implementation is based on
// Xiao Cheng, Jiawei Wang and Yulei Sui. Precise Sparse Abstract Execution via Cross-Domain Interaction.
// 46th International Conference on Software Engineering. (ICSE24)

#ifndef SVF_AE_INTERVAL_STATE_H
#define SVF_AE_INTERVAL_STATE_H

#include "AE/Core/AbstractState.h"
#include "AE/Core/AbstractValue.h"
#include "AE/Core/IntervalValue.h"
#include "Util/GeneralType.h"

#include <algorithm>

namespace SVF
{
class IntervalState final : public AbstractDomain::AbstractState
{
    friend class SVFIR2AbsState;
    friend class RelationSolver;
public:
    using VarToAbsValMap = Map<u32_t, AbstractValue>;
    using AddrToAbsValMap = VarToAbsValMap;

    IntervalState() = default;
    IntervalState(const IntervalState&) = default;
    IntervalState(IntervalState&&) = default;
    IntervalState& operator=(const IntervalState&) = default;
    IntervalState& operator=(IntervalState&&) = default;

    ~IntervalState() override = default;

    std::unique_ptr<AbstractDomain::AbstractState> clone() const override
    {
        return std::make_unique<IntervalState>(*this);
    }

    const char* name() const override
    {
        return "IntervalState";
    }

    // initObjVar
    void initObjVar(const ObjVar* objVar);


    /// The physical address starts with 0x7f...... + idx
    static u32_t getVirtualMemAddress(u32_t idx)
    {
        return AddressValue::getVirtualMemAddress(idx);
    }

    /// Check bit value of val start with 0x7F000000, filter by 0xFF000000
    static bool isVirtualMemAddress(u32_t val)
    {
        return AddressValue::isVirtualMemAddress(val);
    }

    /// Return the internal index if addr is an address otherwise return the value of idx
    u32_t getIDFromAddr(u32_t addr) const
    {
        return _freedAddrs.count(addr) ?  AddressValue::getInternalID(BlackHoleObjAddr) : AddressValue::getInternalID(addr);
    }

    /// Set all value bottom
    IntervalState bottom() const
    {
        IntervalState inv = *this;
        for (auto &item: inv._varToAbsVal)
        {
            if (item.second.isInterval())
                item.second.getInterval().set_to_bottom();
        }
        return inv;
    }

    /// Set all value top
    IntervalState top() const
    {
        IntervalState inv = *this;
        for (auto &item: inv._varToAbsVal)
        {
            if (item.second.isInterval())
                item.second.getInterval().set_to_top();
        }
        return inv;
    }

    /// Return a state containing only the requested variable values.
    IntervalState sliceState(const Set<u32_t>& variables) const
    {
        IntervalState result;
        for (u32_t id : variables)
        {
            const auto value = _varToAbsVal.find(id);
            if (value != _varToAbsVal.end())
                result._varToAbsVal.emplace(*value);
        }
        return result;
    }

    static bool isNullMem(u32_t addr)
    {
        return addr == NullMemAddr;
    }

    static bool isBlackHoleObjAddr(u32_t addr)
    {
        return addr == BlackHoleObjAddr;
    }


private:
    VarToAbsValMap _varToAbsVal; ///< Map a variable (symbol) to its abstract value
    AddrToAbsValMap _addrToAbsVal; ///< Map a memory address to its stored abstract value
    Set<NodeID> _freedAddrs;

public:
    /// get abstract value of variable
    AbstractValue& operator[](u32_t varId)
    {
        assert(!isVirtualMemAddress(varId) && "varId is a virtual memory address, use load() instead");
        return _varToAbsVal[varId];
    }

    /// get abstract value of variable
    const AbstractValue& operator[](u32_t varId) const
    {
        assert(!isVirtualMemAddress(varId) && "varId is a virtual memory address, use load() instead");
        return _varToAbsVal.at(varId);
    }

    AbstractValue& load(u32_t addr)
    {
        assert(isVirtualMemAddress(addr) && "not virtual address?");
        u32_t objId = getIDFromAddr(addr);
        return _addrToAbsVal[objId];
    }

    const AbstractValue& load(u32_t addr) const
    {
        assert(isVirtualMemAddress(addr) && "not virtual address?");
        u32_t objId = getIDFromAddr(addr);
        return _addrToAbsVal.at(objId);
    }

    void store(u32_t addr, const AbstractValue& val)
    {
        assert(isVirtualMemAddress(addr) && "not virtual address?");
        if (isNullMem(addr))
            return;
        const u32_t objId = getIDFromAddr(addr);
        _addrToAbsVal[objId] = val;
    }

    /// whether the variable is in varToAddrs table
    bool inVarToAddrsTable(u32_t id) const
    {
        const auto value = _varToAbsVal.find(id);
        return value != _varToAbsVal.end() && value->second.isAddr();
    }

    /// whether the variable is in varToVal table
    bool inVarToValTable(u32_t id) const
    {
        const auto value = _varToAbsVal.find(id);
        return value != _varToAbsVal.end() && value->second.isInterval();
    }

    /// whether the memory address stores memory addresses
    bool inAddrToAddrsTable(u32_t id) const
    {
        const auto value = _addrToAbsVal.find(id);
        return value != _addrToAbsVal.end() && value->second.isAddr();
    }

    /// whether the memory address stores abstract value
    bool inAddrToValTable(u32_t id) const
    {
        const auto value = _addrToAbsVal.find(id);
        return value != _addrToAbsVal.end() && value->second.isInterval();
    }

    /// get var2val map
    const VarToAbsValMap& getVarToVal() const
    {
        return _varToAbsVal;
    }

    /// get loc2val map
    const AddrToAbsValMap& getLocToVal() const
    {
        return _addrToAbsVal;
    }

    /// domain widen with other, and return the widened domain
    IntervalState widening(const IntervalState&other);

    /// domain narrow with other, and return the narrowed domain
    IntervalState narrowing(const IntervalState&other);

    /// Join another interval/address state into this state.
    void joinWith(const IntervalState&other);

    /// Fine-grained joins used by sparse engines.  Their names make the
    /// propagated state component explicit at each call site.
    void joinVariableValue(u32_t id, const AbstractValue& value);
    void joinMemoryValue(u32_t objectId, const AbstractValue& value);
    void replaceMemoryFrom(const IntervalState& other);
    void joinFreedAddressesFrom(const IntervalState& other);

    void replaceFreedAddressesFrom(const IntervalState& other)
    {
        _freedAddrs = other._freedAddrs;
    }

    /// Meet another interval/address state into this state.
    void meetWith(const IntervalState&other);

    void addToFreedAddrs(NodeID addr)
    {
        _freedAddrs.insert(addr);
    }

    const Set<NodeID>& getFreedAddrs() const
    {
        return _freedAddrs;
    }

    bool isFreedMem(u32_t addr) const
    {
        return _freedAddrs.count(addr) != 0;
    }


    void printAbstractState() const;

    std::string toString() const;

    // lhs == rhs for varToValMap
    bool eqVarToValMap(const VarToAbsValMap&lhs, const VarToAbsValMap&rhs) const;
    // lhs >= rhs for varToValMap
    bool geqVarToValMap(const VarToAbsValMap&lhs, const VarToAbsValMap&rhs) const;
    bool operator==(const IntervalState& rhs) const
    {
        return eqVarToValMap(_varToAbsVal, rhs.getVarToVal()) &&
               eqVarToValMap(_addrToAbsVal, rhs.getLocToVal()) &&
               _freedAddrs == rhs._freedAddrs;
    }

    bool operator!=(const IntervalState& rhs) const
    {
        return !(*this == rhs);
    }

    bool operator<(const IntervalState& rhs) const
    {
        return !(*this >= rhs);
    }

    bool operator>=(const IntervalState& rhs) const
    {
        return geqVarToValMap(_varToAbsVal, rhs.getVarToVal()) &&
               geqVarToValMap(_addrToAbsVal, rhs.getLocToVal()) &&
               std::all_of(rhs._freedAddrs.begin(), rhs._freedAddrs.end(),
                           [&](NodeID address)
                           { return _freedAddrs.count(address) != 0; });
    }

    void clear()
    {
        _addrToAbsVal.clear();
        _varToAbsVal.clear();
        _freedAddrs.clear();
    }

    /// Drop all top-level variables (ValVars), keeping ObjVar storage and
    /// freed addresses intact. Used when building a cycle snapshot so the
    /// ValVar set is controlled by the caller rather than whatever was
    /// cached at the seed node.
    void clearVariableValues()
    {
        _varToAbsVal.clear();
    }

private:
    const void* dynamicTypeToken() const noexcept override
    {
        return staticTypeToken<IntervalState>();
    }
    bool hasCompatibleDomain(
        const AbstractDomain::AbstractState& other) const override;
    void joinState(const AbstractDomain::AbstractState& other) override;
    void meetState(const AbstractDomain::AbstractState& other) override;
    void widenState(const AbstractDomain::AbstractState& next) override;
    void narrowState(const AbstractDomain::AbstractState& next) override;
    bool isBottomState() const override;
    bool isTopState() const override;
    bool leqState(const AbstractDomain::AbstractState& other) const override;
    std::string stateToString() const override;
};

} // namespace SVF


#endif // SVF_AE_INTERVAL_STATE_H
