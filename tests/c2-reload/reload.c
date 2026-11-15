/* Hot reload test, host side: a C program that embeds C² and recompiles scripts while it runs, as a
   game does.  Checks the contract in c2.h: what a reload replaces, what a failed compile keeps,
   [[persist]] globals, __on_reload(), C2_keep_previous, and many reload cycles.  Every scenario runs
   with the machine-code generator, the interpreter and lazy generation.  test.py builds and runs
   it (suite "c2-reload"); by hand:
       cl /O1 /DNDEBUG /MT tests\c2-reload\reload.c 3rd\mir.c 3rd\mir-gen.c && reload
       cc -O1 -DNDEBUG tests/c2-reload/reload.c 3rd/mir.c 3rd/mir-gen.c -lm -ldl -lpthread -o reload && ./reload
   Exit code 0 when every check passes. */
#define C2_NO_MAIN
#include "../../c2.c"          /* C² itself, as a library (c2.h's API) */
#include <signal.h>

static int failures, checks;
static const char *mode_name;
static volatile int last_line;                  /* the last compile or check reached: where a crash happened */
#define CHECK(ok, ...) do { last_line = __LINE__; checks++; if (!(ok)) { failures++; printf("  [%s] line %d: ", mode_name, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); } } while (0)

static void on_crash(int sig) {
    printf("reload: crashed (signal %d) in [%s] after reload.c line %d\n", sig, mode_name, last_line);
    fflush(stdout);
    _Exit(3);
}

static C2 *c2;
#define compile(src) (last_line = __LINE__, compile_(src))
static int compile_(const char *src) {         /* 1 on success; prints unexpected errors */
    const char *err = C2_compile(c2, "script.c2", src);
    if (*err) printf("  [%s] compile error: %s", mode_name, err);
    return !*err;
}
#define FN(T, name) ((T)C2_get(c2, name))
#define VAR(T, name) (*(T *)C2_get(c2, name))

static void basics(void) {
    /* a reload replaces functions and re-runs initializers of plain globals */
    compile("int version(void) { return 1; }  int plain = 1;  void touch(void) { plain = 9; }");
    CHECK(FN(int (*)(void), "version")() == 1, "version 1");
    FN(void (*)(void), "touch")();
    CHECK(VAR(int, "plain") == 9, "plain set to 9");
    compile("int version(void) { return 2; }  int plain = 1;");
    CHECK(FN(int (*)(void), "version")() == 2, "version 2 after the reload");
    CHECK(VAR(int, "plain") == 1, "a plain global starts again (got %d)", VAR(int, "plain"));
    CHECK(C2_get(c2, "touch") == NULL, "touch() is gone");

    /* a failed compile keeps the loaded program, function pointers included */
    int (*v)(void) = FN(int (*)(void), "version");
    const char *err = C2_compile(c2, "script.c2", "int version(void) { return 3 }");
    CHECK(*err != '\0', "the broken script fails");
    CHECK(v() == 2 && FN(int (*)(void), "version") == v, "the old program is still there");
}

