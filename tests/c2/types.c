typedef struct Vec { double x; double y; } Vec;
struct Rec { char tag; int n; Vec v; char name[5]; struct Rec *next; };
double gscale = 2.5;
char gch = 'A';
int garr[4];
double dot(Vec *a, Vec *b) { return a->x * b->x + a->y * b->y; }
char upper(char c) { if (c >= 'a' && c <= 'z') return c - 32; return c; }
double half(int n) { return n / 2.0; }
int trunc_it(double d) { return d; }
void fill(int *p, int n) { for (int i = 0; i < n; i = i + 1) *(p + i) = i * 3; }
int addr_param(int k) { int *q = &k; *q = *q + 5; return k; }
int main() {
    Vec a; Vec b;
    a.x = 1.5; a.y = 2.0; b.x = 4.0; b.y = 0.25;
    double d = dot(&a, &b) * gscale;          /* (6 + 0.5) * 2.5 = 16.25 */
    char c = 300;                              /* wraps to 44 */
    char s[5];
    s[0] = 'h'; s[1] = 'i'; s[2] = 0;
    struct Rec r1; struct Rec r2;
    r1.tag = upper(s[1]);                      /* 'I' = 73 */
    r1.n = 7; r1.v.x = 3.0; r1.v.y = -1.0;
    r1.name[0] = gch; r1.name[4] = 'z';
    r1.next = &r2; r2.n = 11; r2.next = (struct Rec*)0;
    fill(garr, 4);                             /* 0 3 6 9 */
    int *pe = &garr[3];
    int diff = pe - garr;                      /* 3 */
    double h = half(7);                        /* 3.5 */
    int t = trunc_it(-h);                      /* -3 */
    int total = 0;
    struct Rec *it = &r1;
    while (it) { total = total + it->n; it = it->next; }   /* 18 */
    return (int)d + c + r1.tag + r1.name[0] + r1.name[4] + garr[3] + diff + t + total
           + (int)(h * 2) + (int)r1.v.x + addr_param(1) + (s[1] == 'i') - (r1.v.y < 0.0);
}
