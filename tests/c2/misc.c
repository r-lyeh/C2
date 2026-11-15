#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <limits.h>

enum Color { RED, GREEN = 5, BLUE, LAST = BLUE * 2 };
typedef enum { OFF, ON } Switch;

int find(int *a, int n, int key) {
    int i = 0;
again:
    if (i >= n) goto notfound;
    if (a[i] == key) goto found;
    i++;
    goto again;
found:
    return i;
notfound:
    return -1;
}

int main(void) {
    enum Color c = BLUE;
    Switch s = ON;
    int arr[LAST];
    printf("enum %d %d %d %d %d\n", RED, GREEN, c, LAST, s);
    int data[] = {4, 8, 15, 16, 23, 42};
    printf("goto %d %d\n", find(data, 6, 23), find(data, 6, 7));
    printf("chars %d %d %d %d %d\n", 'u', '\x20', '\n', '\0', '\101');
    printf("utf8 [%s] %d\n", "é㝈", (int)strlen("㝈"));
    printf("limits %d %d %u %lld\n", INT_MAX, INT_MIN, UINT_MAX, LONG_MAX);
    printf("math %.4f %.4f %.4f %.1f\n", sqrt(2.0), sin(M_PI / 2), pow(2, 10), floor(-1.5));
    char buf[64];
    sprintf(buf, "%s-%d", "item", 42);
    char copy[64];
    strncpy(copy, buf, 4);
    copy[4] = 0;
    printf("str %s %s %d %d\n", buf, copy, (int)strlen(buf), strcmp("a", "b") < 0);
    int *p = malloc(4 * sizeof(int));
    p[0] = 1;
    p = realloc(p, 100 * sizeof(int));
    p[99] = 2;                          /* realloc keeps the old contents; new bytes are unspecified */
    int kept = p[0] == 1 && p[99] == 2;
    free(p);
    FILE *f = fopen("misc_test.txt", "w");
    fprintf(f, "line %d\n", 7);
    fclose(f);
    f = fopen("misc_test.txt", "r");
    char line[32];
    fgets(line, 32, f);
    fclose(f);
    remove("misc_test.txt");
    printf("realloc kept=%d file=%s", kept, line);
    return c + s;
}