static void persist(void) {
    /* kept: same name and type, whatever the new initializer says */
    compile("[[persist]] int score = 5;  void add(int n) { score += n; }");
    FN(void (*)(int), "add")(10);
    compile("[[persist]] int score = 0;  int get(void) { return score; }");
    CHECK(FN(int (*)(void), "get")() == 15, "score kept (got %d)", FN(int (*)(void), "get")());

    /* a changed type starts again from its initializer, with a warning */
    compile("[[persist]] double score = 1.5;");
    CHECK(VAR(double, "score") == 1.5, "score re-initialized as a double");
    CHECK(strstr(C2_errors(c2), "type changed from 'int' to 'double'") != NULL, "warning: %s", C2_errors(c2));

    /* removed, then back: the program in between didn't have it, so it starts again */
    compile("int other;");
    compile("[[persist]] double score = 2.5;");
    CHECK(VAR(double, "score") == 2.5, "score back with its initializer");

    /* only [[persist]] globals carry over, and a global can become persistent at a reload */
    compile("int hp = 10;  void hit(void) { hp -= 3; }");
    FN(void (*)(void), "hit")();
    compile("[[persist]] int hp = 10;");
    CHECK(VAR(int, "hp") == 7, "hp carried over once marked (got %d)", VAR(int, "hp"));

    /* structs, arrays, heap pointers (the heap belongs to the process, not the program) */
    compile("struct P { int x, y; };  [[persist]] struct P pos;  [[persist]] int grid[4];\n"
            "[[persist]] char *name;\n"
            "void init(void) { pos.x = 3; pos.y = 4; grid[3] = 42; name = strdup(\"hero\"); }");
    FN(void (*)(void), "init")();
    compile("struct P { int x, y; };  [[persist]] struct P pos;  [[persist]] int grid[4];\n"
            "[[persist]] char *name;\n"
            "int sum(void) { return pos.x + pos.y + grid[3] + (int)strlen(name); }  void done(void) { free(name); }");
    CHECK(FN(int (*)(void), "sum")() == 3 + 4 + 42 + 4, "struct, array and heap string kept");
    FN(void (*)(void), "done")();

    /* a persisted function pointer moves to the new program's function of the same name */
    compile("int f1(void) { return 1; }  [[persist]] int (*op)(void);  void pick(void) { op = f1; }");
    FN(void (*)(void), "pick")();
    compile("int f1(void) { return 100; }  [[persist]] int (*op)(void);  int call(void) { return op(); }");
    CHECK(FN(int (*)(void), "call")() == 100, "op() calls the new f1");
    compile("int f2(void) { return 2; }  [[persist]] int (*op)(void);  int is_null(void) { return op == NULL; }");
    CHECK(FN(int (*)(void), "is_null")(), "op is NULL when f1 is gone");
    CHECK(strstr(C2_errors(c2), "pointed to 'f1'") != NULL, "warning: %s", C2_errors(c2));
}

static void on_reload(void) {
    /* __on_reload runs after the persisted values are in, and not on the first compile */
    const char *src = "[[persist]] int reloads;  int seen_score;  [[persist]] int score = 1;\n"
                      "void __on_reload(void) { reloads++; seen_score = score; }";
    C2_delete(c2);
    c2 = C2_new();
    if (strcmp(mode_name, "interp") == 0) C2_use_interpreter(c2, 1);
    if (strcmp(mode_name, "lazy") == 0) C2_set_lazy(c2, 1);
    compile(src);
    CHECK(VAR(int, "reloads") == 0, "not called on the first compile");
    VAR(int, "score") = 77;
    compile(src);
    CHECK(VAR(int, "reloads") == 1, "called once");
    CHECK(VAR(int, "seen_score") == 77, "after the values were carried (saw %d)", VAR(int, "seen_score"));
    compile(src);
    CHECK(VAR(int, "reloads") == 2, "called again");
}

static void keep_previous(void) {
    compile("const char *name(void) { return \"alpha\"; }  int level = 7;");
    const char *s = FN(const char *(*)(void), "name")();
    C2_keep_previous(c2, 1);
    compile("const char *name(void) { return \"beta\"; }");
    CHECK(strcmp(s, "alpha") == 0, "a string literal of the old program stays valid");
    CHECK(C2_get_previous(c2, "level") && *(int *)C2_get_previous(c2, "level") == 7, "old global readable");
    CHECK(strcmp(FN(const char *(*)(void), "name")(), "beta") == 0, "the new program is loaded");
    const char *(*old)(void) = (const char *(*)(void))C2_get_previous(c2, "name");
    CHECK(old && strcmp(old(), "alpha") == 0, "the old function still runs");
    compile("const char *name(void) { return \"gamma\"; }");   /* the next reload replaces the previous one */
    CHECK(strcmp(((const char *(*)(void))C2_get_previous(c2, "name"))(), "beta") == 0, "previous is now beta");
    C2_release_previous(c2);
    CHECK(C2_get_previous(c2, "name") == NULL, "released");
    C2_release_previous(c2);                                   /* twice: nothing to do */
    C2_keep_previous(c2, 0);
}

static void coroutines(void) {
    /* a suspended coroutine's stack returns into the program: reloading then is refused */
    const char *src = "#import \"runtime/coro\"\n"
                      "int n;  co *c;\n"
                      "void tick(void *arg) { for (;;) { n++; co_yield(); } }\n"
                      "void start(void) { c = co_create(tick); c.resume(); }\n"
                      "void stop(void) { c.free(); }\n";
    compile(src);
    FN(void (*)(void), "start")();
    CHECK(VAR(int, "n") == 1, "the coroutine ran once");
    const char *err = C2_compile(c2, "script.c2", src);
    CHECK(strstr(err, "can't reload while 1 coroutine is suspended") != NULL, "reload refused: %s", err);
    CHECK(VAR(int, "n") == 1 && FN(void (*)(void), "stop") != NULL, "the old program is still loaded");
    FN(void (*)(void), "stop")();
    CHECK(compile(src), "reload works once it is freed");
}

