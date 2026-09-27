/* R15i_heap_allocated_in_recursion: -handle-recur=top summary probe (bd0b7dba review). */
#include <stdbool.h>
#include <stdlib.h>
extern void svf_assert(bool);
int *h;
__attribute__((noinline)) void rec(int n) { if (n > 0) rec(n - 1); else { h = malloc(sizeof(int)); *h = 7; } }
int main(void) {
  h = 0;
  rec(1);
  int y = *h;
  svf_assert(y == 5);    /* QUERY R15i.heap_allocated_in_recursion May: concrete 7 */
  return 0;
}
