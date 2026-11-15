/* self tail calls become loops: 50 million deep without growing the stack */
#include <stdio.h>
static long count(long n, long acc) {
    if (n == 0) return acc;
    return count(n - 1, acc + 1);
}
static int swap(int a, int b, int k) {        /* arguments that read the other parameters */
    if (k == 0) return a * 10 + b;
    return swap(b, a, k - 1);
}
static unsigned char wrap(unsigned char c, int k) {   /* narrow parameters stay normalized */
    if (k == 0) return c;
    return wrap(c + 100, k - 1);
}
static double half(double d, int k) { return k ? half(d / 2, k - 1) : d; }
static int fib(int n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }   /* not a tail call */
int main(void) {
    printf("%ld %d %d %d %g %d\n", count(50000000, 0), swap(1, 2, 3), swap(1, 2, 4), wrap(1, 7), half(1024, 10), fib(20));
    return 0;
}
