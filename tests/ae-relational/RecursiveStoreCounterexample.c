/* Concrete companion of CallerFrameCases.c CASE=2.
 * Exit 99 demonstrates the post-call g==1 assertion is false.
 */
int g;
void f(int n);
void again(int n) { if (n) f(n - 1); else g = 99; }
void f(int n) { again(n); again(n); }
int main(void) { g = 1; f(1); return g; }
