/* GNU __builtin_{add,sub,mul}_overflow (exact results, compared with gcc) and __has_builtin & co. */
#include <stdio.h>
#include <stdint.h>
#define T(expr, var, fmt) do { int o = expr; printf("%d " fmt " ", o, var); } while (0)
int main(void) {
    int8_t c; uint8_t uc; int i; unsigned u; long long ll; unsigned long long ull;
    T(__builtin_add_overflow(100, 27, &c), c, "%d");
    T(__builtin_add_overflow(100, 28, &c), c, "%d");
    T(__builtin_sub_overflow(0, 1, &uc), uc, "%d");
    T(__builtin_mul_overflow(-128, 1, &c), c, "%d");
    T(__builtin_mul_overflow(-1, 128, &c), c, "%d");
    T(__builtin_mul_overflow(-1, -128, &c), c, "%d");
    T(__builtin_add_overflow(-1, 1u, &u), u, "%u");
    T(__builtin_sub_overflow(1u, 2u, &i), i, "%d");
    T(__builtin_mul_overflow(3037000499LL, 3037000499LL, &ll), ll, "%lld");
    T(__builtin_mul_overflow(3037000500LL, 3037000500LL, &ll), ll, "%lld");
    T(__builtin_mul_overflow(-4294967296LL, 4294967296LL, &ll), ll, "%lld");
    T(__builtin_mul_overflow(0xffffffffffffffffull, 2, &ull), ull, "%llu");
    T(__builtin_sub_overflow(0, 0x8000000000000000ull, &ll), ll, "%lld");
    T(__builtin_sub_overflow(-1, 0x8000000000000000ull, &ll), ll, "%lld");
    T(__builtin_add_overflow(0x7fffffffffffffffLL, 0x7fffffffffffffffLL, &ull), ull, "%llu");
    puts("");
#if __has_builtin(__builtin_mul_overflow) && __has_c_attribute(nodiscard) && !__has_attribute(availability) && defined(__has_feature)
    puts("__has ok");
#endif
    return 0;
}