static void many(void) {
    /* many cycles: state carried every time, nothing piles up (run it under Valgrind or ASan) */
    const char *a = "[[persist]] long count;  void __on_reload(void) { count++; }  int which(void) { return 1; }";
    const char *b = "[[persist]] long count;  void __on_reload(void) { count++; }  int which(void) { return 2; }";
    int n = strcmp(mode_name, "jit") == 0 ? 300 : 100, ok = 1;
    compile(a);
    VAR(long, "count") = 0;
    for (int i = 1; i <= n && ok; i++) {
        ok = compile(i % 2 ? b : a);
        ok = ok && FN(int (*)(void), "which")() == (i % 2 ? 2 : 1) && VAR(long, "count") == i;
    }
    CHECK(ok, "%d reloads in a row", n);
}

static void reflected(void) {
    /* C2_reflected: a host finds a script's global by name (a console, an editor); after a reload, in the new one */
    const char *v1 = "#import \"runtime/reflect\"\n"
                     "enum Class { WARRIOR, MAGE };  struct Vec { float x, y; };\n"
                     "struct Hero { int hp; Vec pos; array(int) bag; Class cls; unsigned lvl : 4; Hero *target; };\n"
                     "Hero hero = {75, {3, 4}};  Hero boss = {.hp = 900};  [[skip]] int hidden;\n"
                     "void setup(void) { array_push(hero.bag, 10); array_push(hero.bag, 20); hero.cls = MAGE; hero.lvl = 9; hero.target = &boss; }\n";
    const char *v2 = "#import \"runtime/reflect\"\n"
                     "struct Hero { char name[8]; [[clamp(0, 50)]] double hp; };  Hero hero = {\"Bo\", 12.5};\n";
    CHECK(compile(v1), "the first script");
    FN(void (*)(void), "setup")();
    c2ref r = C2_reflected(c2, "hero.hp");
    CHECK(r.type && r.type->kind == C2_INT && *(int *)r.p == 75 && r.field && !strcmp(r.field->name, "hp"), "hero.hp");
    *(int *)r.p = 80;
    CHECK(VAR(int, "hero") == 80, "set through the reference");
    r = C2_reflected(c2, "/hero/pos/y");
    CHECK(r.type && !strcmp(r.type->name, "float") && *(float *)r.p == 4, "/hero/pos/y");
    r = C2_reflected(c2, "hero.bag[1]");
    CHECK(r.type && *(int *)r.p == 20, "hero.bag[1]");
    CHECK(!C2_reflected(c2, "hero.bag[2]").type && !C2_reflected(c2, "hero.nope").type && !C2_reflected(c2, "hidden").type, "not found");
    r = C2_reflected(c2, "hero.cls");
    CHECK(r.type && r.type->kind == C2_ENUM && r.type->count == 2 && !strcmp(r.type->values[*(int *)r.p].name, "MAGE"), "an enum, by its table");
    r = C2_reflected(c2, "hero.lvl");
    CHECK(r.bits == 4 && ((*(unsigned *)r.p >> r.bitpos) & 15) == 9, "a bit-field");
    r = C2_reflected(c2, "hero.target.hp");
    CHECK(r.type && *(int *)r.p == 900, "through a pointer");
    CHECK(compile(v2), "the second script");
    r = C2_reflected(c2, "hero.hp");
    CHECK(r.type && r.type->kind == C2_FLOAT && *(double *)r.p == 12.5 && (r.field->flags & C2_CLAMP) && r.field->max == 50, "the new hero.hp");
    r = C2_reflected(c2, "hero.name");
    CHECK(r.type && r.type->kind == C2_ARRAY && !strcmp((char *)r.p, "Bo") && !strcmp(r.type->name, "char [8]"), "hero.name");
    CHECK(!C2_reflected(c2, "boss").type, "boss is gone");
    CHECK(compile("int plain = 1;") && !C2_reflected(c2, "plain").type, "no runtime: no list");
}

