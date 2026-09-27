/* R14: shared callee called twice from one non-recursive caller, writing only
 * its argument. g == a holds after both calls. On d3-next faf68bcd Dense/Oh
 * prove Safe/Safe with Post pass, Semi gives May/May, D3 gives Safe/Safe but
 * 2 Post Fail on the return edges: the Semi shared-callee Post restore forgets
 * caller SSA scalars, losing a == content(g). Same cause as libproxy 2->114/137. */
#include <stdbool.h>
extern int nondet_i32(void);
extern void svf_assert(bool);
int g;
__attribute__((noinline)) void shared(int *q) { *q = nondet_i32(); }   /* writes only *q */
int main(void) {
  int a = nondet_i32();
  if (a < -1000 || a > 1000) return 0;
  int cell, other;
  g = a;                           /* memory cell related to the SSA scalar a */
  shared(&other);                  /* call site 1: callee writes other, not g */
  svf_assert(g == a);              /* QUERY R14.first Safe (relational) */
  shared(&cell);                   /* call site 2 */
  svf_assert(g == a);              /* QUERY R14.second Safe (relational) */
  return 0;
}

