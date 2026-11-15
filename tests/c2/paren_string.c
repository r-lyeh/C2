// legal C: a string literal in parentheses initializes a char array
#include <stdio.h>
main() {
	char a[7] = ("wat");
	char b[] = ((("hello")));
	struct { char s[4]; int n; } t = { ("ab"), 3 };
	puts(a);
	printf("%s %zu %s %d\n", b, sizeof b, t.s, t.n);
}
