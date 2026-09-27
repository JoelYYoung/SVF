#include <stdbool.h>
#include <stdint.h>
extern void svf_assert(bool);
int g = 1, h = 0, other = 2;
#if CASE == 1
__attribute__((noinline)) void rec(int n) { if (n) rec(n - 1); else h = 99; }
int main(void) { rec(1); svf_assert(g == 1); return 0; } /* Safe: no write to g */
#elif CASE == 2
__attribute__((noinline)) int *rec(int n) { return n ? rec(n - 1) : &other; }
int main(void) { int *p = rec(1); svf_assert(*p == 1); return 0; } /* May */
#elif CASE == 3
extern void external_write(void);
__attribute__((noinline)) void rec(int n) { if (n) rec(n - 1); else external_write(); }
int main(void) { rec(1); svf_assert(g == 1); return 0; } /* May: unknown global write */
#elif CASE == 4
__attribute__((noinline)) void rec(uintptr_t p, int n) {
  if (n) rec(p, n - 1); else *(int *)p = 99;
}
int main(void) { rec((uintptr_t)&g, 1); svf_assert(g == 1); return 0; } /* May */
#elif CASE == 5
__attribute__((noinline)) void rec(int n) { if (n) rec(n - 1); else g = 99; }
int main(void) { int a = g; rec(1); svf_assert(a == 1); return 0; } /* Safe: caller SSA */
#endif
