/* demo_embed.c: embedding C² -- host functions (structs by value too), calling script functions, hot reload.
   Build: cc -DC2_NO_MAIN c2.c demo_embed.c + MIR (see CMakeLists.txt, target c2_demo) */
#include <stdio.h>
#include <string.h>
#include "c2.h"

typedef struct { float x, y, z; } Vec3;

static Vec3 vec3_add(Vec3 a, Vec3 b) { Vec3 r = {a.x + b.x, a.y + b.y, a.z + b.z}; return r; }
static void draw(const char *what, Vec3 at) { printf("draw %s at (%g, %g, %g)\n", what, at.x, at.y, at.z); }
int score = 10;   /* host variable shared with the script */

static const char *script_v1 =
    "typedef struct { float x, y, z; } Vec3;\n"
    "Vec3 vec3_add(Vec3 a, Vec3 b);\n"
    "void draw(const char *what, Vec3 at);\n"
    "extern int score;\n"
    "static Vec3 pos = {1, 2, 3};\n"
    "int frames;\n"
    "Vec3 update(float dt) {\n"
    "    Vec3 vel = {dt, 0, 0};\n"
    "    pos = vec3_add(pos, vel);\n"
    "    draw(\"player\", pos);\n"
    "    score += 1;\n"
    "    return pos;\n"
    "}\n";

static const char *script_v2 =   /* edited while the game runs */
    "typedef struct { float x, y, z; } Vec3;\n"
    "void draw(const char *what, Vec3 at);\n"
    "extern int score;\n"
    "Vec3 update(float dt) {\n"
    "    Vec3 p = {0, dt * 100, 0};\n"
    "    draw(\"enemy\", p);\n"
    "    score *= 2;\n"
    "    return p;\n"
    "}\n";

static const char *script_bad = "int update(float dt) { return dt + ; }\n";

int main(void) {
    C2 *c2 = C2_new();
    C2_add_symbol(c2, "vec3_add", (void *)vec3_add);
    C2_add_symbol(c2, "draw", (void *)draw);
    C2_add_symbol(c2, "score", &score);

    const char *err = C2_compile(c2, "game.c", script_v1);
    if (*err) { printf("%s", err); return 1; }
    Vec3 (*update)(float) = (Vec3 (*)(float))C2_get(c2, "update");
    Vec3 p = update(0.5f);
    update(0.5f);
    printf("returned (%g, %g, %g), score %d, frames %s\n", p.x, p.y, p.z, score, C2_get(c2, "frames") != NULL ? "found" : "missing");

    err = C2_compile(c2, "game.c", script_bad);             /* error: old program stays loaded */
    if (*err) printf("reload failed:\n%s", err);
    update(1);

    if (!*C2_compile(c2, "game.c", script_v2)) {           /* hot reload */
        update = (Vec3 (*)(float))C2_get(c2, "update");
        p = update(0.25f);
        printf("v2 returned (%g, %g, %g), score %d\n", p.x, p.y, p.z, score);
    }
    printf("missing symbol: %p\n", C2_get(c2, "nope"));
    C2_delete(c2);
    return 0;
}
