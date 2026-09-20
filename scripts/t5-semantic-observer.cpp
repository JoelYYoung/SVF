//===- t5-semantic-observer.cpp -- Canonical AE projection -------------//
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

#include <functional>
#include <llvm/IR/Instructions.h>
#include <map>
#include <set>
#include <stdexcept>
#include <string_view>
#include <vector>

using namespace SVF;
using namespace SVFUtil;

#if AUDIT_BOX
static void adapterContract(SVFIR& pag)
{
    const auto check = [](bool valid, const char* message)
    {
        if (!valid) throw std::runtime_error(message);
    };
    const auto addObject = [&]()
    {
        auto* object = const_cast<BaseObjVar*>(
                           pag.getBaseObjVar(pag.addDummyObjNode(SVFType::getSVFInt8Type())));
        object->setNumOfElements(4);
        object->setFieldSensitive();
        return object;
    };
    std::vector<BaseObjVar*> arrays;
    for (auto it = pag.begin(); it != pag.end(); ++it)
        if (auto* object = dyn_cast<StackObjVar>(it->second))
            if (!object->isPointer() && object->getType()->isArrayTy() &&
                    object->getFunction() && object->getFunction()->getName() == "main")
            {
                object->setFieldSensitive();
                arrays.push_back(object);
            }
    check(arrays.size() == 2, "contract requires two numerical array objects");
    const auto* a = arrays[0];
    const auto* b = arrays[1];
    const auto addField = [&](const BaseObjVar* base, APOffset offset)
    {
        const NodeID id = pag.getGepObjVar(base, offset);
        return pag.getGepObjVar(id);
    };
    const auto* a2 = addField(a, 2);
    addField(b, 1);
    const auto* a1 = addField(a, 1);
    SVFIRAdapter adapter(pag);
    const auto first = adapter.firstObjectContentVariable().id();
    std::uint32_t nextLocation = 1;
    std::set<std::uint32_t> ids;
    std::map<const ObjVar*, AbstractDomain::Variable> saved;
    for (bool pointers :
            {
                false, true
            })
        for (auto it = pag.begin(); it != pag.end(); ++it)
            if (const auto* object = dyn_cast<ObjVar>(it->second))
            {
                if (object->isPointer() != pointers) continue;
                const auto location = adapter.location(*object);
                const auto content = adapter.contentVariable(*object);
                check(location.id() == nextLocation++, "location order changed");
                check(&adapter.object(location) == object, "location reverse mapping");
                check(adapter.contentObject(content) == object, "content reverse mapping");
                check(adapter.memoryLayout().contentOf(location) == content, "memory layout mapping");
                check(content.id() >= first && ids.insert(content.id()).second, "content ID collision");
                saved.emplace(object, content);
            }
    check(!ids.empty() && *ids.rbegin() - first + 1 == ids.size(), "content ID gaps");
    for (auto it = pag.begin(); it != pag.end(); ++it)
        if (const auto* value = dyn_cast<ValVar>(it->second))
            if (adapter.contains(*value))
            {
                const auto variable = adapter.variable(*value);
                check(variable.id() < first && adapter.value(variable) == value, "scalar prefix changed");
            }
    if (std::string_view(SVFIRAdapter::contentLayout()) == "function-base")
    {
        const auto firstField = adapter.contentVariable(*a).id() + 1;
        check(std::set<std::uint32_t> {adapter.contentVariable(*a1).id(),
                                      adapter.contentVariable(*a2).id()} ==
              std::set<std::uint32_t> {firstField, firstField + 1},
              "interleaved base fields not grouped");
    }
    const auto oldLayout = adapter.memoryLayout();
    const auto* late = addObject();
    check(!adapter.contains(*late), "late object already registered");
    const auto lateContent = adapter.contentVariable(*late);
    const auto lateLocation = adapter.location(*late);
    check(lateContent.id() == *ids.rbegin() + 1, "late content did not append");
    check(lateLocation.id() == nextLocation, "late location did not append");
    check(adapter.contentObject(lateContent) == late &&
          adapter.memoryLayout().contentOf(lateLocation) == lateContent, "late reverse mapping");
    for (const auto& entry : saved)
    {
        check(adapter.contentVariable(*entry.first) == entry.second, "late registration renumbered content");
        check(oldLayout.contentOf(adapter.location(*entry.first)) == entry.second,
              "late registration changed an existing layout binding");
    }
    SVFUtil::outs() << "ADAPTER_CONTRACT passed " << SVFIRAdapter::contentLayout() << '\n';
}
#endif

