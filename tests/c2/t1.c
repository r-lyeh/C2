#define NULL ((void*)0)
#define N 5   // comment
int g = -3;
struct Node { int val; struct Node *next; };
int later(int x);
void bump(int *p) { *p = *p + 1; }
int sum_list(struct Node *n) {
    int s = 0;
    while (n != NULL) { s = s + n->val; n = n->next; }
    return s;
}
int main() {
    int r = 0;
    int k;
    for (k = 0; k < N; k = k + 1) {
        switch (k) {
            case 0: r = r + 1;           /* fallthrough */
            case 1: r = r + 10; break;
            case 3: continue;
            default: r = r + 100;
        }
        r = r + 1000;
    }
    g = g * 2 - 1;                     // -7
    int x = 7;
    int *px = &x;
    int **ppx = &px;
    **ppx = 9;
    bump(&x);                           // 10
    int a[4];
    int *pa = a;
    for (int i = 0; i < 4; i = i + 1) pa[i] = i * i;
    struct Node n1; struct Node n2;
    n1.val = 5; n1.next = &n2;
    n2.val = 6; n2.next = NULL;
    return r + g + x + a[3] + later(3) % 4 + -2 + sum_list(&n1) + 17 / 5;
}
int later(int x) { return x * 11; }
