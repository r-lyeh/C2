/* ABI test, script side: compiled by the host (abi.c) as abi_script.c and again as abi_script.c2.
   Every struct type of abi_types.h goes both ways: script -> host calls here, host -> script calls
   in abi.c.  s_run() returns the number of failed checks. */
#include <stdio.h>
#include "abi_types.h"

/* implemented by the host */
#define HOST_DECL(T) \
    T h_make_##T(int seed); \
    long long h_sum_##T(T v); \
    long long h_sump_##T(const T *p); \
    long long h_mix_##T(int a, T v, double d, T w, int z);
ABI_ALL(HOST_DECL)

/* called by the host */
#define SCRIPT_DEF(T) \
    T s_make_##T(int seed) { return make_##T(seed); } \
    long long s_sum_##T(T v) { return sum_##T(v); } \
    long long s_mix_##T(int a, T v, double d, T w, int z) { \
        return a + sum_##T(v) * 3 + (long long)(d * 2) + sum_##T(w) * 5 + z; \
    } \
    T s_roundtrip_##T(int seed) { return h_make_##T(seed); }
ABI_ALL(SCRIPT_DEF)

static int failures, checks;
static void check(int ok, const char *what, const char *type) {
    checks++;
    if (!ok) { failures++; printf("  script: %s(%s) wrong\n", what, type); }
}

#define SCRIPT_CHECK(T) { \
    T local = make_##T(9); \
    long long (*fp)(T) = h_sum_##T; \
    check(sum_##T(h_make_##T(7)) == sum_##T(make_##T(7)), "h_make", #T); \
    check(h_sum_##T(make_##T(9)) == sum_##T(local), "h_sum", #T); \
    check(h_sum_##T(local) == sum_##T(local), "h_sum(local)", #T); \
    check(fp(local) == sum_##T(local), "h_sum via pointer", #T); \
    check(h_sump_##T(&local) == sum_##T(local), "h_sump", #T); \
    check(h_mix_##T(1, make_##T(2), 3.5, make_##T(4), 5) == \
          1 + sum_##T(make_##T(2)) * 3 + 7 + sum_##T(make_##T(4)) * 5 + 5, "h_mix", #T); \
    METHOD_CHECK(T) }

#ifdef __C2_EXT__    /* .c2: method calls pass the struct by value, or &x for a pointer parameter */
#define METHOD_CHECK(T) \
    check(local.h_sum_##T() == sum_##T(local), "x.h_sum()", #T); \
    check(local.h_sump_##T() == sum_##T(local), "x.h_sump()", #T);
#else
#define METHOD_CHECK(T)
#endif

int s_checks(void) { return checks; }
int s_run(void) {
    failures = checks = 0;
    ABI_ALL(SCRIPT_CHECK)
    return failures;
}
