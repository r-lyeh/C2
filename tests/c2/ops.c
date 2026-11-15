#include <stdio.h>

int main(void) {
    int a = 6, b = 3, c = 5;
    printf("prec %d %d %d %d %d\n", a + b * c, a - b - c, a / b * c, a % 4 + b << 1, 1 + 2 << 3 >> 1);
    printf("bits %d %d %d %d %d %d\n", a & b, a | b, a ^ b, ~a, a & b | c ^ 1, (a & 4) == 4);
    printf("cmp %d %d %d\n", a < b == 0, a == 6 && b == 3 || c == 0, !a || !!b);
    printf("cond %d %d %s\n", a > b ? a : b, a < b ? 1 : c > 4 ? 2 : 3, a ? "yes" : "no");
    int x = 10;
    x += 5; x -= 3; x *= 4; x /= 3; x %= 7;     /* 15,12,48,16,2 */
    printf("compound %d", x);
    x = 0xF0; x &= 0x3C; x |= 1; x ^= 0xFF; x <<= 2; x >>= 1;
    printf(" %d\n", x);
    int arr[5] = {10, 20, 30, 40, 50};
    int *p = arr;
    int v1 = *p++;          /* 10 */
    int v2 = *++p;          /* 30 */
    int v3 = (*p)++;        /* 30, arr[2] = 31 */
    p += 2;                 /* arr[4] */
    int v4 = *p--;          /* 50 */
    int v5 = p[-1];         /* 31 */
    p -= 1;
    printf("ptr %d %d %d %d %d %d %d %lld\n", v1, v2, v3, v4, v5, *p, 2[arr], p - arr);
    int i = 5;
    int j = i++ + i++;      /* unsequenced in C, avoid: use separate */
    j = 0;
    j = ++i;
    j += i--;
    printf("incdec %d %d\n", i, j);
    double d = 1.5;
    d++; ++d; d *= 2; d -= 0.5;
    float f = 2.0f;
    f /= 4; f += 1;
    printf("fp %.2f %.2f\n", d, f);
    int k = (a = 1, b = 2, a + b);
    printf("comma %d\n", k);
    unsigned m = 1;
    m = ~m >> 28;
    printf("unsigned not/shift %u %d\n", m, -1 >> 30);
    return x + k;
}
