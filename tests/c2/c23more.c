/* C23: [[attributes]] (ignored), constexpr, alignas / alignof / thread_local keywords */
#include <stdio.h>
[[nodiscard]] static int f(int x) { return x + 1; }
constexpr int N = 4;
constexpr double PI = 3.25;
struct [[deprecated("old ]] one")]] S { alignas(16) int a; int b; };
thread_local int tl = 7;
int main(void) {
    [[maybe_unused]] int unused;
    constexpr long M = N * 2, K = M + 1;
    int arr[M];
    static int st[N];
    switch (f(3)) { case N: puts("case N"); break; default: puts("no"); }
    printf("%zu %zu %ld %ld %g %zu %d %zu\n", sizeof arr / sizeof arr[0], sizeof st / sizeof st[0], M, K, PI, alignof(struct S), tl, sizeof(struct S));
    return 0;
}
