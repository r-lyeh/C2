#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <stdint.h>
#include <stdbool.h>

int main(int argc, char **argv) {
    clock_t t0 = clock();
    long acc = 0;
    for (int i = 0; i < 1000000; i++) acc += i % 7;
    double dt = (double)(clock() - t0) / CLOCKS_PER_SEC;
    char *a = calloc(10, 1);
    int zeros = 0;
    for (int i = 0; i < 10; i++) zeros += a[i] == 0;
    a = realloc(a, 20);                 /* plain C realloc: bytes 10..19 are unspecified */
    free(a);
    uint8_t u8 = 255; int64_t big = INT64_MAX; bool ok = true;
    printf("argc=%d argv[1]=%s acc=%lld dt_ok=%d calloc_zeros=%d u8=%d big=%lld ok=%d clock_ok=%d\n",
           argc, argc > 1 ? argv[1] : "-", acc, dt >= 0 && dt < 5, zeros, u8 + 1, big, ok, clock() >= 0);
    return 0;
}
