//===- ae.cpp -- Abstract Execution -------------------------------------//
//
//                     SVF: Static Value-Flow Analysis
//
// Copyright (C) <2013-2017>  <Yulei Sui>
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
//===-----------------------------------------------------------------------===//

/*
 // Abstract Execution
 //
 // Author: Jiawei Wang, Xiao Cheng, Jiawei Yang, Jiawei Ren, Yulei Sui
 */
#include "SVF-LLVM/SVFIRBuilder.h"
#include "Util/CommandLine.h"
#include "Util/Options.h"
#include "WPA/Andersen.h"

#include "AE/Svfexe/AbstractInterpretation.h"

using namespace SVF;
using namespace SVFUtil;


static Option<bool> AETEST(
    "aetest",
    "abstract execution basic function test",
    false
);

class AETest
{
public:
    AETest() = default;

    ~AETest() = default;

    void testBinaryOpStmt()
    {
        // test division /
        assert((IntegerIntervalProjection(4) / IntegerIntervalProjection::bottom()).equals(IntegerIntervalProjection::bottom()));
        assert((IntegerIntervalProjection::bottom() / IntegerIntervalProjection(2)).equals(IntegerIntervalProjection::bottom()));
        assert((IntegerIntervalProjection::top() / IntegerIntervalProjection(0)).equals(IntegerIntervalProjection::bottom()));
        assert((IntegerIntervalProjection(4) / IntegerIntervalProjection(2)).equals(IntegerIntervalProjection(2)));
        assert((IntegerIntervalProjection(3) / IntegerIntervalProjection(2)).equals(IntegerIntervalProjection(1))); //
        assert((IntegerIntervalProjection(-3) / IntegerIntervalProjection(2)).equals(IntegerIntervalProjection(-1))); //
        assert((IntegerIntervalProjection(1, 3) / IntegerIntervalProjection(2)).equals(IntegerIntervalProjection(0, 1))); //
        assert((IntegerIntervalProjection(2, 7) / IntegerIntervalProjection(2)).equals(IntegerIntervalProjection(1, 3))); //
        assert((IntegerIntervalProjection(-3, 3) / IntegerIntervalProjection(2)).equals(IntegerIntervalProjection(-1, 1)));
        assert((IntegerIntervalProjection(-3, IntegerIntervalProjection::plus_infinity()) / IntegerIntervalProjection(2)).equals(IntegerIntervalProjection(-1, IntegerIntervalProjection::plus_infinity())));
        assert((IntegerIntervalProjection(IntegerIntervalProjection::minus_infinity(), 3) / IntegerIntervalProjection(2)).equals(IntegerIntervalProjection(IntegerIntervalProjection::minus_infinity(), 1)));
        assert((IntegerIntervalProjection(1, 3) / IntegerIntervalProjection(1, 2)).equals(IntegerIntervalProjection(0, 3)));//
        assert((IntegerIntervalProjection(-3, 3) / IntegerIntervalProjection(1, 2)).equals(IntegerIntervalProjection(-3, 3)));
        assert((IntegerIntervalProjection(2, 7) / IntegerIntervalProjection(-2, 3)).equals(IntegerIntervalProjection(-7, 7))); //
        assert((IntegerIntervalProjection(-2, 7) / IntegerIntervalProjection(-2, 3)).equals(IntegerIntervalProjection(-7, 7))); //
        assert((IntegerIntervalProjection(IntegerIntervalProjection::minus_infinity(), 7) / IntegerIntervalProjection(-2, 3)).equals(IntegerIntervalProjection::top()));
        assert((IntegerIntervalProjection(-2, IntegerIntervalProjection::plus_infinity()) / IntegerIntervalProjection(-2, 3)).equals(IntegerIntervalProjection::top()));

        assert((IntegerIntervalProjection(-2, 7) / IntegerIntervalProjection(IntegerIntervalProjection::minus_infinity(), 3)).equals(IntegerIntervalProjection(-7, 7)));
        assert((IntegerIntervalProjection(-2, 7) / IntegerIntervalProjection(-2, IntegerIntervalProjection::plus_infinity())).equals(IntegerIntervalProjection(-7, 7)));
        assert((IntegerIntervalProjection(-6, -3) / IntegerIntervalProjection(3, 9)).equals(IntegerIntervalProjection(-2, 0)));
        assert((IntegerIntervalProjection(-6, 6) / IntegerIntervalProjection(3, 9)).equals(IntegerIntervalProjection(-2, 2)));

        // test remainder %
        assert((IntegerIntervalProjection(4) % IntegerIntervalProjection::bottom()).equals(IntegerIntervalProjection::bottom()));
        assert((IntegerIntervalProjection::bottom() % IntegerIntervalProjection(2)).equals(IntegerIntervalProjection::bottom()));
        assert((IntegerIntervalProjection::top() % IntegerIntervalProjection(0)).equals(IntegerIntervalProjection::top()));
        assert((IntegerIntervalProjection(4) % IntegerIntervalProjection(2)).equals(IntegerIntervalProjection(0)));
        assert((IntegerIntervalProjection(3) % IntegerIntervalProjection(2)).equals(IntegerIntervalProjection(1)));
        assert((IntegerIntervalProjection(-3) % IntegerIntervalProjection(2)).equals(IntegerIntervalProjection(-1)));
        assert((IntegerIntervalProjection(1, 3) % IntegerIntervalProjection(2)).equals(IntegerIntervalProjection(0, 1)));
        assert((IntegerIntervalProjection(2, 7) % IntegerIntervalProjection(2)).equals(IntegerIntervalProjection(0, 1)));
        assert((IntegerIntervalProjection(-3, 3) % IntegerIntervalProjection(2)).equals(IntegerIntervalProjection(-1, 1)));
        assert((IntegerIntervalProjection(-3, IntegerIntervalProjection::plus_infinity()) % IntegerIntervalProjection(2)).equals(IntegerIntervalProjection(-1, 1)));
        assert((IntegerIntervalProjection(IntegerIntervalProjection::minus_infinity(), 3) % IntegerIntervalProjection(2)).equals(IntegerIntervalProjection(-1, 1)));
        assert((IntegerIntervalProjection(1, 3) % IntegerIntervalProjection(1, 2)).equals(IntegerIntervalProjection(0, 1)));
        assert((IntegerIntervalProjection(-3, 3) % IntegerIntervalProjection(1, 2)).equals(IntegerIntervalProjection(-1, 1)));
        assert((IntegerIntervalProjection(2, 7) % IntegerIntervalProjection(-2, 3)).equals(IntegerIntervalProjection::top())); //
        assert((IntegerIntervalProjection(-2, 7) % IntegerIntervalProjection(-2, 3)).equals(IntegerIntervalProjection::top())); //
        assert((IntegerIntervalProjection(IntegerIntervalProjection::minus_infinity(), 7) % IntegerIntervalProjection(-2, 3)).equals(IntegerIntervalProjection::top()));
        assert((IntegerIntervalProjection(-2, IntegerIntervalProjection::plus_infinity()) % IntegerIntervalProjection(-2, 3)).equals(IntegerIntervalProjection::top()));
        assert((IntegerIntervalProjection(-2, 7) % IntegerIntervalProjection(IntegerIntervalProjection::minus_infinity(), 3)).equals(IntegerIntervalProjection::top()));
        assert((IntegerIntervalProjection(-2, 7) % IntegerIntervalProjection(-2, IntegerIntervalProjection::plus_infinity())).equals(IntegerIntervalProjection::top()));
        assert((IntegerIntervalProjection(-6, -3) % IntegerIntervalProjection(3, 9)).equals(IntegerIntervalProjection(-6, 0)));
        assert((IntegerIntervalProjection(-6, 6) % IntegerIntervalProjection(3, 9)).equals(IntegerIntervalProjection(-6, 6)));

        // shl  <<
        assert((IntegerIntervalProjection(IntegerIntervalProjection::plus_infinity()) << IntegerIntervalProjection(IntegerIntervalProjection::plus_infinity())).equals(IntegerIntervalProjection(IntegerIntervalProjection::top())));
        assert((IntegerIntervalProjection(IntegerIntervalProjection::plus_infinity()) << IntegerIntervalProjection(2, 2)).equals(IntegerIntervalProjection(IntegerIntervalProjection::plus_infinity())));
        assert((IntegerIntervalProjection(IntegerIntervalProjection::minus_infinity()) << IntegerIntervalProjection(IntegerIntervalProjection::plus_infinity())).equals(IntegerIntervalProjection(IntegerIntervalProjection::top())));
        assert((IntegerIntervalProjection(IntegerIntervalProjection::minus_infinity()) << IntegerIntervalProjection(2, 2)).equals(IntegerIntervalProjection(IntegerIntervalProjection::minus_infinity())));
        assert((IntegerIntervalProjection(2, 2) << IntegerIntervalProjection(IntegerIntervalProjection::plus_infinity())).equals(IntegerIntervalProjection(IntegerIntervalProjection::top())));
        assert((IntegerIntervalProjection(0, 0) << IntegerIntervalProjection(IntegerIntervalProjection::plus_infinity())).equals(IntegerIntervalProjection(0, 0)));
        assert((IntegerIntervalProjection(-2, -2) << IntegerIntervalProjection(IntegerIntervalProjection::plus_infinity())).equals(IntegerIntervalProjection(IntegerIntervalProjection::top())));
        assert((IntegerIntervalProjection(0, 0) << IntegerIntervalProjection(2, 2)).equals(IntegerIntervalProjection(0, 0)));
        assert((IntegerIntervalProjection(2, 2) << IntegerIntervalProjection(3, 3)).equals(IntegerIntervalProjection(16, 16)));
        assert((IntegerIntervalProjection(-2, -2) << IntegerIntervalProjection(3, 3)).equals(IntegerIntervalProjection(-16, -16)));

        assert((IntegerIntervalProjection(4) << IntegerIntervalProjection::bottom()).equals(IntegerIntervalProjection::bottom()));
        assert((IntegerIntervalProjection::bottom() << IntegerIntervalProjection(2)).equals(IntegerIntervalProjection::bottom()));
        assert((IntegerIntervalProjection::top() << IntegerIntervalProjection(0)).equals(IntegerIntervalProjection::top()));
        assert((IntegerIntervalProjection(4) << IntegerIntervalProjection(2)).equals(IntegerIntervalProjection(16)));
        assert((IntegerIntervalProjection(3) << IntegerIntervalProjection(2)).equals(IntegerIntervalProjection(12)));
        assert((IntegerIntervalProjection(-3) << IntegerIntervalProjection(2)).equals(IntegerIntervalProjection(-12)));
        assert((IntegerIntervalProjection(4) << IntegerIntervalProjection(-2)).equals(IntegerIntervalProjection::bottom()));
        assert((IntegerIntervalProjection(1, 3) << IntegerIntervalProjection(2)).equals(IntegerIntervalProjection(4, 12)));
        assert((IntegerIntervalProjection(2, 7) << IntegerIntervalProjection(2)).equals(IntegerIntervalProjection(8, 28)));
        assert((IntegerIntervalProjection(-3, 3) << IntegerIntervalProjection(2)).equals(IntegerIntervalProjection(-12, 12)));
        assert((IntegerIntervalProjection(-3, IntegerIntervalProjection::plus_infinity()) << IntegerIntervalProjection(2)).equals(IntegerIntervalProjection(-12, IntegerIntervalProjection::plus_infinity())));
        assert((IntegerIntervalProjection(IntegerIntervalProjection::minus_infinity(), 3) << IntegerIntervalProjection(2)).equals(IntegerIntervalProjection(IntegerIntervalProjection::minus_infinity(), 12)));
        assert((IntegerIntervalProjection(1, 3) << IntegerIntervalProjection(1, 2)).equals(IntegerIntervalProjection(2, 12)));
        assert((IntegerIntervalProjection(-3, 3) << IntegerIntervalProjection(1, 2)).equals(IntegerIntervalProjection(-12, 12)));
        assert((IntegerIntervalProjection(2, 7) << IntegerIntervalProjection(-2, 3)).equals(IntegerIntervalProjection(2, 56)));
        assert((IntegerIntervalProjection(-2, 7) << IntegerIntervalProjection(-2, 3)).equals(IntegerIntervalProjection(-16, 56)));
        assert((IntegerIntervalProjection(IntegerIntervalProjection::minus_infinity(), 7) << IntegerIntervalProjection(-2, 3)).equals(IntegerIntervalProjection(IntegerIntervalProjection::minus_infinity(), 56)));
        assert((IntegerIntervalProjection(-2, IntegerIntervalProjection::plus_infinity()) << IntegerIntervalProjection(-2, 3)).equals(IntegerIntervalProjection(-16, IntegerIntervalProjection::plus_infinity())));
        assert((IntegerIntervalProjection(-2, 7) << IntegerIntervalProjection(IntegerIntervalProjection::minus_infinity(), 3)).equals(IntegerIntervalProjection(-16, 56)));
        assert((IntegerIntervalProjection(-2, 7) << IntegerIntervalProjection(-2, IntegerIntervalProjection::plus_infinity())).equals(IntegerIntervalProjection::top()));
        assert((IntegerIntervalProjection(-6, -3) << IntegerIntervalProjection(3, 9)).equals(IntegerIntervalProjection(-3072, -24)));
        assert((IntegerIntervalProjection(-6, 6) << IntegerIntervalProjection(3, 9)).equals(IntegerIntervalProjection(-3072, 3072)));
        assert((IntegerIntervalProjection(-2, 7) << IntegerIntervalProjection(IntegerIntervalProjection::minus_infinity(), -1)).equals(IntegerIntervalProjection::bottom()));
        assert((IntegerIntervalProjection(0) << IntegerIntervalProjection::top()).equals(IntegerIntervalProjection(0)));


        // shr >>
        assert((IntegerIntervalProjection(IntegerIntervalProjection::plus_infinity()) >> IntegerIntervalProjection(IntegerIntervalProjection::plus_infinity())).equals(IntegerIntervalProjection(IntegerIntervalProjection::plus_infinity())));
        assert((IntegerIntervalProjection(IntegerIntervalProjection::plus_infinity()) >> IntegerIntervalProjection(2)).equals(IntegerIntervalProjection(IntegerIntervalProjection::plus_infinity())));
        assert((IntegerIntervalProjection(IntegerIntervalProjection::minus_infinity()) >> IntegerIntervalProjection(IntegerIntervalProjection::plus_infinity())).equals(IntegerIntervalProjection(IntegerIntervalProjection::minus_infinity())));
        assert((IntegerIntervalProjection(IntegerIntervalProjection::minus_infinity()) >> IntegerIntervalProjection(2)).equals(IntegerIntervalProjection(IntegerIntervalProjection::minus_infinity())));
        assert((IntegerIntervalProjection(2) >> IntegerIntervalProjection(IntegerIntervalProjection::plus_infinity())).equals(IntegerIntervalProjection(0)));
        assert((IntegerIntervalProjection(0) >> IntegerIntervalProjection(IntegerIntervalProjection::plus_infinity())).equals(IntegerIntervalProjection(0)));
        assert((IntegerIntervalProjection(-2) >> IntegerIntervalProjection(IntegerIntervalProjection::plus_infinity())).equals(IntegerIntervalProjection(-1)));
        assert((IntegerIntervalProjection(0) >> IntegerIntervalProjection(2)).equals(IntegerIntervalProjection(0)));
        assert((IntegerIntervalProjection(15) >> IntegerIntervalProjection(2)).equals(IntegerIntervalProjection(3)));
        assert((IntegerIntervalProjection(-15) >> IntegerIntervalProjection(2)).equals(IntegerIntervalProjection(-4)));

        assert((IntegerIntervalProjection(4) >> IntegerIntervalProjection::bottom()).equals(IntegerIntervalProjection::bottom()));
        assert((IntegerIntervalProjection::bottom() >> IntegerIntervalProjection(2)).equals(IntegerIntervalProjection::bottom()));
        assert((IntegerIntervalProjection::top() >> IntegerIntervalProjection(0)).equals(IntegerIntervalProjection::top()));
        assert((IntegerIntervalProjection(15) >> IntegerIntervalProjection(2)).equals(IntegerIntervalProjection(3)));
        assert((IntegerIntervalProjection(1) >> IntegerIntervalProjection(2)).equals(IntegerIntervalProjection(0)));
        assert((IntegerIntervalProjection(-15) >> IntegerIntervalProjection(2)).equals(IntegerIntervalProjection(-4)));
        assert((IntegerIntervalProjection(4) >> IntegerIntervalProjection(-2)).equals(IntegerIntervalProjection::bottom()));
        assert((IntegerIntervalProjection(1, 3) >> IntegerIntervalProjection(2)).equals(IntegerIntervalProjection(0)));
        assert((IntegerIntervalProjection(2, 7) >> IntegerIntervalProjection(2)).equals(IntegerIntervalProjection(0, 1)));
        assert((IntegerIntervalProjection(-15, 15) >> IntegerIntervalProjection(2)).equals(IntegerIntervalProjection(-4, 3)));
        assert((IntegerIntervalProjection(-15, IntegerIntervalProjection::plus_infinity()) >> IntegerIntervalProjection(2)).equals(IntegerIntervalProjection(-4, IntegerIntervalProjection::plus_infinity())));
        assert((IntegerIntervalProjection(IntegerIntervalProjection::minus_infinity(), 15) >> IntegerIntervalProjection(2)).equals(IntegerIntervalProjection(IntegerIntervalProjection::minus_infinity(), 3)));
        assert((IntegerIntervalProjection(0, 15) >> IntegerIntervalProjection(1, 2)).equals(IntegerIntervalProjection(0, 7)));
        assert((IntegerIntervalProjection(-17, 15) >> IntegerIntervalProjection(1, 2)).equals(IntegerIntervalProjection(-9, 7)));
        assert((IntegerIntervalProjection(2, 7) >> IntegerIntervalProjection(-2, 3)).equals(IntegerIntervalProjection(0, 7)));
        assert((IntegerIntervalProjection(-2, 7) >> IntegerIntervalProjection(-2, 3)).equals(IntegerIntervalProjection(-2, 7)));
        assert((IntegerIntervalProjection(IntegerIntervalProjection::minus_infinity(), 7) >> IntegerIntervalProjection(-2, 3)).equals(IntegerIntervalProjection(IntegerIntervalProjection::minus_infinity(), 7)));
        assert((IntegerIntervalProjection(-2, IntegerIntervalProjection::plus_infinity()) >> IntegerIntervalProjection(-2, 3)).equals(IntegerIntervalProjection(-2, IntegerIntervalProjection::plus_infinity())));
        assert((IntegerIntervalProjection(-2, 7) >> IntegerIntervalProjection(IntegerIntervalProjection::minus_infinity(), 3)).equals(IntegerIntervalProjection(-2, 7)));
        assert((IntegerIntervalProjection(-2, 7) >> IntegerIntervalProjection(-2, IntegerIntervalProjection::plus_infinity())).equals(IntegerIntervalProjection(-2, 7)));
        assert((IntegerIntervalProjection(-6, -3) >> IntegerIntervalProjection(2, 3)).equals(IntegerIntervalProjection(-2, -1)));
        assert((IntegerIntervalProjection(-6, 6) >> IntegerIntervalProjection(2, 3)).equals(IntegerIntervalProjection(-2, 1)));
        assert((IntegerIntervalProjection(-2, 7) >> IntegerIntervalProjection(IntegerIntervalProjection::minus_infinity(), -1)).equals(IntegerIntervalProjection::bottom()));
        assert((IntegerIntervalProjection(0) >> IntegerIntervalProjection::top()).equals(IntegerIntervalProjection(0)));

        // and &
        assert((IntegerIntervalProjection(4) & IntegerIntervalProjection::bottom()).equals(IntegerIntervalProjection::bottom()));
        assert((IntegerIntervalProjection::bottom() & IntegerIntervalProjection(2)).equals(IntegerIntervalProjection::bottom()));
        assert((IntegerIntervalProjection::top() & IntegerIntervalProjection(0)).equals(IntegerIntervalProjection(0)));
        assert((IntegerIntervalProjection(4) & IntegerIntervalProjection(2)).equals(IntegerIntervalProjection(0)));
        assert((IntegerIntervalProjection(3) & IntegerIntervalProjection(2)).equals(IntegerIntervalProjection(2)));
        assert((IntegerIntervalProjection(-3) & IntegerIntervalProjection(2)).equals(IntegerIntervalProjection(0)));
        assert((IntegerIntervalProjection(1, 3) & IntegerIntervalProjection(2)).equals(IntegerIntervalProjection(0, 2)));
        assert((IntegerIntervalProjection(2, 7) & IntegerIntervalProjection(2)).equals(IntegerIntervalProjection(0, 2)));
        assert((IntegerIntervalProjection(-3, 3) & IntegerIntervalProjection(2)).equals(IntegerIntervalProjection(0, 2)));
        assert((IntegerIntervalProjection(-3, IntegerIntervalProjection::plus_infinity()) & IntegerIntervalProjection(2)).equals(IntegerIntervalProjection(0, 2)));
        assert((IntegerIntervalProjection(IntegerIntervalProjection::minus_infinity(), 3) & IntegerIntervalProjection(2)).equals(IntegerIntervalProjection(0, 2)));
        assert((IntegerIntervalProjection(1, 3) & IntegerIntervalProjection(1, 2)).equals(IntegerIntervalProjection(0, 2)));
        assert((IntegerIntervalProjection(-3, 3) & IntegerIntervalProjection(1, 2)).equals(IntegerIntervalProjection(0, 2)));
        assert((IntegerIntervalProjection(2, 7) & IntegerIntervalProjection(-2, 3)).equals(IntegerIntervalProjection(0, 7)));
        assert((IntegerIntervalProjection(-2, 7) & IntegerIntervalProjection(-2, 3)).equals(IntegerIntervalProjection::top()));
        assert((IntegerIntervalProjection(IntegerIntervalProjection::minus_infinity(), 7) & IntegerIntervalProjection(-2, 3)).equals(IntegerIntervalProjection::top()));
        assert((IntegerIntervalProjection(-2, IntegerIntervalProjection::plus_infinity()) & IntegerIntervalProjection(-2, 3)).equals(IntegerIntervalProjection::top()));
        assert((IntegerIntervalProjection(-2, 7) & IntegerIntervalProjection(IntegerIntervalProjection::minus_infinity(), 3)).equals(IntegerIntervalProjection::top()));
        assert((IntegerIntervalProjection(-2, 7) & IntegerIntervalProjection(-2, IntegerIntervalProjection::plus_infinity())).equals(IntegerIntervalProjection::top()));
        assert((IntegerIntervalProjection(-6, -3) & IntegerIntervalProjection(3, 9)).equals(IntegerIntervalProjection(0, 9)));
        assert((IntegerIntervalProjection(-6, 6) & IntegerIntervalProjection(3, 9)).equals(IntegerIntervalProjection(0, 9)));

        // Or |
        assert((IntegerIntervalProjection(4) | IntegerIntervalProjection::bottom()).equals(IntegerIntervalProjection::bottom()));
        assert((IntegerIntervalProjection::bottom() | IntegerIntervalProjection(2)).equals(IntegerIntervalProjection::bottom()));
        assert((IntegerIntervalProjection::top() | IntegerIntervalProjection(-1)).equals(IntegerIntervalProjection::top()));//
        assert((IntegerIntervalProjection(-1) | IntegerIntervalProjection::top()).equals(IntegerIntervalProjection::top()));//
        assert((IntegerIntervalProjection(4) | IntegerIntervalProjection(2)).equals(IntegerIntervalProjection(6)));
        assert((IntegerIntervalProjection(3) | IntegerIntervalProjection(2)).equals(IntegerIntervalProjection(3)));
        assert((IntegerIntervalProjection(-3) | IntegerIntervalProjection(2)).equals(IntegerIntervalProjection(-1)));
        assert((IntegerIntervalProjection(1, 3) | IntegerIntervalProjection(2)).equals(IntegerIntervalProjection(0, 3)));
        assert((IntegerIntervalProjection(2, 7) | IntegerIntervalProjection(2)).equals(IntegerIntervalProjection(0, 7)));
        assert((IntegerIntervalProjection(-3, 3) | IntegerIntervalProjection(2)).equals(IntegerIntervalProjection::top()));
        assert((IntegerIntervalProjection(-3, IntegerIntervalProjection::plus_infinity()) | IntegerIntervalProjection(2)).equals(IntegerIntervalProjection::top()));
        assert((IntegerIntervalProjection(IntegerIntervalProjection::minus_infinity(), 3) | IntegerIntervalProjection(2)).equals(IntegerIntervalProjection::top()));
        assert((IntegerIntervalProjection(1, 3) | IntegerIntervalProjection(1, 2)).equals(IntegerIntervalProjection(0, 3)));
        assert((IntegerIntervalProjection(-3, 3) | IntegerIntervalProjection(1, 2)).equals(IntegerIntervalProjection::top()));
        assert((IntegerIntervalProjection(2, 7) | IntegerIntervalProjection(-2, 3)).equals(IntegerIntervalProjection::top()));
        assert((IntegerIntervalProjection(-2, 7) | IntegerIntervalProjection(-2, 3)).equals(IntegerIntervalProjection::top()));
        assert((IntegerIntervalProjection(IntegerIntervalProjection::minus_infinity(), 7) | IntegerIntervalProjection(-2, 3)).equals(IntegerIntervalProjection::top()));
        assert((IntegerIntervalProjection(-2, IntegerIntervalProjection::plus_infinity()) | IntegerIntervalProjection(-2, 3)).equals(IntegerIntervalProjection::top()));
        assert((IntegerIntervalProjection(-2, 7) | IntegerIntervalProjection(IntegerIntervalProjection::minus_infinity(), 3)).equals(IntegerIntervalProjection::top()));
        assert((IntegerIntervalProjection(-2, 7) | IntegerIntervalProjection(-2, IntegerIntervalProjection::plus_infinity())).equals(IntegerIntervalProjection::top()));
        assert((IntegerIntervalProjection(-6, -3) | IntegerIntervalProjection(3, 9)).equals(IntegerIntervalProjection::top()));
        assert((IntegerIntervalProjection(-6, 6) | IntegerIntervalProjection(3, 9)).equals(IntegerIntervalProjection::top()));

        // Xor ^
        assert((IntegerIntervalProjection(4) ^ IntegerIntervalProjection::bottom()).equals(IntegerIntervalProjection::bottom()));
        assert((IntegerIntervalProjection::bottom() ^ IntegerIntervalProjection(2)).equals(IntegerIntervalProjection::bottom()));
        assert((IntegerIntervalProjection::top() ^ IntegerIntervalProjection(-1)).equals(IntegerIntervalProjection::top()));
        assert((IntegerIntervalProjection(-1) ^ IntegerIntervalProjection::top()).equals(IntegerIntervalProjection::top()));
        assert((IntegerIntervalProjection(4) ^ IntegerIntervalProjection(2)).equals(IntegerIntervalProjection(6)));
        assert((IntegerIntervalProjection(3) ^ IntegerIntervalProjection(2)).equals(IntegerIntervalProjection(1)));
        assert((IntegerIntervalProjection(-3) ^ IntegerIntervalProjection(2)).equals(IntegerIntervalProjection(-1)));
        assert((IntegerIntervalProjection(1, 3) ^ IntegerIntervalProjection(2)).equals(IntegerIntervalProjection(0, 3)));
        assert((IntegerIntervalProjection(2, 7) ^ IntegerIntervalProjection(2)).equals(IntegerIntervalProjection(0, 7)));
        assert((IntegerIntervalProjection(-3, 3) ^ IntegerIntervalProjection(2)).equals(IntegerIntervalProjection::top()));
        assert((IntegerIntervalProjection(-3, IntegerIntervalProjection::plus_infinity()) ^ IntegerIntervalProjection(2)).equals(IntegerIntervalProjection::top()));
        assert((IntegerIntervalProjection(IntegerIntervalProjection::minus_infinity(), 3) ^ IntegerIntervalProjection(2)).equals(IntegerIntervalProjection::top()));
        assert((IntegerIntervalProjection(1, 3) ^ IntegerIntervalProjection(1, 2)).equals(IntegerIntervalProjection(0, 3)));
        assert((IntegerIntervalProjection(-3, 3) ^ IntegerIntervalProjection(1, 2)).equals(IntegerIntervalProjection::top()));
        assert((IntegerIntervalProjection(2, 7) ^ IntegerIntervalProjection(-2, 3)).equals(IntegerIntervalProjection::top()));
        assert((IntegerIntervalProjection(-2, 7) ^ IntegerIntervalProjection(-2, 3)).equals(IntegerIntervalProjection::top()));
        assert((IntegerIntervalProjection(IntegerIntervalProjection::minus_infinity(), 7) ^ IntegerIntervalProjection(-2, 3)).equals(IntegerIntervalProjection::top()));
        assert((IntegerIntervalProjection(-2, IntegerIntervalProjection::plus_infinity()) ^ IntegerIntervalProjection(-2, 3)).equals(IntegerIntervalProjection::top()));
        assert((IntegerIntervalProjection(-2, 7) ^ IntegerIntervalProjection(IntegerIntervalProjection::minus_infinity(), 3)).equals(IntegerIntervalProjection::top()));
        assert((IntegerIntervalProjection(-2, 7) ^ IntegerIntervalProjection(-2, IntegerIntervalProjection::plus_infinity())).equals(IntegerIntervalProjection::top()));
        assert((IntegerIntervalProjection(-6, -3) ^ IntegerIntervalProjection(3, 9)).equals(IntegerIntervalProjection::top()));
        assert((IntegerIntervalProjection(-6, 6) ^ IntegerIntervalProjection(3, 9)).equals(IntegerIntervalProjection::top()));
    }

};


