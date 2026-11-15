#include <stdio.h>
#include <stdint.h>

union Num { int32_t i; float f; uint8_t bytes[4]; };
typedef struct {
    int kind;
    union { long l; double d; const char *s; };    /* anonymous union */
} Value;

typedef union { struct { uint16_t lo, hi; }; uint32_t whole; } Word;

int main(void) {
    union Num n;
    n.f = 1.0f;
    printf("bits %08x byte3 %02x size %d\n", n.i, n.bytes[3], (int)sizeof n);
    Value vals[3] = {{0, {42}}, {1}, {2}};
    vals[1].d = 2.5;
    vals[2].s = "text";
    for (int i = 0; i < 3; i++) {
        switch (vals[i].kind) {
        case 0: printf("long %lld\n", vals[i].l); break;
        case 1: printf("double %.1f\n", vals[i].d); break;
        default: printf("string %s\n", vals[i].s);
        }
    }
    Word w;
    w.whole = 0x12345678;
    union Num m = {7};
    printf("lo %x hi %x sizeof Value %d init %d\n", w.lo, w.hi, (int)sizeof(Value), m.i);
    return (int)sizeof(Value) + m.i;
}
