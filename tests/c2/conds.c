#include <stdio.h>
#include <math.h>
/* conditions compiled as jumps: short-circuit order, nesting, negation, NaN */
static int calls;
static int t(int v) { calls = calls * 10 + (v ? 1 : 2); return v; }
static const char *yesno(int b) { return b ? "y" : "n"; }
int main(void) {
    int a = 1, b = 0, c = 1, r = 0;
    calls = 0; if (t(a) && t(b) || t(c)) r = 1; printf("%d %d\n", r, calls);
    calls = 0; if (t(b) || (t(a) && !(t(b) || t(c)))) r = 2; else r = 3; printf("%d %d\n", r, calls);
    calls = 0; if (!(t(a) && t(c))) r = 4; else r = 5; printf("%d %d\n", r, calls);
    calls = 0; if ((t(a) || t(b)) && (t(b) || t(c))) r = 6; printf("%d %d\n", r, calls);
    double nan = NAN, one = 1.0;
    printf("nan: %s %s %s %s %s\n", yesno(nan < one), yesno(!(nan < one)), yesno(nan >= one), yesno(nan != nan), yesno(one < 2.0 && !(nan > 0)));
    if (nan < one) puts("bad"); else puts("nan-else ok");
    if (!(nan < one) && one == 1.0) puts("not-less ok");
    int n = 0, i = 0;
    while (i < 10 && (i % 7 != 6 || n < 0)) { n += i; i++; }
    printf("while %d %d\n", n, i);
    for (i = 0, n = 0; i < 100 && !(n > 50 || i == 20); i++) n += 3;
    printf("for %d %d\n", n, i);
    i = 0; do { i += 2; } while (i < 9 && i != 6 || i == 7);
    printf("do %d\n", i);
    int x = 5, y = (x > 3 && x < 10) || x == 0;   /* value context still 0/1 */
    printf("value %d %d\n", y, !(x && 0) + (x || 0));
    return r;
}
