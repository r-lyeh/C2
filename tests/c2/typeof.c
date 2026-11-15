/* C23: typeof, typeof_unqual, __typeof__, auto type inference, u8 character literals */
#include <stdio.h>
int f(int x) { return x * 2; }
int main(void) {
    const int ci = 5;
    int arr[4] = {1, 2, 3, 4};
    typeof(ci) a = 1;             /* const int */
    typeof_unqual(ci) b = 2; b++;
    __typeof__(arr) arr2 = {9};   /* int[4] */
    typeof(int *) p = arr;
    typeof(f) *fp = f;
    auto x = 1.5;                 /* double */
    auto y = arr;                 /* int * */
    auto z = ci;                  /* int */
    z++;
    auto s = "hi";
    unsigned char u = u8'A';
    printf("%d %d %zu %d %d %zu %zu %d %s %d %zu %zu\n", a, b, sizeof arr2, *p, fp(3), sizeof x, sizeof y, z, s, u, sizeof u8'A', sizeof(typeof(u8"ab")));
    return 0;
}
