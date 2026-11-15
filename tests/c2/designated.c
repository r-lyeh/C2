#include <stdio.h>

struct Vec { int x, y, z; };
struct Line { struct Vec a, b; const char *tag; };
union Val { int i; float f; struct { short lo, hi; }; };
typedef struct { int id; char name[8]; struct Vec pos; } Ent;

int sparse[] = {[2] = 20, [5] = 50, 60};                  /* size 7 */
struct Vec gv = {.z = 3, .x = 1};
struct Line gl = {.b.y = 9, .tag = "glob", .a = {1, 2, 3}};
Ent ents[] = {{.id = 1, .name = "one"}, [2] = {.pos.z = 7, .id = 3}};
struct Vec *gptr = &(struct Vec){10, 20, 30};             /* file-scope literal: static storage */
const char *days[7] = {[0] = "sun", [6] = "sat"};

int sum(struct Vec v) { return v.x + v.y + v.z; }
int total(const int *a, int n) { int s = 0; for (int i = 0; i < n; i++) s += a[i]; return s; }

int main(void) {
    struct Vec v = {.y = 5};
    union Val u = {.f = 1.5f};
    union Val w = {.lo = 1, 2};                            /* continues to .hi */
    int grid[3][3] = {[1][1] = 5, [2] = {7, 8, 9}, [0][2] = 3};
    printf("sparse %d %d %d %d n=%d\n", sparse[2], sparse[5], sparse[6], sparse[0], (int)(sizeof sparse / sizeof *sparse));
    printf("gv %d %d %d gl %d %d %d %d %s\n", gv.x, gv.y, gv.z, gl.a.x, gl.a.z, gl.b.y, gl.b.x, gl.tag);
    printf("ents %d %s %d %d %d n=%d\n", ents[0].id, ents[0].name, ents[1].id, ents[2].id, ents[2].pos.z,
           (int)(sizeof ents / sizeof ents[0]));
    printf("v %d %d %d u %08x w %d %d grid %d %d %d %d %d\n", v.x, v.y, v.z, u.i, w.lo, w.hi, grid[0][2], grid[1][1],
           grid[2][0], grid[2][2], grid[1][0]);
    printf("literals %d %d %d %d %s %s %d\n", sum((struct Vec){1, 2, 3}), sum((struct Vec){.z = 10}),
           total((int[]){1, 2, 3, 4}, 4), gptr->y, days[0], days[6], days[3] == 0);
    for (int i = 0; i < 3; i++) {
        int *p = (int[3]){i, i * 2};                       /* fresh literal each iteration */
        printf("loop %d %d %d;", p[0], p[1], p[2]);
    }
    struct Vec *pv = &(struct Vec){.x = 4};
    pv->y = 6;
    printf(" pv %d\n", sum(*pv));
    return sum(gv) + grid[1][1] + sum(*gptr);
}
