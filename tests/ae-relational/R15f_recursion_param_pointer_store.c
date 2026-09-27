/* R15f_recursion_param_pointer_store: -handle-recur=top summary probe (skipRecursionWithTop). Expected May. */
#include <stdbool.h>
extern void svf_assert(bool);
int g;
__attribute__((noinline)) void rec(int *p, int n) { if (n > 0) rec(p, n - 1); else *p = n + 98; }
int main(void) {
  g = 1;
  rec(&g, 1);
  svf_assert(g == 1);   /* QUERY R15f.param_pointer_store May */
  return 0;
}