static const char *inner_error;
static int host_recompile(void) {               /* the script calls this; it compiles, as a console would */
    const char *e = C2_compile(c2, "script.c2", "int value(void) { return 2; }");
    inner_error = e;
    return *e != 0;
}
static void safe_reload(void) {
    /* C2_safe_reload: a compile from inside a script call is refused; after it returns, it works */
    const char *src = "int host_recompile(void);\nint depth(int n) { return n ? depth(n - 1) : host_recompile(); }\n"
                      "int run(void) { return depth(3); }\nint value(void) { return 1; }\n";
    C2_add_symbol(c2, "host_recompile", (void *)host_recompile);
    C2_safe_reload(c2, 1);
    CHECK(compile(src), "a script that calls the host");
    CHECK(FN(int (*)(void), "run")() == 1 && strstr(inner_error, "can't reload while 5 script functions are running"), "refused: %s", inner_error);
    CHECK(FN(int (*)(void), "value")() == 1, "the program is still the old one");
    CHECK(compile("int value(void) { return 3; }") && FN(int (*)(void), "value")() == 3, "after the call returns: fine");
    C2_safe_reload(c2, 0);
}

static void migrate(void) {
    /* a [[persist]] struct whose type changed moves field by field (both programs import the runtime) */
    const char *v1 = "#import \"runtime\"\n"
                     "enum Class { WARRIOR, MAGE, ROGUE };  struct Item { string name; int count; };\n"
                     "struct Hero { string name; int hp; float speed; Class cls; array(Item) bag; string gone; unsigned lvl : 4; int ids[3]; int tag; };\n"
                     "[[persist]] Hero hero;\n"
                     "void setup(void) { hero.name = string(\"Ada Lovelace, Countess\"); hero.hp = 75; hero.speed = 2.5; hero.cls = ROGUE;\n"
                     "  array_push(hero.bag, (Item){string(\"potion\"), 3}); array_push(hero.bag, (Item){string(\"key\"), 1});\n"
                     "  hero.gone = string(\"a field the next version drops\"); hero.lvl = 9; hero.ids[0] = 1; hero.ids[1] = 2; hero.ids[2] = 3; hero.tag = 7; }\n";
    const char *v2 = "#import \"runtime\"\n"
                     "enum Class { ROGUE = 10, MAGE, WARRIOR, BARD };  struct Item { int count; string name; float weight; };\n"
                     "struct Hero { int level; Class cls; [[clamp(0, 50)]] double hp; string name; array(Item) bag; int speed; short ids[2];\n"
                     "              unsigned char lvl; string tag; bool fresh; };\n"
                     "[[persist]] Hero hero = {.fresh = true, .level = 1};\n"
                     "int check(void) { return hero.level == 1 && hero.cls == ROGUE && hero.hp == 50 && !strcmp(hero.name.data(), \"Ada Lovelace, Countess\") &&\n"
                     "  array_count(hero.bag) == 2 && hero.bag[0].count == 3 && !strcmp(hero.bag[1].name.data(), \"key\") && hero.bag[1].weight == 0 &&\n"
                     "  hero.speed == 2 && hero.ids[0] == 1 && hero.ids[1] == 2 && hero.lvl == 9 && hero.tag.len() == 0 && hero.fresh; }\n";
    CHECK(compile(v1), "the first Hero");
    FN(void (*)(void), "setup")();
    CHECK(compile(v2), "the second Hero");
    CHECK(FN(int (*)(void), "check")(), "moved field by field: names, numbers, enum by name, clamp, array items");
    CHECK(strstr(C2_errors(c2), "'hero.tag' starts again: its type changed from 'int' to 'string'") != NULL, "warning: %s", C2_errors(c2));
    CHECK(compile(v1), "and back again");
    const char *m1 = "#import \"runtime\"\n"
                     "enum Kind { FIRE, ICE };\n"
                     "[[persist]] map(const char *, int) scores;  [[persist]] map(int, Kind) kinds;  [[persist]] set(short) seen;\n"
                     "void fill(void) { map_set(scores, \"ada\", 3); map_set(scores, \"bo\", 7); map_set(kinds, 1, ICE);\n"
                     "  map_set(kinds, 2, FIRE); set_add(seen, 5); set_add(seen, 9); }\n";
    const char *m2 = "#import \"runtime\"\n"
                     "enum Kind { WATER, ICE, FIRE };\n"
                     "[[persist]] map(const char *, double) scores;  [[persist]] map(long long, Kind) kinds;  [[persist]] set(int) seen;\n"
                     "int check(void) { return map_count(scores) == 2 && *map_get(scores, \"bo\") == 7.0 && *map_get(scores, \"ada\") == 3.0 &&\n"
                     "  *map_get(kinds, 1LL) == ICE && *map_get(kinds, 2LL) == FIRE && set_has(seen, 9) && set_count(seen) == 2; }\n";
    CHECK(compile(m1), "maps");
    FN(void (*)(void), "fill")();
    CHECK(compile(m2) && FN(int (*)(void), "check")(), "maps and sets whose types changed move entry by entry");
    const char *s1 = "#import \"runtime\"\n"
                     "[[persist]] map(string, int) ages;  [[persist]] set(string) names;\n"
                     "void fill(void) { ages.set(\"a name much longer than twenty-three bytes\", 3); ages.set(\"bo\", 7);\n"
                     "  names.add(\"another name longer than twenty-three\"); }\n";
    const char *s2 = "#import \"runtime\"\n"
                     "[[persist]] map(string, double) ages;  [[persist]] set(string) names;\n"
                     "int check(void) { names.add(\"another name longer than twenty-three\");\n"
                     "  return ages.count() == 2 && *ages.get(\"a name much longer than twenty-three bytes\") == 3.0 &&\n"
                     "  *ages.get(\"bo\") == 7.0 && names.count() == 1; }\n";
    CHECK(compile(s1), "string keys");
    FN(void (*)(void), "fill")();
    CHECK(compile(s2) && FN(int (*)(void), "check")(), "string keys move with their text");
}

