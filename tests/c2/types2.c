#include <stdio.h>
#include <limits.h>

int main(void) {
    int i = INT_MAX;
    i = i + 1;                                   /* wraps to INT_MIN (32-bit) */
    unsigned u = 0;
    u = u - 1;                                   /* UINT_MAX */
    unsigned char uc = 250;
    uc += 10;                                    /* 4 */
    signed char sc = 127;
    sc++;                                        /* -128 */
    short s = 32767;
    s++;
    unsigned short us = 65535;
    us++;
    long l = 1L << 40;
    unsigned long ul = ULONG_MAX;
    printf("int wrap %d, uint %u, uchar %d, schar %d, short %d, ushort %d\n", i, u, uc, sc, s, us);
    printf("long %lld, ulong %llu, sizes %d %d %d %d %d %d %d\n", l, ul, (int)sizeof(char), (int)sizeof(short),
           (int)sizeof(int), (int)sizeof(long), (int)sizeof(float), (int)sizeof(double), (int)sizeof(void *));
    printf("unsigned cmp %d %d, signed cmp %d\n", u > 5u, -1 < 1u, -1 < 1);
    printf("udiv %u umod %u sdiv %d smod %d\n", 4000000000u / 3u, 4000000000u % 7u, -7 / 2, -7 % 2);
    printf("shifts %d %u %d %lld\n", -16 >> 2, 0x80000000u >> 4, 1 << 31, 1L << 62);
    printf("literals %d %d %d %u %lld\n", 0x1F, 017, 0b1011, 0xFFFFFFFF, 10000000000);
    float f = 1.0f / 3.0f;
    double d = 1.0 / 3.0;
    printf("float %.9f double %.17f mixed %.6f\n", f, d, f * 3 + d);
    int fi = (int)3.99f;
    unsigned fu = (unsigned)3e9;
    float big = 16777217;                        /* not representable: 16777216 */
    printf("conv %d %u %.1f %d %.3f\n", fi, fu, big, (int)-2.7, (float)7 / 2);
    char c = 'A' + 2;
    printf("char %c %d, promo %d\n", c, c, c + 1);
    long long ll = -5;
    unsigned long long ull = 5;
    printf("ll %lld %llu %d\n", ll, ull, ll < (long long)ull);
    return (i == INT_MIN) + (u == UINT_MAX) * 2 + (uc == 4) * 4 + (sc == -128) * 8 + (fi == 3) * 16;
}
