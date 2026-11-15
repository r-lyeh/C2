/* .c file: plain C, the C² extensions are off */
#if !defined(__C2__) || defined(__C2_EXT__)
#error "__C2__ must be defined, __C2_EXT__ must not"
#endif
int main(void) { return 0; }
