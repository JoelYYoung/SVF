/* Ordinary sequential C harness. nondet_i32 has no callbacks or side effects.
 * Build CASE=1..5 with -O0 -g -Xclang -disable-O0-optnone, then mem2reg.
 */
#include <stdbool.h>
extern int nondet_i32(void);
extern void svf_assert(bool);
int g;
__attribute__((noinline)) void shared(int *q) { *q = nondet_i32(); }
#if CASE == 1
__attribute__((noinline)) void f1(void) {
  int a = nondet_i32(), local;
  if (a < -1000 || a > 1000) return;
  g = a; shared(&local); svf_assert(g == a); /* Safe */
}
__attribute__((noinline)) void f2(void) {
  int a = nondet_i32();
  if (a < -1000 || a > 1000) return;
  g = a; shared(&g); svf_assert(g == a); /* May */
}
int main(void) { f1(); f2(); return 0; }
#elif CASE == 2
/* Mutual recursion, with a concrete execution that changes g. The top
 * recursion policy must not prove the post-call assertion. */
__attribute__((noinline)) void f(int n);
__attribute__((noinline)) void again(int n) { if (n) f(n - 1); else g = 99; }
__attribute__((noinline)) void f(int n) { again(n); again(n); }
int main(void) { g = 1; f(1); svf_assert(g == 1); return 0; } /* May */
#elif CASE == 3
/* A completely resolved indirect call must include both targets. */
__attribute__((noinline)) void other(int *p) { *p = nondet_i32(); }
__attribute__((noinline)) void dispatch(int *p, bool c) {
  void (*fn)(int *) = c ? shared : other; fn(p);
}
int main(void) {
  int a = nondet_i32(), local;
  if (a < -1000 || a > 1000) return 0;
  g = a; dispatch(&local, true); svf_assert(g == a); /* Safe, may be imprecise */
  dispatch(&g, false); svf_assert(g == a); /* May */
  return 0;
}
#elif CASE == 4
/* Unknown external may call F. No admission solely from missing CG edges. */
extern void callback_boundary(void);
__attribute__((noinline)) void opaque_shared(void) { callback_boundary(); }
int main(void) { opaque_shared(); opaque_shared(); return 0; }
#elif CASE == 5
/* Unknown indirect target. Only the non-reentry decision is checked, not a
 * safety query under this deliberately incomplete external model. */
extern void (*get_callback(void))(void);
__attribute__((noinline)) void indirect_shared(void) { get_callback()(); }
int main(void) { indirect_shared(); indirect_shared(); return 0; }
#endif
