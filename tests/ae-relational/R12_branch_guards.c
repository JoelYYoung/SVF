/* R12 guard probes for the shared branch-memory refinement fix (12a57147).
 * Build: clang -O0 -Xclang -disable-O0-optnone -g -DCASE=N; opt -passes=mem2reg.
 * Red on 20d73d8c (false Safe, fixed to May): 1 call, 2 non-singleton load,
 * 3 stale switch, 7 unsigned guard read as signed.
 * Precision costs of the fix (true Safe -> May): 4/9 cast (whole Octagon too),
 * 5 unsigned fresh, 8 cross-block fresh (Box/Oh only; whole Octagon keeps it).
 * 6 fresh switch stays Safe. Note: CASE 4 was meant to overwrite w but writes o;
 * it is kept as recorded and duplicates 9. */
#include <stdbool.h>
extern int nondet_i32(void);
extern void svf_assert(bool);
int o, q; unsigned u; long w;
__attribute__((noinline)) void overwrite(int v) { o=v; }
int main(void) {
  int x=nondet_i32(), v=nondet_i32(), c=nondet_i32();
  if (x < -1000 || x > 1000 || v < -1000 || v > 1000) return 0;
  o=x; q=v;
#if CASE == 1  /* call between load and branch, same block */
  int t=o; overwrite(v);
  if (t<5) { int y=o; svf_assert(y<5); }
#elif CASE == 2  /* non-singleton pointer, load and branch fresh */
  int *p = c ? &o : &q;
  int t=*p;
  if (t<5) { int y=o; svf_assert(y<5); }
#elif CASE == 3  /* switch on stale load */
  int t=o; o=v;
  switch (t) { case 0: { int y=o; svf_assert(y==0); break; } default: break; }
#elif CASE == 4  /* sext cast of a fresh load, then overwrite by store */
  w=x; long t=w; o=v;
  if ((int)t<5) { long y=w; svf_assert(y<5); }
#elif CASE == 5  /* unsigned compare on fresh load */
  u=(unsigned)x;
  unsigned t=u;
  if (t<5u) { unsigned y=u; svf_assert(y<5u); svf_assert((int)y>=0); }
#elif CASE == 6  /* switch, fresh singleton: precision probe */
  int t=o;
  switch (t) { case 3: { int y=o; svf_assert(y==3); break; } default: break; }
#elif CASE == 7  /* unsigned guard read as signed: x=-1 gives u=UINT_MAX>5u, (int)u=-1 */
  u=(unsigned)x;
  unsigned t=u;
  if (t>5u) { int y=(int)u; svf_assert(y>5); }
#elif CASE == 8  /* fresh load, branch in a later block, no write: precision cost */
  int t=o;
  if (c) q=1;
  if (t<5) { int y=o; svf_assert(y<5); }
#elif CASE == 9  /* sext of a fresh load, same object, no write: precision cost */
  w=x; long t=w;
  if ((int)t<5) { long y=w; svf_assert(y<5); }
#endif
  return 0;
}

