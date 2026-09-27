/* Build with clang -O0 -g -DCASE=N, then opt -passes=mem2reg.
 * Global cells are initialized before reading. All inputs lie in [-1000,1000].
 * These are correctness probes, not performance benchmarks. */
#include <stdbool.h>
extern int nondet_i32(void);
extern void svf_assert(bool);
int o, q;
__attribute__((noinline)) void overwrite(int v) { o=v; }
int main(void) {
    int x=nondet_i32(), v=nondet_i32();
    if(x < -1000 || x > 1000 || v < -1000 || v > 1000) return 0;
    o=x; q=v;
#if CASE == 1
    int t=o;
    bool cond=t<5;
    o=v;
    if(cond) svf_assert(o<5); /* May: write after comparison */
#elif CASE == 2
    int t=o;
    overwrite(v);
    if(t<5) svf_assert(o<5); /* May: intervening call */
#elif CASE == 3
    int t=o;
    if(t<5) {
        int y=o;
        svf_assert(y<5); /* Safe: fresh singleton load */
        svf_assert(y==x); /* Safe in whole Octagon: preserve relation */
    }
#elif CASE == 4
    int *p=(nondet_i32()!=0)?&o:&q;
    int t=*p;
    if(t<5) svf_assert(o<5); /* May: load need not read o */
#elif CASE == 5
    int t=o;
    o=v;
    switch(t) { case 0: svf_assert(o==0); break; default: break; } /* May */
#endif
    return 0;
}