static int host_unlink(const char *path) { (void)path; return 42; }
static void sandbox(void) {
    /* C2_sandbox: a script can't delete files or start programs; the host's own symbols still win */
    const char *src = "#include <stdio.h>\n#include <stdlib.h>\nint unlink(const char *p);\n"
                      "int removed(void) { return remove(\"no-such-file\") == -1 && unlink(\"x\") == 42; }\n"
                      "int ran(void) { return system(\"exit 0\"); }\n";
    C2_add_symbol(c2, "unlink", (void *)host_unlink);
    C2_sandbox(c2, 1);
    CHECK(compile(src), "a sandboxed script");
    CHECK(FN(int (*)(void), "removed")(), "remove refused, the host's unlink called");
    CHECK(FN(int (*)(void), "ran")() == -1, "system refused");
    CHECK(*C2_compile(c2, "s.c2", "int getpid(void); int main() { return getpid(); }") != '\0', "nothing else found by name");
    const char *quit = "#include <stdlib.h>\nint steps;\n"
                       "int step(int n) { steps++; if (n == 3) exit(5); if (n == 4) abort(); return n; }\n"
                       "int update(int n) { int r = step(n); steps += 10; return r; }\n";
    CHECK(compile(quit), "a script that exits");
    CHECK(FN(int (*)(int), "update")(1) == 1 && VAR(int, "steps") == 11 && C2_exited(c2, NULL) == 0, "runs as usual");
    int code = 0;
    FN(int (*)(int), "update")(3);
    CHECK(C2_exited(c2, &code) == 1 && code == 5 && VAR(int, "steps") == 12, "exit(5) came back to the host (steps %d)", VAR(int, "steps"));
    CHECK(FN(int (*)(int), "update")(2) == 2 && VAR(int, "steps") == 23 && C2_exited(c2, NULL) == 0, "and the script runs again");
    FN(int (*)(int), "update")(4);
    CHECK(C2_exited(c2, &code) == 2, "abort() came back too");
    C2_sandbox(c2, 0);
    CHECK(compile(src) && FN(int (*)(void), "ran")() == 0, "system runs again with the sandbox off");
}

int main(void) {
    signal(SIGSEGV, on_crash);                   /* (Windows: access violations too) */
    signal(SIGILL, on_crash);
    signal(SIGFPE, on_crash);
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *modes[] = {"jit", "interp", "lazy"};
    for (int m = 0; m < 3; m++) {
        mode_name = modes[m];
        c2 = C2_new();
        if (m == 1) C2_use_interpreter(c2, 1);
        if (m == 2) C2_set_lazy(c2, 1);
        basics();
        persist();
        keep_previous();
        on_reload();                                         /* (starts a fresh C2) */
        coroutines();
        many();
        reflected();
        sandbox();
        migrate();
        safe_reload();
        C2_delete(c2);
    }
    printf("reload: %d checks, %d failed\n", checks, failures);
    return failures != 0;
}
