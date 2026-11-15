/* example.c: embedding C² in a host program.     Build and run: make example   (Windows: make.bat example)
   Shows: host symbols without a signature (the script declares them) and with one (declared once by the
   host: scripts just use them, and a script that disagrees gets an error), structs by value both ways,
   calling script functions, hot reload ([[persist]], __on_reload, a failed compile keeping the old
   program), and the script call stack (C2_set_debug, C2_backtrace). */
#include <stdio.h>
#include "c2.h"

typedef struct { float x, y, z; } Vec3;

static Vec3 vec3_add(Vec3 a, Vec3 b) { Vec3 r = {a.x + b.x, a.y + b.y, a.z + b.z}; return r; }
static void draw(const char *what, Vec3 at) { printf("  draw %s at (%g, %g, %g)\n", what, at.x, at.y, at.z); }
static void log_line(const char *text) { printf("  [log] %s\n", text); }
static void check(int ok, const char *what) {        /* a host assert: where in the script did it fail? */
    if (!ok) printf("  check failed: %s, script call stack:\n%s", what, C2_backtrace());
}
int score = 10;                                     /* shared with the script */

/* version 1 of the script: .c2, so it has methods, [[persist]] and zeroed locals */
static const char *game_v1 =
    "void log_line(const char *text);              // signatureless host symbol: declared here\n"
    "[[persist]] int frames;                       // survives hot reloads\n"
    "static Vec3 pos = {1, 2, 3};\n"
    "Vec3 update(float dt) {\n"
    "    pos = vec3_add(pos, (Vec3){dt, 0, 0});    // typed host symbols: no declarations needed\n"
    "    draw(\"player\", pos);\n"
    "    score += 1;\n"
    "    frames++;\n"
    "    return pos;\n"
    "}\n";

/* version 2, edited while the game runs */
static const char *game_v2 =
    "void log_line(const char *text);\n"
    "[[persist]] int frames;                       // keeps version 1's count\n"
    "void __on_reload(void) { log_line(\"reloaded\"); }\n"
    "Vec3 update(float dt) {\n"
    "    Vec3 p = Vec3(0, dt * 100, frames);       // a constructor (.c2)\n"
    "    draw(\"enemy\", p);\n"
    "    score *= 2;\n"
    "    check(score < 20, \"score < 20\");\n"
    "    frames++;\n"
    "    return p;\n"
    "}\n";

static const char *game_bad = "Vec3 update(float dt) { return dt + ; }\n";
static const char *game_clash = "void draw(int id);\nVec3 update(float dt) { draw(1); return (Vec3){0}; }\n";

int main(void) {
    C2 *c2 = C2_new();
    C2_set_debug(c2, 1);                                /* scripts keep a call stack: C2_backtrace() */

    /* without a signature: just a name and an address; the script declares it as it should be */
    C2_add_symbol(c2, "log_line", (void *)log_line);

    /* with a signature: declared once by the host, checked against what scripts say */
    C2_declare(c2, "typedef struct { float x, y, z; } Vec3;");
    const char *err = C2_add_function(c2, "Vec3 vec3_add(Vec3 a, Vec3 b);", (void *)vec3_add);
    if (!*err) err = C2_add_function(c2, "void draw(const char *what, Vec3 at);", (void *)draw);
    if (!*err) err = C2_add_function(c2, "void check(int ok, const char *what);", (void *)check);
    if (!*err) err = C2_add_variable(c2, "int score;", &score);   /* `extern` is implied */
    if (*err) { printf("%s\n", err); return 1; }

    puts("version 1:");
    err = C2_compile(c2, "game.c2", game_v1);
    if (*err) { printf("%s", err); return 1; }
    Vec3 (*update)(float) = (Vec3 (*)(float))C2_get(c2, "update");
    update(0.5f);
    Vec3 p = update(0.5f);
    int *frames = (int *)C2_get(c2, "frames");
    printf("  returned (%g, %g, %g), score %d, frames %d\n", p.x, p.y, p.z, score, *frames);

    puts("a broken edit:");
    err = C2_compile(c2, "game.c2", game_bad);              /* fails: version 1 stays loaded */
    printf("%s", err);
    update(1);

    puts("a script that disagrees with the host's signature:");
    printf("%s", C2_compile(c2, "game.c2", game_clash));

    puts("version 2 (hot reload):");
    err = C2_compile(c2, "game.c2", game_v2);
    if (*err) { printf("%s", err); return 1; }
    update = (Vec3 (*)(float))C2_get(c2, "update");         /* old addresses are gone: get them again */
    frames = (int *)C2_get(c2, "frames");
    p = update(0.25f);
    printf("  returned (%g, %g, %g), score %d, frames %d\n", p.x, p.y, p.z, score, *frames);

    printf("not in the script: %s\n", C2_get(c2, "nope") ? "found" : "NULL");
    C2_delete(c2);
    return 0;
}
