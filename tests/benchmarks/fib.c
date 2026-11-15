/* naive recursive Fibonacci: measures call overhead */
#include <stdio.h>
#include <stdlib.h>

int fibR(int n) {
    if (n < 2) return n;
    return fibR(n - 2) + fibR(n - 1);
}

int main(int argc, char **argv) {
    int n = argc > 1 ? atoi(argv[1]) : 43;
    printf("fib(%d) = %d\n", n, fibR(n));
    return 0;
}
