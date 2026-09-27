/* R15j_recursion_no_write_positive: -handle-recur=top summary probe (bd0b7dba review). */
#include <stdbool.h>
extern void svf_assert(bool);
int g;
__attribute__((noinline)) void rec(int n) { if (n > 0) rec(n - 1); }
int main(void) {
  int a = 3;
  g = 1;
  rec(1);
  svf_assert(g == 1);    /* QUERY R15j.no_write_positive Safe */
  svf_assert(a == 3);    /* QUERY R15j.caller_ssa_positive Safe */
  return 0;
}
