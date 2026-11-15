int x = 1000;
int f(int x) { { int x = 5; x = x + 1; } return x; }
int main() {
    int r = x;                 /* global 1000 */
    int x = 1;
    {
        int x = 2;
        r = r + x;             /* 2 */
        {
            double x = 0.5;
            r = r + (int)(x * 8);  /* 4 */
        }
        r = r + x;             /* 2 */
    }
    for (int x = 10; x < 12; x = x + 1) r = r + x;   /* 21 */
    for (int x = 0; x < 1; x = x + 1) { int y = 3; r = r + y; }
    int *p = &x;
    { int x = 77; *p = *p + x; }   /* outer x = 78 */
    r = r + x + f(9);          /* 78 + 9 */
    switch (x) { case 78: { int x = 1; r = r + x; } case 2: r = r + 1; break; default: r = 0; }
    return r - 1000;           /* 2+4+2+21+3+78+9+1+1 = 121 */
}
