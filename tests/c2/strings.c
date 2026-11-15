#define N 3
int printf(const char *fmt, ...);
int strcmp(const char *a, const char *b);
int count(char *s, char c) { int n = 0; while (*s) { if (*s == c) n++; s++; } return n; }
int main() {
    int x = 5;
    int sz = sizeof(x++) + sizeof x;        /* no side effect */
    char *p = "ab" "c" "N";                 /* concat, no macro expansion inside */
    char *e = "";
    printf("%s %c %d [%s] x=%d sz=%d\n", p, p[1], count("banana", 'a'), e, x, sz);
    printf("tab:\t|bell-free octal:\0011|\n");
    printf("%.3f %d %s\n", 2.5, -7, "lit");
    int r = 0;
    for (int i = 0; i < N; i += strcmp("a", "a") + 1) r += count("xx", 'x');
    printf("r=%d cmp=%d\n", r, strcmp(p, "abcN") == 0 && !strcmp(e, ""));
    return r + x + count(p, 'N');
}
