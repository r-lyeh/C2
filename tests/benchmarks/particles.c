/* local struct temporaries: positions/velocities in structs, member access in a loop */
#include <stdio.h>
#include <stdlib.h>
typedef struct { double x, y; } P;
typedef struct { double x, y, vx, vy; } Particle;
int main(int argc, char **argv) {
    int n = argc > 1 ? atoi(argv[1]) : 3000;
    Particle *ps = calloc(1000, sizeof *ps);
    for (int i = 0; i < 1000; i++) { ps[i].vx = (i % 7) * 0.1; ps[i].vy = (i % 5) * 0.2; }
    double total = 0;
    for (int step = 0; step < n; step++) {
        for (int i = 0; i < 1000; i++) {
            P p, v;                                     /* local struct temporaries */
            p.x = ps[i].x; p.y = ps[i].y; v.x = ps[i].vx; v.y = ps[i].vy;
            v.y -= 0.001;
            p.x += v.x * 0.01; p.y += v.y * 0.01;
            if (p.y < 0) { p.y = -p.y; v.y = -v.y * 0.9; }
            ps[i].x = p.x; ps[i].y = p.y; ps[i].vx = v.x; ps[i].vy = v.y;
            total += p.x + p.y;
        }
    }
    printf("%.4f\n", total);
    free(ps);
    return 0;
}
