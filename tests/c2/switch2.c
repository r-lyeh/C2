#include <stdio.h>

int duff(char *to, const char *from, int count) {
    int n = (count + 7) / 8;
    switch (count % 8) {
    case 0: do { *to++ = *from++;
    case 7:      *to++ = *from++;
    case 6:      *to++ = *from++;
    case 5:      *to++ = *from++;
    case 4:      *to++ = *from++;
    case 3:      *to++ = *from++;
    case 2:      *to++ = *from++;
    case 1:      *to++ = *from++;
            } while (--n > 0);
    }
    return count;
}

int classify(int x) {
    switch (x) {
    default: return -1;             /* default first */
    case 1: case 2: return 12;
    case 'a': return 'a';
    case -5: return 5;
    }
}

int main(void) {
    char out[32];
    duff(out, "hello, duff's device!", 22);
    int s = 0;
    for (int i = 0; i < 5; i++)
        switch (i) {
        case 1: continue;           /* continue goes to the loop */
        case 3: s += 100;
        }
    printf("%s %d %d %d %d %d\n", out, classify(2), classify('a'), classify(-5), classify(9), s);
    return s;
}
