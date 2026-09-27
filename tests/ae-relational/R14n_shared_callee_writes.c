/* R14n: negative companion of R14; the second call writes g. Must stay May. */
#include <stdbool.h>
extern int nondet_i32(void);
extern void svf_assert(bool);
int g;
__attribute__((noinline)) void shared(int *q) { *q = nondet_i32(); }   /* writes g on the 2nd call */
int main(void) {
  int a = nondet_i32();
  if (a < -1000 || a > 1000) return 0;
  int other;
  g = a;
  shared(&other);
  svf_assert(g == a);              /* QUERY R14n.first Safe (relational) */
  shared(&g);
  svf_assert(g == a);              /* QUERY R14n.second May */
  return 0;
}

