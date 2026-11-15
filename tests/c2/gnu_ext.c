/* GNU C extensions and C corners found by the c2m suite */
#include <stdio.h>
#include <math.h>

extern int later[];                       /* an incomplete array, sized further down */
static int sum_later(void) { return later[0] + later[2]; }
int later[3] = {10, 20, 30};

int tentative[];                          /* tentative, never sized: one element */

struct empty_member { int a; ; int b; };  /* a stray ';' (an empty macro) */
struct header { int n; char data[0]; };   /* GNU zero-length array */
struct pt { int x, y; };

static int run(const unsigned char *code) {   /* labels as values: a threaded interpreter */
    static void *ops[] = {&&op_inc, &&op_dbl, &&op_end};
    int acc = 1;
    const unsigned char *pc = code;
    goto *ops[*pc++];
op_inc: acc += 1; goto *ops[*pc++];
op_dbl: acc *= 2; goto *ops[*pc++];
op_end: return acc;
}

static int div_or_zero(int a, int b) {
    return ({
        if (b == 0) return 0;
        int r = a / b;
        goto done;
        r = -1;
done:
        r;                                    /* a label before the value */
    });
}

int main(void) {
    tentative[0] = 5;
    printf("later %d, tentative %d\n", sum_later(), tentative[0]);
    printf("stray ; %zu, zero-length %zu\n", sizeof(struct empty_member), sizeof(struct header));
    unsigned char prog[] = {0, 1, 1, 0, 2};
    printf("threaded %d\n", run(prog));
    printf("div %d %d\n", div_or_zero(7, 2), div_or_zero(7, 0));
    printf("postfix %d %d\n", (struct pt){3, 4}.y, ({ struct pt p = {5, 6}; p; }).x);
    printf("huge %d\n", HUGE_VALL > 1e308);
    return 0;
}
