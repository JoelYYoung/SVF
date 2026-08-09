//===- IntervalState.cpp ---- SVF interval/address program state -------//
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
 * IntervalState.cpp
 *
 *  Created on: Jul 9, 2022
 *      Author: Xiao Cheng, Jiawei Wang
 *
 */

#include <iomanip>
#include "AE/Core/IntervalState.h"
#include "SVFIR/SVFIR.h"
#include "Util/SVFUtil.h"
#include "Util/Options.h"

using namespace SVF;
using namespace SVFUtil;

bool IntervalState::equals(const IntervalState&other) const
{
    return *this == other;
}

u32_t IntervalState::hash() const
{
    size_t h = getVarToVal().size() * 2;
    Hash<u32_t> hf;
    for (const auto &t: getVarToVal())
    {
        h ^= hf(t.first) + 0x9e3779b9 + (h << 6) + (h >> 2);
    }
    size_t h2 = getLocToVal().size() * 2;
    for (const auto &t: getLocToVal())
    {
        h2 ^= hf(t.first) + 0x9e3779b9 + (h2 << 6) + (h2 >> 2);
    }
    Hash<std::pair<u32_t, u32_t>> pairH;
    size_t result = pairH({h, h2});
    return static_cast<u32_t>(result);
}

IntervalState IntervalState::widening(const IntervalState& other)
{
    // widen interval
    IntervalState es = *this;
    for (auto it = es._varToAbsVal.begin(); it != es._varToAbsVal.end(); ++it)
    {
        auto key = it->first;
        if (other._varToAbsVal.find(key) != other._varToAbsVal.end())
            if (it->second.isInterval() && other._varToAbsVal.at(key).isInterval())
                it->second.getInterval().widen_with(other._varToAbsVal.at(key).getInterval());
    }
    for (auto it = es._addrToAbsVal.begin(); it != es._addrToAbsVal.end(); ++it)
    {
        auto key = it->first;
        if (other._addrToAbsVal.find(key) != other._addrToAbsVal.end())
            if (it->second.isInterval() && other._addrToAbsVal.at(key).isInterval())
                it->second.getInterval().widen_with(other._addrToAbsVal.at(key).getInterval());
    }
    return es;
}

IntervalState IntervalState::narrowing(const IntervalState& other)
{
    IntervalState es = *this;
    for (auto it = es._varToAbsVal.begin(); it != es._varToAbsVal.end(); ++it)
    {
        auto key = it->first;
        if (other._varToAbsVal.find(key) != other._varToAbsVal.end())
            if (it->second.isInterval() && other._varToAbsVal.at(key).isInterval())
                it->second.getInterval().narrow_with(other._varToAbsVal.at(key).getInterval());
    }
    for (auto it = es._addrToAbsVal.begin(); it != es._addrToAbsVal.end(); ++it)
    {
        auto key = it->first;
        if (other._addrToAbsVal.find(key) != other._addrToAbsVal.end())
            if (it->second.isInterval() && other._addrToAbsVal.at(key).isInterval())
                it->second.getInterval().narrow_with(other._addrToAbsVal.at(key).getInterval());
    }
    return es;

}

/// domain join with other, important! other widen this.
void IntervalState::joinWith(const IntervalState& other)
{
    for (auto it = other._varToAbsVal.begin(); it != other._varToAbsVal.end(); ++it)
    {
        auto key = it->first;
        auto oit = _varToAbsVal.find(key);
        if (oit != _varToAbsVal.end())
        {
            oit->second.join_with(it->second);
        }
        else
        {
            _varToAbsVal.emplace(key, it->second);
        }
    }
    for (auto it = other._addrToAbsVal.begin(); it != other._addrToAbsVal.end(); ++it)
    {
        auto key = it->first;
        auto oit = _addrToAbsVal.find(key);
        if (oit != _addrToAbsVal.end())
        {
            oit->second.join_with(it->second);
        }
        else
        {
            _addrToAbsVal.emplace(key, it->second);
        }
    }
    _freedAddrs.insert(other._freedAddrs.begin(), other._freedAddrs.end());
}

void IntervalState::joinVariableValue(u32_t id,
                                      const AbstractValue& value)
{
    auto [it, inserted] = _varToAbsVal.emplace(id, value);
    if (!inserted)
        it->second.join_with(value);
}

void IntervalState::joinMemoryValue(u32_t objectId,
                                    const AbstractValue& value)
{
    auto [it, inserted] = _addrToAbsVal.emplace(objectId, value);
    if (!inserted)
        it->second.join_with(value);
}

