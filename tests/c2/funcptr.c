#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int (*binop)(int, int);
typedef struct { const char *name; binop fn; } Op;

int add(int a, int b) { return a + b; }
int sub(int a, int b) { return a - b; }
int mul(int a, int b) { return a * b; }

Op ops[] = {{"add", add}, {"sub", sub}, {"mul", &mul}};

int apply(binop f, int a, int b) { return f(a, b); }
binop pick(int i) { return ops[i].fn; }
int (*pick2(int i))(int, int) { return i ? sub : add; }

int cmp_int(const void *a, const void *b) {
    int x = *(const int *)a, y = *(const int *)b;
    return (x > y) - (x < y);
}
int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

double twice(double x) { return 2 * x; }
float halve(float x) { return x / 2; }

void (*hook)(int) = NULL;
int hooked;
void on(int v) { hooked += v; }

int main(void) {
    int total = 0;
    for (int i = 0; i < 3; i++) {
        int r = ops[i].fn(7, 3);
        printf("%s(7,3) = %d\n", ops[i].name, r);
        total += r;
    }
    printf("apply %d, pick %d, pick2 %d, deref %d, call-expr %d\n",
           apply(mul, 6, 7), pick(1)(10, 4), pick2(0)(2, 3), (*pick(2))(5, 5), (pick2(1))(9, 1));
    int v[] = {5, 3, 9, 1, 7, 2};
    qsort(v, 6, sizeof v[0], cmp_int);
    for (int i = 0; i < 6; i++) printf("%d ", v[i]);
    char *words[] = {"pear", "apple", "fig"};
    qsort(words, 3, sizeof(char *), cmp_str);
    printf("| %s %s %s\n", words[0], words[1], words[2]);
    int key = 7;
    int *found = bsearch(&key, v, 6, sizeof(int), cmp_int);
    double (*fd)(double) = twice;
    float (*ff)(float) = &halve;
    binop table[2] = {add, sub};
    int (*arr_of_fp[2])(int, int) = {mul, add};
    printf("bsearch %lld, fp-double %.1f, fp-float %.2f, table %d %d, eq %d %d\n", found - v, fd(1.25), ff(3.0f),
           table[1](8, 5), arr_of_fp[0](4, 4), table[0] == add, fd != 0);
    if (!hook) hook = on;
    hook(5);
    hook(6);
    return total + hooked;     /* 10 + 4 + 21 + 11 = 46 */
}
