#include <stdio.h>

static int counter = 100;
static const char *label = "lbl";

int next_id(void) {
    static int id;                 /* zero, once */
    return ++id;
}

int fib_memo(int n) {
    static long memo[50];
    if (n < 2) return n;
    if (memo[n]) return (int)memo[n];
    return (int)(memo[n] = fib_memo(n - 1) + fib_memo(n - 2));
}

const char *weekday(int d) {
    static const char *names[] = {"mon", "tue", "wed", "thu", "fri", "sat", "sun"};
    return names[d % 7];
}

int *shared(void) {
    static int x = 5, *px = &x;
    return px;
}

int main(void) {
    next_id(); next_id();
    int third = next_id();
    for (int i = 0; i < 3; i++) {
        static int loops;           /* not reset by the loop */
        loops++;
        if (i == 2) printf("loops %d\n", loops);
    }
    *shared() += 1;
    printf("id %d fib %d day %s %s shared %d counter %d %s\n", third, fib_memo(40), weekday(2), weekday(13),
           *shared(), counter, label);
    return third + *shared();
}