void IntervalState::replaceMemoryFrom(const IntervalState& other)
{
    _addrToAbsVal = other._addrToAbsVal;
}

void IntervalState::joinFreedAddressesFrom(const IntervalState& other)
{
    _freedAddrs.insert(other._freedAddrs.begin(),
                       other._freedAddrs.end());
}

/// domain meet with other, important! other widen this.
void IntervalState::meetWith(const IntervalState& other)
{
    for (auto it = other._varToAbsVal.begin(); it != other._varToAbsVal.end(); ++it)
    {
        auto key = it->first;
        auto oit = _varToAbsVal.find(key);
        if (oit != _varToAbsVal.end())
        {
            oit->second.meet_with(it->second);
        }
    }
    for (auto it = other._addrToAbsVal.begin(); it != other._addrToAbsVal.end(); ++it)
    {
        auto key = it->first;
        auto oit = _addrToAbsVal.find(key);
        if (oit != _addrToAbsVal.end())
        {
            oit->second.meet_with(it->second);
        }
    }
    Set<NodeID> intersection;
    std::set_intersection(_freedAddrs.begin(), _freedAddrs.end(),
                          other._freedAddrs.begin(), other._freedAddrs.end(),
                          std::inserter(intersection, intersection.begin()));
    _freedAddrs = std::move(intersection);
}

// initObjVar
void IntervalState::initObjVar(const ObjVar* objVar)
{
    NodeID varId = objVar->getId();

    // Check if the object variable has an associated value

    const BaseObjVar* obj = PAG::getPAG()->getBaseObject(objVar->getId());

    // Handle constant data, arrays, and structures
    if (obj->isConstDataOrConstGlobal() || obj->isConstantArray() || obj->isConstantStruct())
    {
        if (const ConstIntObjVar* consInt = SVFUtil::dyn_cast<ConstIntObjVar>(objVar))
        {
            s64_t numeral = consInt->getSExtValue();
            (*this)[varId] = IntervalValue(numeral, numeral);
        }
        else if (const ConstFPObjVar* consFP = SVFUtil::dyn_cast<ConstFPObjVar>(objVar))
        {
            (*this)[varId] = IntervalValue(consFP->getFPValue(), consFP->getFPValue());
        }
        else if (SVFUtil::isa<ConstNullPtrObjVar>(objVar))
        {
            (*this)[varId] = IntervalValue(0, 0);
        }
        else if (SVFUtil::isa<GlobalObjVar>(objVar))
        {
            (*this)[varId] = AddressValue(IntervalState::getVirtualMemAddress(varId));
        }
        else if (obj->isConstantArray() || obj->isConstantStruct())
        {
            (*this)[varId] = IntervalValue::top();
        }
        else
        {
            (*this)[varId] = IntervalValue::top();
        }
    }
    // Handle non-constant memory objects
    else
    {
        (*this)[varId] = AddressValue(IntervalState::getVirtualMemAddress(varId));
    }
    return;
}

void IntervalState::printAbstractState() const
{
    SVFUtil::outs() << "-----------Var and Value-----------\n";
    u32_t fieldWidth = 20;
    SVFUtil::outs().flags(std::ios::left);
    std::vector<std::pair<u32_t, AbstractValue>> varToAbsValVec(_varToAbsVal.begin(), _varToAbsVal.end());
    std::sort(varToAbsValVec.begin(), varToAbsValVec.end(), [](const auto &a, const auto &b)
    {
        return a.first < b.first;
    });
    for (const auto &item: varToAbsValVec)
    {
        SVFUtil::outs() << std::left << std::setw(fieldWidth) << ("Var" + std::to_string(item.first));
        if (item.second.isInterval())
        {
            SVFUtil::outs() << " Value: " << item.second.getInterval().toString() << "\n";
        }
        else if (item.second.isAddr())
        {
            SVFUtil::outs() << " Value: {";
            u32_t i = 0;
            for (const auto& addr: item.second.getAddrs())
            {
                ++i;
                if (i < item.second.getAddrs().size())
                {
                    SVFUtil::outs() << "0x" << std::hex << addr << ", ";
                }
                else
                {
                    SVFUtil::outs() << "0x" << std::hex << addr;
                }
            }
            SVFUtil::outs() << "}\n";
        }
        else
        {
            SVFUtil::outs() << " Value: ⊥\n";
        }
    }

    std::vector<std::pair<u32_t, AbstractValue>> addrToAbsValVec(_addrToAbsVal.begin(), _addrToAbsVal.end());
    std::sort(addrToAbsValVec.begin(), addrToAbsValVec.end(), [](const auto &a, const auto &b)
    {
        return a.first < b.first;
    });

    for (const auto& item: addrToAbsValVec)
    {
        std::ostringstream oss;
        oss << "0x" << std::hex << IntervalState::getVirtualMemAddress(item.first);
        SVFUtil::outs() << std::left << std::setw(fieldWidth) << oss.str();
        if (item.second.isInterval())
        {
            SVFUtil::outs() << " Value: " << item.second.getInterval().toString() << "\n";
        }
        else if (item.second.isAddr())
        {
            SVFUtil::outs() << " Value: {";
            u32_t i = 0;
            for (const auto& addr: item.second.getAddrs())
            {
                ++i;
                if (i < item.second.getAddrs().size())
                {
                    SVFUtil::outs() << "0x" << std::hex << addr << ", ";
                }
                else
                {
                    SVFUtil::outs() << "0x" << std::hex << addr;
                }
            }
            SVFUtil::outs() << "}\n";
        }
        else
        {
            SVFUtil::outs() << " Value: ⊥\n";
        }
    }
    SVFUtil::outs() << "-----------------------------------------\n";
}

