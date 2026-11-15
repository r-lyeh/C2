#include <stdio.h>
#include <stdarg.h>
#include <string.h>

int sum(int n, ...) {
    va_list ap;
    va_start(ap, n);
    int s = 0;
    for (int i = 0; i < n; i++) s += va_arg(ap, int);
    va_end(ap);
    return s;
}

double avg(int n, ...) {
    va_list ap;
    va_start(ap, n);
    double s = 0;
    for (int i = 0; i < n; i++) s += va_arg(ap, double);
    va_end(ap);
    return n ? s / n : 0;
}

/* mixed types, described by a format */
void show(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    for (const char *p = fmt; *p; p++) {
        switch (*p) {
        case 'i': printf("i:%d ", va_arg(ap, int)); break;
        case 'l': printf("l:%lld ", (long long)va_arg(ap, long)); break;
        case 'd': printf("d:%.2f ", va_arg(ap, double)); break;
        case 's': printf("s:%s ", va_arg(ap, char *)); break;
        case 'c': printf("c:%c ", (char)va_arg(ap, int)); break;
        }
    }
    va_end(ap);
    printf("\n");
}

/* forwarding to the C library */
int logf_(char *buf, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsprintf(buf, fmt, ap);
    va_end(ap);
    return n;
}

void log_twice(const char *fmt, ...) {
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    vprintf(fmt, ap);
    vprintf(fmt, ap2);
    va_end(ap2);
    va_end(ap);
}

int count_until_zero(int first, ...) {
    va_list ap;
    va_start(ap, first);
    int n = 0;
    for (int v = first; v != 0; v = va_arg(ap, int)) n++;
    va_end(ap);
    return n;
}

int many(int a, int b, int c, int d, int e, int f, int g, ...) {   /* varargs on the stack */
    va_list ap;
    va_start(ap, g);
    int x = va_arg(ap, int), y = va_arg(ap, int);
    double z = va_arg(ap, double);
    va_end(ap);
    return a + b + c + d + e + f + g + x + y + (int)z;
}

int main(void) {
    char buf[64];
    int n = logf_(buf, "%s=%d (%.1f)", "x", 42, 2.5);
    printf("sum %d %d avg %.3f %.1f buf [%s] %d\n", sum(3, 1, 2, 3), sum(0), avg(3, 1.0, 2.0, 4.5), avg(0), buf, n);
    show("ilds c", 7, 1L << 40, 3.14159, "str", 'z');
    log_twice("twice %d %s\n", 5, "ok");
    printf("until %d many %d\n", count_until_zero(5, 4, 3, 0), many(1, 2, 3, 4, 5, 6, 7, 8, 9, 10.5));
    return sum(4, 10, 20, 30, 40);
}
