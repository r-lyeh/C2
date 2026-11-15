#include <stdio.h>
#include <string.h>
static int sum(int n, int a[n]) { int s = 0; for (int i = 0; i < n; i++) s += a[i]; return s; }
static double trace(int n, double m[][4]) { double t = 0; for (int i = 0; i < n && i < 4; i++) t += m[i][i]; return t; }
int main(void) {
    int total = 0;
    for (int n = 1; n <= 100000; n++) {          /* stack must be released every iteration */
        int a[n % 50 + 1];
        a[0] = n;
        total += a[0] % 3 + (int)(sizeof a / sizeof a[0]) - (n % 50 + 1);
    }
    int n = 5;
    int v[n];
    for (int i = 0; i < n; i++) v[i] = i * i;
    double m[n][4];
    memset(m, 0, sizeof m);
    for (int i = 0; i < 4; i++) m[i][i] = i + 0.5;
    char buf[n * 3];
    strcpy(buf, "vla-ok");
    printf("%d %d %zu %g %s %zu\n", total, sum(n, v), sizeof v, trace(n, m), buf, sizeof m);
    {
        long z[n + 2];
        memset(z, 0, sizeof z);        /* .c: a VLA has no value until set (zeroinit.c2 tests .c2) */
        int zero = 1;
        for (int i = 0; i < n + 2; i++) zero &= z[i] == 0;
        printf("zeroed %d\n", zero);
    }
    return 0;
}
