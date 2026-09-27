/* R15g_recursion_pointer_valued_store: -handle-recur=top summary probe (skipRecursionWithTop). Expected May. */
#include <stdbool.h>
#include <stddef.h>
extern void svf_assert(bool);
int a = 1, b = 2;
int *gp;
__attribute__((noinline)) void rec(int n) { if (n > 0) rec(n - 1); else gp = &b; }
int main(void) {
  gp = &a;
  rec(1);
  int v = *gp;
  svf_assert(v == 1);   /* QUERY R15g.pointer_store May: gp = &b */
  return 0;
}
