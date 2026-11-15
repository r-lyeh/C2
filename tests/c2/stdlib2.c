#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#include <time.h>
#include <errno.h>
#include <inttypes.h>
#include <iso646.h>
#include <signal.h>
#include <locale.h>
#include <float.h>
#include <stdint.h>
#include <stddef.h>

static volatile sig_atomic_t got;
static void on_sig(int s) { got = s; }

int main(void) {
    char buf[64] = "a,b;;c", *t, out[128] = "";
    for (t = strtok(buf, ",;"); t; t = strtok(NULL, ",;")) { strcat(out, t); strcat(out, "|"); }
    printf("strtok %s spn %d cspn %d pbrk %s chr %s\n", out, (int)strspn("aab", "a"), (int)strcspn("abc", "c"),
           strpbrk("hello", "lo"), (char *)memchr("xyz", 'y', 3));
    printf("strtoul %lu strtoll %lld atoll %lld llabs %lld\n", strtoul("4000000000", NULL, 10), strtoll("-77", NULL, 10),
           atoll("123456789012"), llabs(-5LL));
    div_t d = div(17, 5); ldiv_t ld = ldiv(-17L, 5L); lldiv_t lld = lldiv(100LL, 7LL);
    printf("div %d %d ldiv %ld %ld lldiv %lld %lld\n", d.quot, d.rem, ld.quot, ld.rem, lld.quot, lld.rem);
    int e; double ip;
    printf("math %.3f %.3f %.3f %d %.2f %.2f %ld %lld %.3f %.1f %d %d %d\n", sinh(1.0), cbrt(27.0), frexp(8.0, &e), e,
           modf(3.75, &ip), ip, lrint(2.5), llround(-2.5), tgamma(5.0), fma(2, 3, 4), isnan(NAN), isinf(HUGE_VAL), signbit(-0.0));
    printf("ctype %d %d %d %d\n", !!isblank(' '), !!iscntrl('\n'), !!isgraph('a'), !!isprint(' '));
    time_t now = 86400 * 365;
    struct tm *g = gmtime(&now);
    char ts[64];
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M", g);
    struct tm tmv = {0}; tmv.tm_year = 100; tmv.tm_mday = 1; tmv.tm_isdst = -1;
    printf("time %s yday %d mktime_ok %d diff %.0f\n", ts, g->tm_yday, mktime(&tmv) != -1, difftime(10, 4));
    struct timespec sp; printf("timespec %d %d\n", timespec_get(&sp, TIME_UTC), sp.tv_nsec >= 0 && sp.tv_nsec < 1000000000);
    errno = 0; strtol("99999999999999999999999", NULL, 10);
    printf("errno ERANGE %d EDOM-set %d msg %d\n", errno == ERANGE, (errno = EDOM, errno == EDOM), strerror(ENOENT) != NULL);
    int64_t big = INT64_MAX; uint32_t u = UINT32_MAX;
    printf("inttypes %" PRId64 " %" PRIu32 " %" PRIx64 " %" PRIdMAX "\n", big, u, (uint64_t)255, imaxabs(-3));
    printf("iso646 %d %d\n", 1 and not 0, 0 or 0);
    signal(SIGINT, on_sig); raise(SIGINT);
    printf("signal %d\n", got == SIGINT);
    printf("locale %s\n", setlocale(LC_ALL, "C"));
    char *dup = strdup("dup"); printf("strdup %s %d\n", dup, DBL_DIG); free(dup);
    FILE *f = fopen("stdlib2_tmp.txt", "w+"); fprintf(f, "12 34"); rewind(f); int a, b; fscanf(f, "%d %d", &a, &b);
    ungetc('x', f); printf("file %d %d %c err %d\n", a, b, fgetc(f), ferror(f)); fclose(f); remove("stdlib2_tmp.txt");
    printf("sizes %d %d\n", (int)SIZE_MAX == -1, (int)sizeof(max_align_t) >= 8);
    return 0;
}
