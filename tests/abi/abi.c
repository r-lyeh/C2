/* ABI test, host side: a C program built by the platform's compiler that embeds C², hands it
   host functions taking and returning structs of awkward sizes (abi_types.h), compiles
   abi_script.c (as .c and as .c2) and calls its functions with the same structs.  verify.py builds
   and runs it (suite "abi"); by hand:
       cl /O1 /DNDEBUG /MT tests\abi\abi.c mir\mir.c mir\mir-gen.c && abi tests\abi\abi_script.c
       cc -O1 -DNDEBUG -fno-tree-sra tests/abi/abi.c mir/mir.c mir/mir-gen.c -lm -ldl -lpthread -o abi
   With two more arguments (abi_lib.h and the abi_lib library without extension) it also tests
   C2_load / C2_unload.
   Exit code 0 when every check passes. */
#define C2_NO_MAIN
#include "../../c2.c"          /* C² itself, as a library (c2.h's API) */
#include "abi_types.h"

#define HOST_DEF(T) \
    T h_make_##T(int seed) { return make_##T(seed); } \
    long long h_sum_##T(T v) { return sum_##T(v); } \
    long long h_sump_##T(const T *p) { return sum_##T(*p); } \
    long long h_mix_##T(int a, T v, double d, T w, int z) { \
        return a + sum_##T(v) * 3 + (long long)(d * 2) + sum_##T(w) * 5 + z; \
    }
ABI_ALL(HOST_DEF)

static int failures, checks;
static void check(int ok, const char *what, const char *type, const char *file) {
    checks++;
    if (!ok) { failures++; printf("  host: %s(%s) wrong [%s]\n", what, type, file); }
}

#define ADD_SYMBOLS(T) \
    C2_add_symbol(c2, "h_make_" #T, (void *)h_make_##T); \
    C2_add_symbol(c2, "h_sum_" #T, (void *)h_sum_##T); \
    C2_add_symbol(c2, "h_sump_" #T, (void *)h_sump_##T); \
    C2_add_symbol(c2, "h_mix_" #T, (void *)h_mix_##T);

#define HOST_CHECK(T) { \
    T (*mk)(int) = (T (*)(int))C2_get(c2, "s_make_" #T); \
    long long (*sm)(T) = (long long (*)(T))C2_get(c2, "s_sum_" #T); \
    long long (*mx)(int, T, double, T, int) = (long long (*)(int, T, double, T, int))C2_get(c2, "s_mix_" #T); \
    T (*rt)(int) = (T (*)(int))C2_get(c2, "s_roundtrip_" #T); \
    if (!mk || !sm || !mx || !rt) { check(0, "C2_get", #T, file); } else { \
        check(sum_##T(mk(11)) == sum_##T(make_##T(11)), "s_make", #T, file); \
        check(sm(make_##T(12)) == sum_##T(make_##T(12)), "s_sum", #T, file); \
        check(mx(1, make_##T(2), 3.5, make_##T(4), 5) == \
              1 + sum_##T(make_##T(2)) * 3 + 7 + sum_##T(make_##T(4)) * 5 + 5, "s_mix", #T, file); \
        check(sum_##T(rt(13)) == sum_##T(make_##T(13)), "s_roundtrip", #T, file); \
    } }

static void run(const char *file, const char *source) {
    C2 *c2 = C2_new();
    ABI_ALL(ADD_SYMBOLS)
    const char *err = C2_compile(c2, file, source);
    if (*err) { printf("  %s: compile failed:\n%s", file, err); failures++; C2_delete(c2); return; }
    int (*srun)(void) = (int (*)(void))C2_get(c2, "s_run");
    int (*schecks)(void) = (int (*)(void))C2_get(c2, "s_checks");
    failures += srun();
    checks += schecks();
    ABI_ALL(HOST_CHECK)
    C2_delete(c2);
}

/* C2_load: a header + a shared library (argv[2], without extension), then C2_unload */
static const char *lib_script =
    "int lib_check(void) {\n"
    "    lib_vec3 a = {1, 0, 0}, b = {0, 1, 0};\n"
    "    lib_vec3 c = lib_cross(a, b);\n"
    "    double v[3] = {1.5, 2.5, 3};\n"
    "    int bad = 0;\n"
    "    bad += lib_add(2, 3) != 5;\n"
    "    bad += c.x != 0 || c.y != 0 || c.z != 1;\n"
    "    bad += lib_sum(3, v) != 7;\n"
    "    bad += lib_name()[0] != 'a';\n"
    "    return bad;\n"
    "}\n";

static void run_load(const char *header, const char *library) {
    C2 *c2 = C2_new();
    const char *err = C2_load(c2, header, library);
    check(!*err, "C2_load", header, err);
    if (!*err) {
        err = C2_compile(c2, "lib.c", lib_script);
        check(!*err, "compile with C2_load", "lib", err);
        int (*lc)(void) = (int (*)(void))C2_get(c2, "lib_check");
        check(lc && lc() == 0, "library calls", "lib", "lib.c");
        check(!*C2_unload(c2, header), "C2_unload", header, "");
        check(*C2_compile(c2, "lib.c", lib_script) != 0, "compile after C2_unload fails", "lib", "lib.c");
        check(lc && lc() == 0, "old program still runs after C2_unload", "lib", "lib.c");
    }
    C2_delete(c2);
}

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "tests/abi/abi_script.c";
    FILE *f = fopen(path, "rb");
    if (!f) { printf("cannot open %s\n", path); return 2; }
    static char source[1 << 16];
    source[fread(source, 1, sizeof source - 1, f)] = '\0';
    fclose(f);
    char path2[1024];
    snprintf(path2, sizeof path2, "%s2", path);   /* the same source as a .c2 file: method calls too */
    run(path, source);
    run(path2, source);
    if (argc > 3) run_load(argv[2], argv[3]);
    printf("abi: %d checks, %d failed\n", checks, failures);
    return failures != 0;
}
