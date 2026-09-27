/* R15d_self_recursion_nonconst_store: -handle-recur=top summary probe (skipRecursionWithTop). Expected May. */
#include <stdbool.h>
extern void svf_assert(bool);
int g;
__attribute__((noinline)) void rec(int n) { if (n > 0) rec(n - 1); else g = n + 98; }
int main(void) {
  g = 1;
  rec(1);
  svf_assert(g == 1);   /* QUERY R15c.self_recursion_direct_store May */
  return 0;
}
