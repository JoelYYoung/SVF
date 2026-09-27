/* R15a_mutual_recursion_const_store: -handle-recur=top summary probe (skipRecursionWithTop). Expected May. */
#include <stdbool.h>
extern void svf_assert(bool);
int g;
void f(int n);
__attribute__((noinline)) void again(int n) { if (n > 0) f(n - 1); else g = 99; }
__attribute__((noinline)) void f(int n) { again(n); again(n); }
int main(void) {
  g = 1;
  f(1);
  svf_assert(g == 1);   /* QUERY R15.recursive_store May: concrete g = 99 */
  return 0;
}
