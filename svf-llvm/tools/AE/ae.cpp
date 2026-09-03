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
#include "SVF-LLVM/SVFIRBuilder.h"
#include "Util/CommandLine.h"
#include "Util/Options.h"
#include "WPA/Andersen.h"

using namespace SVF;
using namespace SVFUtil;

int main(int argc, char** argv)
{
    constexpr int extraArgc = 3;
    int argNum = 0;
    char** arguments = new char*[argc + extraArgc];
    for (; argNum < argc; ++argNum)
        arguments[argNum] = argv[argNum];

    arguments[argNum++] = const_cast<char*>("-model-consts=true");
    arguments[argNum++] = const_cast<char*>("-model-arrays=true");
    arguments[argNum++] = const_cast<char*>("-pre-field-sensitive=false");
    assert(argNum == argc + extraArgc);

    const std::vector<std::string> modules =
        OptionBase::parseOptions(argNum, arguments, "Static Symbolic Execution",
                                 "[options] <input-bitcode...>");
    delete[] arguments;

    LLVMModuleSet::getLLVMModuleSet()->buildSVFModule(modules);
    SVFIRBuilder builder;
    SVFIR* pag = builder.build();
    AndersenWaveDiff* ander = AndersenWaveDiff::createAndersenWaveDiff(pag);
    builder.updateCallGraph(ander->getCallGraph());

    AbstractInterpretation& ae = AbstractInterpretation::getAEInstance();
    if (Options::BufferOverflowCheck())
        ae.addDetector(std::make_unique<BufOverflowDetector>());
    if (Options::NullDerefCheck())
        ae.addDetector(std::make_unique<NullptrDerefDetector>());
    ae.runOnModule();

    AndersenWaveDiff::releaseAndersenWaveDiff();
    LLVMModuleSet::releaseLLVMModuleSet();
    return 0;
}
