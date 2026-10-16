//===- ae.cpp -- Abstract Execution -------------------------------------//
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
#include "SVF-LLVM/ImpactSourceVariables.h"
#include "SVF-LLVM/SVFIRBuilder.h"
#include "Util/CommandLine.h"
#include "Util/Options.h"
#include "WPA/Andersen.h"

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string_view>
#include <vector>

using namespace SVF;
using namespace SVFUtil;

int main(int argc, char** argv)
{
    std::vector<char*> arguments(argv, argv + argc);
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

    if (Options::AESparsity() == AbstractInterpretation::Sparse &&
            Options::AEDomain() != AbstractInterpretation::Box)
    {
        std::cerr << "ae: relational numerical domains support only "
                     "dense and semi-sparse modes\n";
        return 2;
    }

    LLVMModuleSet::getLLVMModuleSet()->buildSVFModule(modules);
    SVFIRBuilder builder;
    SVFIR* pag = builder.build();
    AndersenWaveDiff* ander = AndersenWaveDiff::createAndersenWaveDiff(pag);
    builder.updateCallGraph(ander->getCallGraph());

    registerImpactSourceVariables();

    AbstractInterpretation& ae = AbstractInterpretation::getAEInstance();
    if (Options::BufferOverflowCheck())
        ae.addDetector(std::make_unique<BufOverflowDetector>());
    if (Options::NullDerefCheck())
        ae.addDetector(std::make_unique<NullptrDerefDetector>());
    int status = 0;
    try
    {
        ae.runOnModule();
    }
    catch (const std::exception& error)
    {
        SVFUtil::errs() << "ae: " << error.what() << '\n';
        status = 2;
    }

    // L4 names: SVF IDs are per-process allocation order, so map them to
    // LLVM semantic names in the same process. Runs after every timed phase.
    if (const char* namesPath = std::getenv("SVF_AE_L4_NAMES"); namesPath && status == 0)
    {
        const auto start = std::chrono::steady_clock::now();
        std::ofstream names(namesPath, std::ios::out | std::ios::trunc);
        const auto semanticName = [](const llvm::Value* value) {
            std::string text;
            llvm::raw_string_ostream stream(text);
            if (const auto* instruction = llvm::dyn_cast<llvm::Instruction>(value))
                stream << LLVMModuleSet::instructionSemanticSite(instruction);
            else if (const auto* argument = llvm::dyn_cast<llvm::Argument>(value))
                stream << "fn=" << argument->getParent()->getName() << ":arg=" << argument->getArgNo();
            else if (const auto* global = llvm::dyn_cast<llvm::GlobalValue>(value))
            {
                stream << "global=";
                global->printAsOperand(stream, false);
            }
            else if (llvm::isa<llvm::Constant>(value))
            {
                stream << "const=";
                value->printAsOperand(stream, true);
            }
            else
                stream << "other=";
            stream.flush();
            for (char& c : text)
                if (c == '\t' || c == '\n')
                    c = ' ';
            return text;
        };
        // Integer bit width lets L4 treat a full type range as no information.
        const auto bitWidth = [](const llvm::Type* type) {
            return type->isIntegerTy() ? std::to_string(type->getIntegerBitWidth()) : std::string("-");
        };
        LLVMModuleSet* set = LLVMModuleSet::getLLVMModuleSet();
        for (const auto& [value, id] : set->valSyms())
            if (value)
                names << "v\t" << id << '\t' << bitWidth(value->getType()) << '\t'
                      << semanticName(value) << '\n';
        for (const auto& [value, id] : set->objSyms())
            if (value)
                names << "o\t" << id << "\t-\t" << semanticName(value) << '\n';
        names.close();
        SVFUtil::outs() << "AE_L4_NAMES_SECONDS "
                        << std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count()
                        << '\n';
        SVFUtil::outs().flush();
    }

    AndersenWaveDiff::releaseAndersenWaveDiff();
    LLVMModuleSet::releaseLLVMModuleSet();
    return status;
}
