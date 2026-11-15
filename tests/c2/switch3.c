#include <stdio.h>
/* dense switches become jump tables; sparse ones stay a chain of compares */
static int dense(int x) {
    switch (x) {
    case -2: return 100;
    case -1: return 101;
    case 0: return 102;
    case 1:
    case 2: return 103;
    case 4: return 104;
    case 5: x += 10;              /* falls through */
    case 6: return 105 + x;
    default: return -1;
    }
}
static int sparse(long x) {
    switch (x) { case 1: return 1; case 1000: return 2; case 1000000: return 3; case -7: return 4; } return 0;
}
static const char *name(unsigned char c) {
    switch (c) {
    case 'a': case 'e': case 'i': case 'o': case 'u': return "vowel";
    case 'b': case 'c': case 'd': case 'f': case 'g': return "consonant";
    }
    return "other";
}
int main(void) {
    int sum = 0;
    for (int i = -4; i < 9; i++) { printf("%d:%d ", i, dense(i)); sum += dense(i); }
    printf("\n%d %d %d %d %d\n", sparse(1), sparse(1000), sparse(1000000), sparse(-7), sparse(5));
    printf("%s %s %s %s\n", name('a'), name('d'), name('z'), name(200));
    return sum & 0xff;
}
