/* R15h_first_write_in_recursion: -handle-recur=top summary probe (bd0b7dba review). */
#include <stdbool.h>
extern void svf_assert(bool);
__attribute__((noinline)) void rec(int *p, int n) { if (n > 0) rec(p, n - 1); else *p = 7; }
int main(void) {
  int x;                 /* no write before the summarized call */
  rec(&x, 1);
  int y = x;
  svf_assert(y == 5);    /* QUERY R15h.first_write_in_recursion May: concrete x = 7 */
  return 0;
}
