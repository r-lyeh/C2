/* C23: bool/true/false keywords, #elifdef/#elifndef, __VA_OPT__, enums with a fixed underlying type,
   multi-character constants (source order) */
#include <stdio.h>
#define F(a, ...) f(a __VA_OPT__(,) __VA_ARGS__)
#define G(...) g(0 __VA_OPT__(, __VA_ARGS__ + 1))
#define CAT(x, ...) x ## __VA_OPT__(_tail) ## _end
#define EMPTY
int f(int a, ...) { return a; }
int g(int a, ...) { return a; }
int x_end = 1, x_tail_end = 2;
#ifdef NOPE
#error no
#elifdef __C2__
#define PICK 1
#else
#define PICK 2
#endif
#if 0
#elifndef NOPE
#define PICK2 3
#endif
#if true && !false
#define T_OK 1
#endif
enum Small : unsigned char { S0, S1 = 200 };
enum Big : long long { B0 = 1LL << 40 };
enum Wide { W0 = 0x100000000 };
enum Small sm = S1;
int main(void) {
    bool b = true, c = false;
    printf("%d %d %d %d %d %d %d %d\n", F(7), F(1, 2, 3), G(), G(5), CAT(x), CAT(x, y), PICK, PICK2);
    printf("%d %d %d %zu %zu\n", T_OK, b, c, sizeof(true), sizeof(bool));
    printf("%zu %zu %d %lld %zu %zu %lld\n", sizeof(enum Small), sizeof sm, sm, (long long)B0, sizeof(enum Big), sizeof(W0), (long long)W0);
    printf("%x %x %d %d %x\n", 'ab', '1234', '12' < '1234', 'a', '\377\1');
    #if 'ab' == 0x6162
    puts("pp multichar ok");
    #endif
    return 0;
}
