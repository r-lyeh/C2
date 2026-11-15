/* 12-byte structs by value (Win64: passed and returned through hidden pointers), as in demo.c */
#include <stdio.h>
typedef struct { float x, y, z; } Vec3;
Vec3 vec3_add(Vec3 a, Vec3 b) { Vec3 r = {a.x + b.x, a.y + b.y, a.z + b.z}; return r; }
void draw(const char *what, Vec3 at) { printf("draw %s at (%g, %g, %g)\n", what, at.x, at.y, at.z); }
int score = 10;
static Vec3 pos = {1, 2, 3};
Vec3 update(float dt) {
    Vec3 vel = {dt, 0, 0};
    pos = vec3_add(pos, vel);
    draw("player", pos);
    score += 1;
    return pos;
}
int main(void) {
    Vec3 p = update(0.5f);
    update(0.5f);
    printf("returned (%g, %g, %g), score %d\n", p.x, p.y, p.z, score);
    return 0;
}
