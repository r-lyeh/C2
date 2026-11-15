#include <stdio.h>
#include <limits.h>
/* 32-bit integer semantics: wrap-around, narrowing, widening, mixed signedness */
unsigned hash(const char *s) { unsigned h = 5381; while (*s) h = h * 33 + (unsigned char)*s++; return h; }
int smul(int a, int b) { return a * b; }
long widen(int x) { return x; }
long uwiden(unsigned x) { return x; }

int main(void) {
    long big = (1L << 40) + 7;
    int lo = (int)big;                 /* 7 */
    long back = lo;                    /* 7, not 1<<40 + 7 */
    unsigned u = 0xFFFFFFFFu;
    u = u + 2;                         /* 1 */
    int neg = -5;
    unsigned un = (unsigned)neg;       /* 4294967291 */
    long l1 = widen(-3), l2 = uwiden(3000000000u);
    int m = smul(70000, 70000);        /* wraps (as gcc does in practice) */
    int idx[4] = {10, 20, 30, 40};
    int k = 5;
    k = k * 1000000000 / 1000000000;   /* overflow inside: just exercise it */
    int i3 = 3;
    printf("%d %lld %u %u %lld %lld %d\n", lo, back, u, un, l1, l2, idx[i3 & 3]);
    printf("hash %u %u\n", hash("hello"), hash("the quick brown fox"));
    printf("cmp %d %d %d %d\n", -1 < 0, (unsigned)-1 > 0u, un > 5u, (int)un < 0);
    printf("div %d %d %u %u\n", -7 / 2, -7 % 2, 4000000000u / 3u, 4000000000u % 7u);
    printf("shift %d %u %d\n", -16 >> 2, 0x80000000u >> 31, 1 << 30);
    double d = lo + u;                 /* 8.0 */
    long sum = 0;
    for (int i = 0; i < 100000; i++) sum += i;   /* int i, long sum */
    switch (lo + 1) { case 8: printf("switch ok %.1f %lld\n", d, sum); break; default: printf("switch bad\n"); }
    if (lo - 7) printf("if bad\n");
    return (back == 7) + (m != 0) * 2;
}
