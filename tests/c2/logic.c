int calls = 0;
int bump(int v) { calls = calls + 1; return v; }
int main() {
    int r = 0;
    if (bump(0) && bump(1)) r = r + 100;      /* short-circuit: 1 call */
    if (bump(1) || bump(1)) r = r + 1;        /* 1 call */
    if (!bump(0)) r = r + 2;                  /* 1 call */
    if (bump(1) && !bump(0) && bump(2)) r = r + 4;  /* 3 calls */
    int x = (bump(0) || bump(0)) + (1 && 2) + !5 + !0.0;  /* 2 calls; 0+1+0+1 */
    double z = 0.0;
    if (!z && 0.5) r = r + 8;
    if (z || 0.0) r = r + 1000;
    return r + x * 16 + calls * 64;           /* 15 + 32 + 8*64 = 559 */
}
