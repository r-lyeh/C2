int printf(const char *fmt, ...);
int puts(const char *s);
void *malloc(long n);
void free(void *p);
int strlen(const char *s);
double sqrt(double x);

char *greeting = "Hello, \"MIR\"!";

struct P { int x; double y; char tag; };

int main() {
    printf("%s\n", greeting);
    int n = 0;
    for (int i = 0; i < 10; i++) n += i;            /* 45 */
    int k = 10;
    int a = k++ + ++k;                              /* 10 + 12 = 22, k = 12 */
    k -= 2; k *= 3; k /= 4; k %= 4;                 /* 10 -> 30 -> 7 -> 3 */
    double d = 1.5;
    d += 1; d *= 2;                                 /* 5.0 */
    d++;                                            /* 6.0 */
    int *buf = malloc(sizeof(int) * 4);
    int *q = buf;
    *q++ = 7; *q++ = 8; *q = 9;
    char s[6];
    s[0] = 'M'; s[1] = 'I'; s[2] = 'R'; s[3] = 0;
    printf("n=%d a=%d k=%d d=%.2f sqrt2=%.4f buf=%d,%d,%d len=%d s=%s\n",
           n, a, k, d, sqrt(2.0), buf[0], buf[1], buf[2], strlen("tab\there"), s);
    printf("sizeof: int=%d char=%d double=%d ptr=%d P=%d s=%d buf[0]=%d\n",
           sizeof(int), sizeof(char), sizeof(double), sizeof(char*), sizeof(struct P), sizeof s, sizeof buf[0]);
    puts("done");
    free(buf);
    return n + a + k + (int)d;                      /* 45+22+3+6 = 76 */
}
