#include <stdio.h>
int main(void) {
    printf("%s %s %d\n", "\u137a", "\U0001F600!", (int)'\u0041');
    return '\u137a' == 0x137a ? 0 : 1;
}
