#include <stdio.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
struct A { unsigned a:3, b:5; int c:7; char d; unsigned e:20, f:20; };
struct B { char x; int y:4; long long z:40; short w:3; };
struct C { int a:1; int :0; int b:2; unsigned :3; unsigned c:4; bool flag:1; };
union U { unsigned v; struct { unsigned lo:16, hi:16; } h; };
struct D { long long big:60; unsigned char t:2; };
struct E { int x; struct { int p:4, q:4; }; int r:12; };
/* layouts differ: GCC/clang pack bit-fields across types, MSVC starts a new unit when the type's size
   changes.  L() prints 1 when the value matches this platform's layout, else the value itself. */
#ifdef _WIN32
#define L(v, gcc, msvc) ((int)(v) == (msvc) ? 1 : (int)(v))
#else
#define L(v, gcc, msvc) ((int)(v) == (gcc) ? 1 : (int)(v))
#endif
int main(void) {
    struct A a = {5, 17, -3, 'k', 1000000, 999};
    printf("A %d %u %u %d %c %u %u\n", L(sizeof a, 12, 16), a.a, a.b, a.c, a.d, a.e, a.f);
    a.a = 9; a.b += 20; a.c = 63; a.c++; a.e = ~0u;
    printf("A' %u %u %d %u %u\n", a.a, a.b, a.c, a.e, a.f);
    struct B b = {1, -5, -123456789012LL, 3};
    printf("B %d %d %lld %d\n", L(sizeof b, 8, 24), b.y, (long long)b.z, b.w);
    struct C c = {0}; c.a = -1; c.b = 1; c.c = 15; c.flag = 7;
    printf("C %d %d %d %u %d\n", L(sizeof c, 8, 12), c.a, c.b, c.c, c.flag);
    union U u; u.v = 0x12345678;
    printf("U %x %x %d\n", u.h.lo, u.h.hi, L(sizeof u, 4, 4));
    struct D d = {-1, 3}; d.big -= 2;
    printf("D %d %lld %d\n", L(sizeof d, 8, 16), (long long)d.big, d.t);
    struct E e = {1, {2, -3}, 100}; e.q = 5;
    printf("E %d %d %d %d %d\n", L(sizeof e, 12, 12), e.x, e.p, e.q, e.r);
    struct A a2; memset(&a2, 0, sizeof a2); a2.a = 5; a2.b = 17; a2.c = -3; a2.d = 'k'; a2.e = 1000000; a2.f = 999;
    unsigned char *raw = (unsigned char *)&a2;
    int sum = 0; for (int i = 0; i < (int)sizeof a; i++) sum += raw[i] * (i + 1);
    printf("raw %d\n", L(sum, 3642, 5372));
    printf("promote %d %d\n", a.a - 10 < 0, (int)sizeof(a.a + 0));
    return 0;
}
