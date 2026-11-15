#include <stdio.h>
int main(void){ int x = 5, *p = &x; int c = 0; printf("%d\n", *(c ? NULL : p)); printf("%d\n", *(c ? (void*)0 : p)); printf("%d\n", *(c ? 0 : p)); return 0; }
