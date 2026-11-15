/* __on_enter / __on_leave: defined by the script, called around every other function */
#include <stdio.h>
static int depth;
void __on_enter(const char *func, const char *file, int line) { printf("%*s> %s (%s:%d)\n", depth * 2, "", func, file, line); depth++; }
void __on_leave(const char *func, const char *file, int line) { depth--; printf("%*s< %s (line %d)\n", depth * 2, "", func, line); }
typedef struct { double x, y; } V;
static V vmake(double a) { V v = {a, a + 1}; return v; }
static int fib(int n) {
    if (n < 2) return n;
    return fib(n - 1) + fib(n - 2);
}
static double half(double d) { return d / 2; }
int main(void) {
    V v = vmake(1);
    printf("%d %g %g\n", fib(3), half(5), v.y);
    return 0;
}