int main(int argc, char** argv)
{
#ifdef SVF_BOX_PAGE_IDENTITY
    SVFUtil::outs() << "BOX_REPRESENTATION "
                    << SVF::AbstractDomain::BoxDomain::storageRepresentation() << '\n';
    SVFUtil::outs() << "BOX_CONTENT_LAYOUT " << SVFIRAdapter::contentLayout() << '\n';
#endif
    std::vector<char*> arguments(argv, argv + argc);
#if AUDIT_BOX
    const auto contract = std::find_if(arguments.begin(), arguments.end(), [](const char* value)
    {
        return std::string_view(value) == "--adapter-contract";
    });
    const bool checkAdapter = contract != arguments.end();
    if (checkAdapter) arguments.erase(contract);
#endif
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
    const auto addDefault = [&](std::string_view option, char* value)
    {
        if (!hasOption(option))
            arguments.push_back(value);
    };
    addDefault("-model-consts", const_cast<char*>("-model-consts=true"));
    addDefault("-model-arrays", const_cast<char*>("-model-arrays=true"));
    addDefault("-pre-field-sensitive",
               const_cast<char*>("-pre-field-sensitive=false"));

    const std::vector<std::string> modules =
        OptionBase::parseOptions(static_cast<int>(arguments.size()),
                                 arguments.data(), "Static Symbolic Execution",
                                 "[options] <input-bitcode...>");

    LLVMModuleSet::getLLVMModuleSet()->buildSVFModule(modules);
    SVFIRBuilder builder;
    SVFIR* pag = builder.build();
#if AUDIT_BOX
    if (checkAdapter)
    {
        adapterContract(*pag);
        LLVMModuleSet::releaseLLVMModuleSet();
        return 0;
    }
#endif
    AndersenWaveDiff* ander = AndersenWaveDiff::createAndersenWaveDiff(pag);
    builder.updateCallGraph(ander->getCallGraph());

    AbstractInterpretation& ae = AbstractInterpretation::getAEInstance();
    if (Options::BufferOverflowCheck())
        ae.addDetector(std::make_unique<BufOverflowDetector>());
    if (Options::NullDerefCheck())
        ae.addDetector(std::make_unique<NullptrDerefDetector>());
    ae.runOnModule();


    // Snapshot before getters: Original's sparse queries can create def-site states.
    std::vector<const ICFGNode*> observedNodes;
    for (auto it = pag->getICFG()->begin(); it != pag->getICFG()->end(); ++it)
        if (ae.hasAbsState(it->second)) observedNodes.push_back(it->second);
    auto* llvmModules = LLVMModuleSet::getLLVMModuleSet();
    const auto llvmIdentity = [&](const llvm::Value* value)
    {
        std::string key;
        llvm::raw_string_ostream stream(key);
        if (const auto* inst = llvm::dyn_cast<llvm::Instruction>(value))
        {
            unsigned blockIndex = 0, instructionIndex = 0;
            for (const auto& block : *inst->getFunction())
            {
                if (&block == inst->getParent()) break;
                ++blockIndex;
            }
            for (const auto& instruction : *inst->getParent())
            {
                if (&instruction == inst) break;
                ++instructionIndex;
            }
            stream << inst->getFunction()->getName() << ":bb" << blockIndex
                   << ":i" << instructionIndex;
            stream.flush();
            return key;
        }
        else if (const auto* arg = llvm::dyn_cast<llvm::Argument>(value))
            stream << arg->getParent()->getName() << ':';
        value->printAsOperand(stream, true);
        stream.flush();
        for (char& c : key) if (c == '\t' || c == '\n' || c == '\r') c = ' ';
        return key;
    };
    const auto valueIdentity = [&](const SVFValue* value)
    {
        if (llvmModules->hasLLVMValue(value))
            return llvmIdentity(llvmModules->getLLVMValue(value));
        // Only SVF's canonical synthetic values may lack LLVM provenance.
        return std::string("SVF:") + std::to_string(value->getId());
    };
    std::function<std::string(NodeID)> objectKey = [&](NodeID id) -> std::string
    {
        if (id == 0) return "null";
        const auto* object = pag->getSVFVar(id);
        if (const auto* field = SVFUtil::dyn_cast<GepObjVar>(object))
            return "G:" + objectKey(field->getBaseNode()) + ':' +
                   std::to_string(field->getConstantFieldIdx());
        return "B:" + valueIdentity(object);
    };
    const auto addAddressTarget = [&](NodeID id, bool& unknown,
                                      std::set<std::string>& targets)
    {
        if (id == 0)
        {
            targets.insert("null");
            return;
        }
        const auto* object = SVFUtil::dyn_cast<ObjVar>(pag->getSVFVar(id));
        const BaseObjVar* base = object
                                ? pag->getBaseObject(object->getId()) : nullptr;
        // BlackHole is SVF's summary for an unknown modeled object, not a
        // distinct concrete allocation. Canonicalize both implementations to
        // the same semantic component instead of comparing synthetic IDs.
        if (base && base->isBlackHoleObj())
        {
            unknown = true;
            return;
        }
        targets.insert(objectKey(id));
    };
    const auto nodeIdentity = [&](const ICFGNode* node)
    {
        std::string key = std::to_string(node->getNodeKind()) + ':';
        const ICFGNode* identity = node;
        if (const auto* ret = SVFUtil::dyn_cast<RetICFGNode>(node)) identity = ret->getCallICFGNode();
        if (llvmModules->hasLLVMValue(identity)) return key + valueIdentity(identity);
        if (node->getFun()) return key + node->getFun()->getName();
        return key + "global";
    };
    for (const auto* node : observedNodes)
    {
        // Global initialization is a separate query contract, not a program instruction.
        if (node->getId() == 0) continue;
        const std::string nodeKey = nodeIdentity(node);
        std::string anchor = nodeKey;
        // The first line identifies the ICFG node; appended statements can be unordered.
        anchor = anchor.substr(0, anchor.find('\n'));
        for (char& c : anchor) if (c == '\t' || c == '\r') c = ' ';
        SVFUtil::outs() << "REACH\t" << nodeKey << "\t" << anchor << '\n';
        std::map<NodeID, const SVFVar*> values;
        std::vector<const SVFVar*> memoryPointers;
        const auto add = [&](const SVFVar* value)
        {
            if (value) values.emplace(value->getId(), value);
        };
        for (const auto* stmt : node->getSVFStmts())
        {
            if (const auto* assign = SVFUtil::dyn_cast<AssignStmt>(stmt))
            {
                add(assign->getLHSVar());
                if (!SVFUtil::isa<AddrStmt>(stmt)) add(assign->getRHSVar());
            }
            if (const auto* multi = SVFUtil::dyn_cast<MultiOpndStmt>(stmt))
            {
                add(multi->getRes());
                // CallPE contains arguments from all callers, including unreachable ones.
                if (!SVFUtil::isa<CallPE>(stmt))
                    for (const auto* operand : multi->getOpndVars()) add(operand);
            }
            if (const auto* unary = SVFUtil::dyn_cast<UnaryOPStmt>(stmt))
            { add(unary->getRes()); add(unary->getOpVar()); }
            if (const auto* branch = SVFUtil::dyn_cast<BranchStmt>(stmt)) add(branch->getCondition());
            if (const auto* select = SVFUtil::dyn_cast<SelectStmt>(stmt)) add(select->getCondition());
            if (const auto* load = SVFUtil::dyn_cast<LoadStmt>(stmt)) memoryPointers.push_back(load->getRHSVar());
            if (const auto* store = SVFUtil::dyn_cast<StoreStmt>(stmt)) memoryPointers.push_back(store->getLHSVar());
        }
        for (const auto* pointer : memoryPointers)
            for (NodeID id : ander->getPts(pointer->getId()))
                add(SVFUtil::dyn_cast<ObjVar>(pag->getSVFVar(id)));
        for (const auto& entry : values)
        {
            const auto* value = entry.second;
            const bool object = SVFUtil::isa<ObjVar>(value);
            // Some synthetic ObjVars discovered through Andersen's points-to
            // sets do not retain a usable type object.  Object cells are
            // queried independently of their LLVM type, so never dereference
            // the type pointer for them.
            const bool integer = !object &&
                SVFUtil::isa<SVFIntegerType>(value->getType());
            const std::string key = object ? objectKey(value->getId()) : valueIdentity(value);
            std::string identity = key;
            for (char& c : identity) if (c == '\n' || c == '\r' || c == '\t') c = ' ';
            SVFUtil::outs() << "IDENT\t" << nodeKey << '\t' << key << '\t' << identity << '\n';
            if (integer || object)
            {
#if AUDIT_BOX
                const auto interval = ae.getInterval(value, node);
#else
                const auto interval = ae.getAbsValue(value, node).getInterval();
#endif
                SVFUtil::outs() << "VALUE\t" << nodeKey << '\t' << key
                    << '\t' << (object ? "memory-numeric" : "integer") << '\t'
                    << (interval.isBottom() ? "BOTTOM" : interval.isTop() ? "TOP" : interval.toString()) << '\n';
            }
            if (value->isPointer() || object)
            {
                std::set<std::string> targets;
                bool unknown = false, raw = false;
#if AUDIT_BOX
                const auto addresses = ae.getAddressSet(value, node);
                unknown = addresses.hasUnknownObject(); raw = addresses.mayContainRawAddress();
                for (auto location : addresses)
                {
                    const auto* object = location.isNull()
                                         ? nullptr : ae.objectAt(location);
                    addAddressTarget(object ? object->getId() : 0,
                                     unknown, targets);
                }
#else
                const auto addresses = ae.getAbsValue(value, node).getAddrs();
                for (auto address : addresses)
                    addAddressTarget(address & FlippedAddressMask,
                                     unknown, targets);
#endif
                SVFUtil::outs() << "VALUE\t" << nodeKey << '\t' << key
                    << '\t' << (object ? "memory-address" : "pointer")
                    << "\tunknown=" << unknown << ";raw=" << raw << ";targets=";
                for (const auto& target : targets) SVFUtil::outs() << target << ',';
                SVFUtil::outs() << '\n';
            }
        }
    }
    // The large canonical projection is buffered.  Flush it before SVF
    // teardown so a teardown fault cannot leave a prefix that looks like a
    // valid semantic result.
    SVFUtil::outs().flush();
    AndersenWaveDiff::releaseAndersenWaveDiff();
    LLVMModuleSet::releaseLLVMModuleSet();
    return 0;
}
