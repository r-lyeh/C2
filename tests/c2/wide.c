/* C95/C99 gaps: wide literals and wchar_t, digraphs, _Pragma, K&R definitions */
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

wchar_t gs[] = L"glob\x41";
const wchar_t *gp = L"ptr" L"cat";

/* K&R definitions: types after the declarator, int by default, float arrives as double */
int max(a, b)
register int a, b;
{
    return a > b ? a : b;
}
static scale(n, f, rows)
float f;
int rows[][3];
{
    return (int)(n * f) + rows[1][2] + (int)sizeof rows;
}
int knr_decl();          /* old-style declaration: no parameter information */

_Pragma("once")
#define DO_PRAGMA(x) _Pragma(#x)
DO_PRAGMA(GCC diagnostic ignored "-Wall")

%:define CAT(a, b) a %:%: b
int main(void) <%
    wchar_t s[] = L"héllo €";
    wchar_t c = L'€', u = L'é', x = L'\x41';
    printf("wide %d %d %d %d %d %d %d\n", (int)(sizeof s / sizeof s[0]), (int)s[1], (int)c, (int)u, (int)x,
           (int)wcslen(s), sizeof(wchar_t) == sizeof(L'x') || sizeof(wchar_t) == 2);
    const wchar_t *p = "a" L"b";
    printf("concat %d %d %d %d\n", (int)p[0], (int)p[1], (int)p[2], (int)(sizeof L"xyz" / sizeof(wchar_t)));
    printf("%ls %ls %d\n", gs, gp, (int)wcslen(gp));
    char mb[32];
    wcstombs(mb, L"abc", sizeof mb);
    wchar_t back[8];
    int nb = (int)mbstowcs(back, "xyz", 8);
    printf("%s %d %d\n", mb, nb, (int)back[2]);
    struct { int n; wchar_t name[6]; } rec = {1, L"bob"};
    printf("%ls %d %d %d\n", rec.name, (int)rec.name[3], wcscmp(rec.name, L"bob"), wcscmp(rec.name, L"bobby") < 0);
    int CAT(v, 1)<:2:> = <%4, 5%>;
    printf("digraphs %d %d\n", v1<:0:>, v1<:1:>);
    int rows[2][3] = {{0}, {0, 0, 7}};
    printf("knr %d %d\n", max(3, -5), scale(4, 2.5f, rows));
    return 0;
%>