int main(int argc, char** argv)
{
    int arg_num = 0;
    int extraArgc = 3;
    char **arg_value = new char *[argc + extraArgc];
    for (; arg_num < argc; ++arg_num)
    {
        arg_value[arg_num] = argv[arg_num];
    }
    // add extra options
    arg_value[arg_num++] = (char*) "-model-consts=true";
    arg_value[arg_num++] = (char*) "-model-arrays=true";
    arg_value[arg_num++] = (char*) "-pre-field-sensitive=false";
    assert(arg_num == (argc + extraArgc) && "more extra arguments? Change the value of extraArgc");

    std::vector<std::string> moduleNameVec;
    moduleNameVec = OptionBase::parseOptions(
                        arg_num, arg_value, "Static Symbolic Execution", "[options] <input-bitcode...>"
                    );
    delete[] arg_value;
    if (AETEST())
    {
        AETest aeTest;
        aeTest.testBinaryOpStmt();
        return 0;
    }

    LLVMModuleSet::getLLVMModuleSet()->buildSVFModule(moduleNameVec);
    SVFIRBuilder builder;
    SVFIR* pag = builder.build();
    // Run Andersen's to resolve indirect calls, then update SVFIR with resolved targets.
    // The Andersen singleton will be reused inside AbstractInterpretation::runOnModule().
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