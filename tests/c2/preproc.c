#include <stdio.h>
#include <string.h>
#define STR_(x) #x
#define STR(x) STR_(x)
#include "inc/config.h"
#include "inc/config.h"      /* #pragma once: second include is empty */

#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define SQUARE(x) ((x) * (x))
#define CAT(a, b) a##b
#define XCAT(a, b) CAT(a, b)
#define LOG(fmt, ...) printf("[%s:%d] " fmt "\n", "pp", __LINE__, ##__VA_ARGS__)
#define COUNT_ARGS(...) COUNT_(__VA_ARGS__, 5, 4, 3, 2, 1, 0)
#define COUNT_(a, b, c, d, e, n, ...) n
#define FIELD(T, name) T name;
#define EMPTY
#define LPAREN (
#define f(x) (x + 1)
#define g f

/* C11 6.10.3.5 example 3 */
#define x 3
#define fx(a) fx(x * (a))
#undef x
#define x 2
#define gx fx
#define h gx(~
#define m(a) a(w)
#define w 0,1
#define t(a) a
#define p() int
#define q(x) x
#define r(x,y) x ## y

struct Point { FIELD(int, px) FIELD(int, py) };

#if defined(__C2__) && !defined(NOT_DEFINED) && VERSION_MAJOR * 10 + VERSION_MINOR >= 27
#define FEATURE 1
#elif VERSION_MAJOR > 1
#define FEATURE 2
#else
#define FEATURE 3
#endif

#ifdef __GNUC__
#undef FEATURE
#define FEATURE 1           /* gcc defines __GNUC__, C² defines __C2__: same result */
#endif

#ifndef FEATURE
#error FEATURE should be defined
#endif

#if 0
this is never compiled: { ] ( #bad directive stuff
#endif

int (fx)(int v) { return v; }   /* parenthesised: not a macro call */
int one = 1;
#define one (one + 1)   /* self reference: expanded once, not forever */

int main(void) {
    int CAT(my, var) = 5;
    struct Point pt = {1, 2};
    LOG("start");
    LOG("max %d sq %d", MAX(3, 7), SQUARE(1 + 2));
    printf("version %s, myvar %d, xcat %d, args %d %d %d\n", VERSION_STR, myvar, XCAT(my, var),
           COUNT_ARGS(a), COUNT_ARGS(a, b, c), COUNT_ARGS(1, 2, 3, 4, 5));
    printf("feature %d, point %d %d, file %s, stringize %s\n", FEATURE, pt.px, pt.py, strrchr(__FILE__, 'p'),
           STR(a  +   b "q\n" 'c'));
    int rec_base = 0;
#define rec_base rec_base + 1
    int r0 = rec_base;
    int g1 = g(2) + f(f(1));
    int std = fx(2) + gx(2) + q(1) + r(1, 2) + t(one);
    p() three = 3;
    printf("rec %d g %d std %d three %d func %s line %d\n", r0, g1, std, three, __func__, __LINE__);
    return FEATURE + included_times;
}
