#include <stdio.h>
#include <string.h>

typedef struct { float x, y; } Vec2;
struct Rect { Vec2 min, max; char name[8]; int id; };
struct Node { int v; struct Node *next; };

Vec2 vadd(Vec2 a, Vec2 b) { a.x += b.x; a.y += b.y; return a; }
float area(struct Rect r) { r.min.x = 1000; return (r.max.x - r.min.x + 1000) * (r.max.y - r.min.y); }
struct Rect make(int id) {
    struct Rect r = {{0, 0}, {2, 3}, "rect", 0};
    r.id = id;
    return r;
}

int primes[] = {2, 3, 5, 7, 11};
const char *names[] = {"zero", "one", "two"};
struct Rect grect = {{1, 1}, {4, 5}, "glob", 7};
Vec2 gpts[3] = {{1, 2}, {3, 4}};
char greet[] = "hi there";
double gd = 2.5;
int gsum = 3 * 4 + (1 << 2);

int main(void) {
    Vec2 a = {1, 2}, b = {10, 20};
    Vec2 c = vadd(a, b);
    struct Rect r = make(9);
    struct Rect r2 = r;
    r2.name[0] = 'R';
    float ar = area(r);
    printf("vadd %.1f %.1f  a %.1f  area %.1f  r.min.x %.1f\n", c.x, c.y, a.x, ar, r.min.x);
    printf("make %s %d %.1f  copy %s\n", r.name, r.id, r.max.y, r2.name);
    struct Node n3 = {3, NULL}, n2 = {2, &n3}, n1 = {1, &n2};
    int sum = 0;
    for (struct Node *p = &n1; p; p = p->next) sum += p->v;
    int arr[6] = {1, 2, 3};
    int brace[] = {5, 6, 7, 8};
    struct Rect rs[2] = {{{0, 0}, {1, 1}, "a", 1}, {{0, 0}, {2, 2}, "b", 2}};
    Vec2 elided[2] = {1, 2, 3, 4};
    char word[10] = "abc";
    printf("list %d %d %d %d | %d %d | %s %s %d | %.0f %.0f | %s %d\n", sum, arr[2], arr[3], arr[5],
           (int)(sizeof brace / sizeof brace[0]), brace[3], rs[0].name, rs[1].name, rs[1].id,
           elided[1].x, elided[1].y, word, word[5]);
    printf("globals %d %s %s %d %.0f %.0f %.0f %s %d %.1f %d\n", primes[4], names[2], grect.name, grect.id,
           grect.max.y, gpts[1].y, gpts[2].x, greet, (int)sizeof greet, gd, gsum);
    Vec2 t = sum > 5 ? a : b;
    printf("ternary %.0f\n", t.x);
    for (int i = 0; i < 3; i++) {
        int z = 0;                     /* .c: locals have no value until set (zeroinit.c2 tests .c2) */
        struct Rect zr = {0};
        printf("zero %d %d %.0f;", z, zr.id, zr.max.x);
        z = 99;
        zr.id = 99;
    }
    printf("\n");
    return sum + (int)ar;
}
