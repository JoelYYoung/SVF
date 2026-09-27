// SPDX-License-Identifier: AGPL-3.0-or-later
#include "AE/Core/PackedRelationalDomain.h"
#include <iostream>
#include <stdexcept>
namespace A = SVF::AbstractDomain;
using E=A::LinearExpression;
using D=A::PackedRelationalDomain;
const A::Variable x(1),y(2),o(3),z(4);
E c(long n) { return E(A::Rational(n)); }
void check(bool b,const char* message) { if(!b) throw std::runtime_error(message); }
bool entails(const D& d,const A::LinearConstraint& c) { return d.entails(c)==A::CheckResult::True; }
void range(D& d,A::Variable v,long lo,long hi)
{ d.assume(A::greaterEqual(E(v),c(lo))); d.assume(A::lessEqual(E(v),c(hi))); }
int main()
{
    try
    {
        auto schema=std::make_shared<const D::Packing>(D::Packing{{x,y,o},{y,z}});
        {
            D d(schema); d.assign(o,c(1)); d.assign(x,E(o)); d.assign(o,c(2)); d.assign(y,E(o));
            check(entails(d,A::equal(E(y)-E(x),c(1))),"store must preserve old scalar value");
        }
        for(bool write:{false,true})
        {
            D d(schema); range(d,o,1,10); d.assign(x,E(o));
            if(write) d.forget(o);
            range(d,o,2,10); d.assign(y,E(o));
            check(entails(d,A::equal(E(x),E(y)))==!write,"assume versus overwrite");
        }
        {
            D d(schema); range(d,x,0,10); d.assign(y,E(x)+c(1));
            d.assume(A::lessEqual(E(x),c(5)));
            check(entails(d,A::lessEqual(E(y),c(6))),"indirect assume refinement");
            d.assign(z,E(y));
            check(!entails(d,A::lessEqual(E(z),c(6))),"overlap has no automatic reduction");
            d.assign(z,E(x));
            check(entails(d,A::lessEqual(E(z),c(5))),"outside read interval");
        }
        {
            D d(schema); d.assign(x,c(1)); d.assign(y,c(2)); d.assign(z,c(3));
            d.assignParallel({{x,E(y)},{y,E(z)},{z,E(x)}});
            check(entails(d,A::equal(E(x),c(2))) && entails(d,A::equal(E(y),c(3))) &&
                  entails(d,A::equal(E(z),c(1))),"parallel assignments use one prestate");
        }
        {
            D d(schema); range(d,x,0,10); d.expand(x,{y});
            check(!entails(d,A::equal(E(x),E(y))),"expand must not create equality");
            check(entails(d,A::lessEqual(E(y),c(10))),"expand copies bounds");
        }
        {
            D left(schema),right(schema); left.assign(x,c(0)); left.assign(y,c(2));
            right.assign(x,c(1)); right.assign(y,c(3)); left.joinWith(right);
            check(entails(left,A::equal(E(y)-E(x),c(2))),"join affine invariant");
            check(right.isSubsetOf(left)==A::CheckResult::True,"join upper bound");
            auto widened=left; widened.widenWith(right);
            check(left.isSubsetOf(widened)==A::CheckResult::True,"widen upper bound");
            left.forget(x); check(!entails(left,A::equal(E(y)-E(x),c(2))),"forget kills relation");
        }
        {
            D d(schema); d.assign(y,E(x)+c(1)); d.assume(A::equal(E(x),E(y)));
            check(d.isBottom(),"relational contradiction");
            D top(schema); d.joinWith(top); check(d.isTop(),"bottom normalized for join");
        }
        {
            D d(schema); d.assign(z,c(4)); d.fold(y,{z});
            check(!entails(d,A::equal(E(y),c(4))),"fold includes previous target");
            d.project({y}); check(d.bound(z).isTop(),"project removes object");
        }
        std::cout<<"PackedRelationalDomain: 9 transfer scenarios PASS\n";
        return 0;
    }
    catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
