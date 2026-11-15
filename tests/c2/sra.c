#include <stdio.h>
typedef struct { int a; char c; double d; float f; long l; unsigned char u; void *p; short s; } M;
typedef struct { float x, y; } V2;
static V2 add(V2 a, V2 b) { V2 r = {a.x + b.x, a.y + b.y}; return r; }
static M bump(M m) { m.a++; m.c += 2; m.d *= 2; return m; }
int main(void) {
    M m = {1, 'a', 2.5, 1.5f, 7, 250, 0, -3};
    m.u += 10;                      /* wraps */
    m.c++; m.s *= 2; m.l <<= 3; m.f /= 2;
    M n = bump(m);                  /* whole value in and out */
    M o; o = n;                     /* whole assignment */
    o.a += m.a;
    V2 p = {1, 2}, v = {0.5f, -1}, z;
    for (int i = 0; i < 10; i++) { p = add(p, v); if (p.y < 0) { p.y = -p.y; v.y = -v.y; } }
    z = (m.a > 100) ? p : v;
    const V2 k = {3, 4};
    V2 w = k; w.x += k.y;
    printf("%d %c %.2f %.2f %ld %u %d %d %c %.1f\n", m.a, m.c, m.d, m.f, m.l, m.u, m.s, o.a, o.c, o.d);
    printf("%.2f %.2f %.2f %.2f %.1f\n", p.x, p.y, z.x, z.y, w.x);
    return o.a;
}