std::string IntervalState::toString() const
{
    u32_t varIntervals = 0, varAddrs = 0, varBottom = 0;
    for (const auto& item : _varToAbsVal)
    {
        if (item.second.isInterval()) ++varIntervals;
        else if (item.second.isAddr()) ++varAddrs;
        else ++varBottom;
    }
    u32_t addrIntervals = 0, addrAddrs = 0, addrBottom = 0;
    for (const auto& item : _addrToAbsVal)
    {
        if (item.second.isInterval()) ++addrIntervals;
        else if (item.second.isAddr()) ++addrAddrs;
        else ++addrBottom;
    }
    std::ostringstream oss;
    oss << "IntervalState {\n"
        << "  VarToAbsVal: " << _varToAbsVal.size() << " entries ("
        << varIntervals << " intervals, " << varAddrs << " addresses, " << varBottom << " bottom)\n"
        << "  AddrToAbsVal: " << _addrToAbsVal.size() << " entries ("
        << addrIntervals << " intervals, " << addrAddrs << " addresses, " << addrBottom << " bottom)\n"
        << "  FreedAddrs: " << _freedAddrs.size() << "\n";
    oss << "}";
    return oss.str();
}


bool IntervalState::eqVarToValMap(const VarToAbsValMap&lhs, const VarToAbsValMap&rhs) const
{
    if (lhs.size() != rhs.size()) return false;
    for (const auto &item: lhs)
    {
        auto it = rhs.find(item.first);
        if (it == rhs.end())
            return false;
        if (!item.second.equals(it->second))
            return false;
    }
    return true;
}

bool IntervalState::geqVarToValMap(const VarToAbsValMap&lhs, const VarToAbsValMap&rhs) const
{
    if (rhs.empty()) return true;
    for (const auto &item: rhs)
    {
        auto it = lhs.find(item.first);
        if (it == lhs.end()) return false;
        if (!it->second.getInterval().contain(item.second.getInterval()))
            return false;
        for (u32_t address : item.second.getAddrs())
            if (!it->second.getAddrs().contains(address))
                return false;
    }
    return true;
}

bool IntervalState::hasCompatibleDomain(
    const AbstractDomain::AbstractState& other) const
{
    return dynamic_cast<const IntervalState*>(&other) != nullptr;
}

void IntervalState::joinState(const AbstractDomain::AbstractState& other)
{
    joinWith(static_cast<const IntervalState&>(other));
}

void IntervalState::meetState(const AbstractDomain::AbstractState& other)
{
    meetWith(static_cast<const IntervalState&>(other));
}

void IntervalState::widenState(const AbstractDomain::AbstractState& next)
{
    *this = widening(static_cast<const IntervalState&>(next));
}

void IntervalState::narrowState(const AbstractDomain::AbstractState& next)
{
    *this = narrowing(static_cast<const IntervalState&>(next));
}

bool IntervalState::isBottomState() const
{
    // AE represents unreachable program points by absence from abstractTrace;
    // an allocated IntervalState therefore does not encode global bottom.
    return false;
}

bool IntervalState::isTopState() const
{
    if (!_varToAbsVal.empty() || !_addrToAbsVal.empty() ||
        !_freedAddrs.empty())
        return false;
    return true;
}

bool IntervalState::leqState(
    const AbstractDomain::AbstractState& other) const
{
    return static_cast<const IntervalState&>(other) >= *this;
}

std::string IntervalState::stateToString() const
{
    return toString();
}
