/* C23 preprocessor bits: #embed, __has_include, __has_embed, digit separators.
   c23pp.bin is "AB<tab>C" (no newline, so git line-ending conversion cannot change it) */
#include <stdio.h>
#if !defined(__has_include) || !defined(__has_embed)
#error "__has_include / __has_embed should be defined"
#endif
#if __has_include(<stdio.h>) && __has_include("c23pp.c") && !__has_include("no-such-file.h")
#define HAS_INCLUDE_OK 1
#endif
#if __has_embed("c23pp.bin") == __STDC_EMBED_FOUND__ && __has_embed("no-such-file.bin") == __STDC_EMBED_NOT_FOUND__ \
    && __has_embed("c23pp.bin" limit(0)) == __STDC_EMBED_EMPTY__ && __has_embed("c23pp.bin" unknown_param) == 0
#define HAS_EMBED_OK 1
#endif
static const unsigned char all[] = {
#embed "c23pp.bin"
};
static const unsigned char two[] = {
#embed "c23pp.bin" limit(1 + 1) suffix(, 0)
};
static const char str[] = {
#embed "c23pp.bin" prefix('[', ) suffix(, ']', '\0')
};
static const int none[] = {
#embed "c23pp.bin" limit(0) if_empty(-1)
};
int main(void) {
    long long big = 1'000'000, hex = 0xFF'FF, oct = 0'17;
    double d = 1'234.5;
    printf("%d %d %d %d %d %d %d [%s] %d\n", HAS_INCLUDE_OK, HAS_EMBED_OK, (int)sizeof all, all[0], all[3], two[1], two[2], str, none[0]);
    printf("%lld %lld %lld %g %c\n", big, hex, oct, d, 'x');
    return 0;
}
