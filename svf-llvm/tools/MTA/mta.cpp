//===- mta.cpp --Program Analysis for Multithreaded Programs------------------//
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

#include "SVF-LLVM/LLVMUtil.h"
#include "SVF-LLVM/SVFIRBuilder.h"
#include "MTA/MTA.h"
#include "MTA/SlicedMTA.h"
#include "Util/CommandLine.h"
#include "Util/Options.h"

#include <iostream>
#include <string>
#include <vector>

using namespace llvm;
using namespace std;
using namespace SVF;

int main(int argc, char** argv)
{
    // Progress lines must reach the terminal (or a driver pipe) as they are
    // produced, not when the stream buffer fills.
    std::cout << std::unitbuf;

    std::vector<std::string> moduleNameVec = OptionBase::parseOptions(
                argc, argv, "MTA Analysis", "[options] <input-bitcode...>");

    SVFUtil::outs() << "[MTA] Loading LLVM bitcode and building the module...\n";
    LLVMModuleSet::buildSVFModule(moduleNameVec);
    SVFUtil::outs() << "[MTA] Building the SVFIR (PAG + ICFG)...\n";
    SVFIRBuilder builder;
    SVFIR* pag = builder.build();
    SVFUtil::outs() << "[MTA] SVFIR ready; starting the analysis\n";

    // MTA's only client is race detection. -flow-sensitive (default) selects the
    // MSli pipeline (ILA + FSPTA), which decides slicing and pre-analysis context
    // handling internally; otherwise run the flow-insensitive Andersen detector.
    if (Options::FlowSensitive())
    {
        // The only LLVM-dependent step -- materialising resolved indirect calls
        // into the PAG -- is injected here.
        SlicedMTA sliced;
        sliced.runOnModule(pag, [&](CallGraph* cg)
        {
            builder.updateCallGraph(cg);
        });
    }
    else
    {
        MTA mta;
        mta.runOnModule(pag);
    }

    LLVMModuleSet::releaseLLVMModuleSet();
    return 0;
}
