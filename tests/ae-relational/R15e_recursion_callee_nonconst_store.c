/* R15e_recursion_callee_nonconst_store: -handle-recur=top summary probe (skipRecursionWithTop). Expected May. */
#include <stdbool.h>
extern void svf_assert(bool);
int g;
__attribute__((noinline)) void leaf(int n) { g = n + 98; }
__attribute__((noinline)) void rec(int n) { if (n > 0) rec(n - 1); else leaf(n); }
int main(void) {
  g = 1;
  rec(1);
  svf_assert(g == 1);   /* QUERY R15b.self_recursion_callee_store May */
  return 0;
}
