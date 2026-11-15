#include <stdio.h>
#include <math.h>
#include <ctype.h>
typedef struct { double x, y; } V;
V add(V a, V b) { V r = {a.x + b.x, a.y + b.y}; return r; }
V scale(V a, double k) { a.x *= k; a.y *= k; return a; }
double len(V a) { return sqrt(a.x * a.x + a.y * a.y); }
int depth(V a, int n) { if (n == 0) return (int)a.x; a.x += 1; return depth(a, n - 1); }
int main(void) {
    V a = {3, 4}, b = {1, 1};
    V c = add(add(a, b), scale(b, 2));
    printf("%.1f %.1f %.1f %.1f %d\n", c.x, c.y, len(a), len(add(a, b)), depth(a, 5));
    printf("%d %c %.3f\n", isdigit('7') != 0, toupper('q'), sqrtf(2.0f));
    float f = 0.1f;
    printf("%f %g\n", f, f * 3);
    return (int)len(a);
}
