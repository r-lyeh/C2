/* %ld with a 64-bit long: on Windows the compiler turns it into %lld in literal formats */
#include <stdio.h>
int main(void){ long a = -5; unsigned long b = 18446744073709551615ul; long r = 0; char buf[64];
 printf("%ld %lu %lx %5ld|%%ld %lld %lf\n", a, b, b, a, 7ll, 1.5);
 snprintf(buf, sizeof buf, "%ld", a); puts(buf);
 sscanf("123456789012", "%ld", &r); printf("%ld\n", r); return 0; }
