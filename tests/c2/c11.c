#include <stdio.h>
#include <stdbool.h>
#include <stdalign.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(sizeof(int) == 4, "int is 32-bit");
static_assert(1);

struct Msg { int len; char data[]; };
struct Vec { float x; alignas(16) float y; };

#define type_of(x) _Generic((x), int: "int", double: "double", char *: "char *", const char *: "const char *", \
                            struct Msg *: "struct Msg *", bool: "bool", default: "other")
#define abs_of(x) _Generic((x), double: fabs_d, default: abs_i)(x)
static double fabs_d(double v) { return v < 0 ? -v : v; }
static int abs_i(int v) { return v < 0 ? -v : v; }

_Noreturn static void die(void) { exit(3); }
_Thread_local int counter;

int main(void) {
    bool b = 5, c = 0.25, d = NULL, e = &b;
    _Bool f = 0;
    f++; f++;
    bool g = 2; g += 3;
    printf("bool %d %d %d %d %d %d sizeof %d\n", b, c, d, e, f, g, (int)sizeof(bool));
    struct Msg *m = malloc(sizeof(struct Msg) + 6);
    m->len = 5; strcpy(m->data, "hello");
    printf("fam %d %s sizeof %d offset %d\n", m->len, m->data, (int)sizeof(struct Msg), (int)offsetof(struct Msg, data));
    printf("align %d %d %d off %d size %d\n", (int)_Alignof(double), (int)alignof(struct Vec), (int)_Alignof(char),
           (int)offsetof(struct Vec, y), (int)sizeof(struct Vec));
    char *s = "x"; const char *cs = s;
    printf("generic %s %s %s %s %s %s %s\n", type_of(1), type_of(2.0), type_of(s), type_of(cs), type_of(m), type_of(b), type_of(1L));
    printf("generic call %g %d\n", abs_of(-2.5), abs_of(-7));
    printf("hex %g %g %g long-double %g\n", 0x1p4, 0x1.8p1, 0xA.8p0, (double)1.5L);
    _Static_assert(_Alignof(long) == 8, "long");
    counter++;
    free(m);
    if (counter != 1) die();
    return 0;
}
