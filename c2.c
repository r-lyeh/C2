/*
 * C² — a C-subset compiler targeting MIR (github.com/vnmakarov/mir)
 *
 *   c2 file.c        build MIR in memory with the MIR C API, JIT it, run main() (exit code = result)
 *   c2 -S file.c     print MIR textual IR
 *   c2 -T file.c     emit MIR text, feed it to MIR_scan_string, JIT and run   (cross-checks -S)
 *   c2 -A file.c     build with the C API and print it back with MIR_output   (cross-checks API)
 *   (also -E, -i, -C, -Crt, -O0..-O3, -lazy, -I, -D: see README.md)
 *   file.c is compiled as C; file.c2 also gets the C² extensions (and __C2_EXT__ defined).
 *
 * Also a library (c2.h): build this file with C2_NO_MAIN and call C2_compile / C2_get.
 *
 * Build (MIR's sources are in mir/; no build system needed):
 *   cl /O2 /DNDEBUG /MT c2.c mir\mir.c mir\mir-gen.c                         (Windows: cl or clang-cl)
 *   cc -O2 -DNDEBUG -fno-tree-sra c2.c mir/mir.c mir/mir-gen.c -lm -ldl -lpthread -o c2
 *   NDEBUG: MIR's internal checks are for MIR's developers, and one trips on valid code.
 *   -fno-tree-sra: gcc miscompiles mir.c without it (MIR's own build uses it); clang doesn't need it.
 *   -DNO_MIR builds c2.c alone, without the JIT: only -S and -E work.
 *
 * One code generator, two back ends: every instruction goes through EMIT(), which either formats
 * MIR text or calls MIR_new_insn_arr()/MIR_append_insn(), depending on the global `use_api`.
 * Opcodes are listed once in INSN_LIST (an X-macro): enum, text mnemonics and MIR codes.
 *
 * What it supports (docs/README.md lists it per C standard, with what is missing):
 *   types     char short int long long-long (signed/unsigned), _Bool, float, double, void, enum,
 *             pointers, function pointers, multi-dimensional arrays, VLAs (outermost dimension),
 *             struct/union (by value, nested, anonymous members, bit-fields, flexible array member),
 *             typedef, const/volatile/restrict, static/extern/register/inline storage
 *   C11       _Static_assert, _Generic, _Alignof, _Alignas (struct members), _Noreturn,
 *             _Thread_local (one copy), compound literals, designated initializers
 *   code      every C operator with C precedence, if/switch (jump tables)/for/while/do, break,
 *             continue, goto, variadic functions (stdarg.h), static locals, recursion,
 *             K&R function definitions
 *   literals  decimal/hex/octal integers with suffixes, decimal and hex floats, string concatenation,
 *             escapes including \u and \U (UTF-8 encoded), wide L"..." / L'x' (wchar_t)
 *   cpp       #define (function-like, variadic, # and ##), #undef, #if/#ifdef/#ifndef/#elif/#else
 *             with defined(), #include "file" and -I dirs, #error/#warning, #line, #pragma once,
 *             _Pragma, digraphs, .c2: file-scoped macros in .h2 headers (#export),
 *             C23's #embed, __has_include(), __has_embed() and digit separators (1'000'000),
 *             __FILE__ __LINE__ __DATE__ __TIME__ __COUNTER__, -D
 *   headers   built in, mapped onto the host C runtime: assert ctype dlfcn errno float inttypes
 *             limits locale math signal stdalign stdarg stdbool stddef stdint stdio stdlib
 *             stdnoreturn string time wchar
 *   ABI       platform calling convention (SysV x86-64, Win64), structs by value both ways, so
 *             scripts and host call each other through plain function pointers
 *   runs as   JIT (-O0..-O3, lazy per-function generation), MIR interpreter (-i), or C output (-C)
 * Differences: long is 64-bit everywhere; .c2 locals start zeroed (README.md lists the rest).
 */
#if !defined(_WIN32) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE   /* RTLD_DEFAULT */
#endif
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include <math.h>
#include <ctype.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <limits.h>
#include <time.h>
#include <setjmp.h>
#include <errno.h>
#include <signal.h>
#include <locale.h>
#ifdef _WIN32                     /* dynamic libraries (C2_load, <dlfcn.h> for scripts); not <windows.h>: */
__declspec(dllimport) void *__stdcall LoadLibraryA(const char *);           /* it defines TokenType */
__declspec(dllimport) void *__stdcall GetProcAddress(void *, const char *);
__declspec(dllimport) int __stdcall FreeLibrary(void *);
#else
#include <dlfcn.h>
#endif
#include "c2.h"

#ifndef NO_MIR
#include "mir/mir.h"
#include "mir/mir-gen.h"
#endif

/* ============================================================ compiler memory
 * Everything the compiler allocates is linked in one list, so a compile -- finished, or abandoned
 * half-way by an error -- releases all of it (cc_free_all).  The macros cover the compiler code
 * below; the library and runtime parts at the end use the plain C functions. */
typedef struct MHdr { struct MHdr *prev, *next; } MHdr;   /* 16 bytes: keeps malloc's alignment */
static MHdr cc_heap = {&cc_heap, &cc_heap};
static void *cc_link(MHdr *h) {
    if (!h) { fputs("c2: out of memory\n", stderr); abort(); }
    h->prev = &cc_heap;
    h->next = cc_heap.next;
    cc_heap.next->prev = h;
    cc_heap.next = h;
    return h + 1;
}
static void cc_unlink(MHdr *h) { h->prev->next = h->next; h->next->prev = h->prev; }
static void *cc_malloc(size_t n) { return cc_link((MHdr *)(malloc)(sizeof(MHdr) + n)); }
static void *cc_calloc(size_t n, size_t sz) { return cc_link((MHdr *)(calloc)(1, sizeof(MHdr) + n * sz)); }
static void *cc_realloc(void *p, size_t n) {
    if (!p) return cc_malloc(n);
    MHdr *h = (MHdr *)p - 1;
    cc_unlink(h);
    return cc_link((MHdr *)(realloc)(h, sizeof(MHdr) + n));
}
static void cc_free(void *p) {
    if (!p) return;
    MHdr *h = (MHdr *)p - 1;
    cc_unlink(h);
    (free)(h);
}
static void cc_free_all(void) {
    while (cc_heap.next != &cc_heap) {
        MHdr *h = cc_heap.next;
        cc_unlink(h);
        (free)(h);
    }
}
#define malloc(n) cc_malloc(n)
#define calloc(n, sz) cc_calloc(n, sz)
#define realloc(p, n) cc_realloc(p, n)
#define free(p) cc_free(p)

/* ============================================================ diagnostics and errors
 * Messages collect in diag_buf (the library returns them from C2_errors); the command line
 * also echoes them to stderr.  error() abandons the compile: longjmp to the compile driver, or
 * exit when there is none. */
static char *diag_buf;
static size_t diag_len, diag_cap;
static int diag_echo, diag_mute;
static jmp_buf *err_jmp;

static void diag(const char *fmt, ...) {
    char msg[2048];
    va_list a;
    va_start(a, fmt);
    vsnprintf(msg, sizeof msg, fmt, a);
    va_end(a);
    if (diag_mute) return;
    if (diag_echo) { fflush(stdout); fputs(msg, stderr); }
    size_t n = strlen(msg);
    if (diag_len + n + 1 > diag_cap) {
        size_t cap = (diag_len + n + 1) * 2;
        char *p = (char *)(realloc)(diag_buf, cap);
        if (!p) return;
        diag_buf = p;
        diag_cap = cap;
    }
    memcpy(diag_buf + diag_len, msg, n + 1);
    diag_len += n;
}

static int cur_line = 1;
static char cur_file[1024] = "?";   /* set by the preprocessor's # N "file" markers */

#if defined(_MSC_VER)
#define NORETURN __declspec(noreturn)
#else
#define NORETURN __attribute__((noreturn))
#endif
static NORETURN void fatal(void) {
    if (err_jmp) longjmp(*err_jmp, 1);
    exit(1);
}

static NORETURN void error(const char *fmt, ...) {
    char msg[1024];
    va_list a;
    va_start(a, fmt);
    vsnprintf(msg, sizeof msg, fmt, a);
    va_end(a);
    if (cur_line > 0) diag("%s:%d: error: %s\n", cur_file, cur_line, msg);
    else diag("error: %s\n", msg);
    fatal();
}

/* ============================================================ growable text buffer */
typedef struct { char *p; size_t len, cap; } Buf;

static void buf_append(Buf *b, const char *data, size_t len) {
    if (b->len + len + 1 >= b->cap) {
        b->cap = b->len + len + 8192;
        b->p = realloc(b->p, b->cap);
    }
    memcpy(b->p + b->len, data, len);
    b->len += len;
    b->p[b->len] = '\0';
}

static void buf_emit(Buf *b, const char *fmt, ...) {
    char tmp[1024];
    va_list a;
    va_start(a, fmt);
    vsnprintf(tmp, sizeof tmp, fmt, a);
    va_end(a);
    buf_append(b, tmp, strlen(tmp));
}

static void buf_clear(Buf *b) { b->len = 0; if (b->p) b->p[0] = '\0'; }

/* ============================================================ lexer */
typedef enum {
    TOKEN_INT, TOKEN_CHAR, TOKEN_SHORT, TOKEN_LONG, TOKEN_UNSIGNED, TOKEN_SIGNED, TOKEN_FLOAT,
    TOKEN_DOUBLE, TOKEN_VOID, TOKEN_STRUCT, TOKEN_UNION, TOKEN_ENUM, TOKEN_STATIC, TOKEN_EXTERN, TOKEN_TYPEDEF, TOKEN_CONST, TOKEN_NULL,
    TOKEN_BOOL, TOKEN_ALIGNAS, TOKEN_ALIGNOF, TOKEN_STATIC_ASSERT, TOKEN_GENERIC,
    TOKEN_TYPEOF, TOKEN_TYPEOF_UNQUAL, TOKEN_AUTO, TOKEN_CONSTEXPR, TOKEN_REGISTER,
    TOKEN_LPAREN, TOKEN_RPAREN, TOKEN_LBRACE, TOKEN_RBRACE, TOKEN_LBRACKET, TOKEN_RBRACKET,
    TOKEN_DOT, TOKEN_ARROW, TOKEN_RETURN, TOKEN_IF, TOKEN_ELSE, TOKEN_WHILE, TOKEN_FOR, TOKEN_DO,
    TOKEN_SWITCH, TOKEN_CASE, TOKEN_DEFAULT, TOKEN_BREAK, TOKEN_CONTINUE, TOKEN_GOTO, TOKEN_SIZEOF,
    TOKEN_COLON, TOKEN_QUESTION, TOKEN_COMMA, TOKEN_SEMI, TOKEN_ELLIPSIS,
    TOKEN_NUM, TOKEN_FLOAT_NUM, TOKEN_IDENT, TOKEN_STR,
    TOKEN_PLUS, TOKEN_MINUS, TOKEN_ASTERISK, TOKEN_SLASH, TOKEN_PERCENT, TOKEN_AMPERSAND,
    TOKEN_PIPE, TOKEN_CARET, TOKEN_TILDE, TOKEN_SHL, TOKEN_SHR,
    TOKEN_NOT, TOKEN_ANDAND, TOKEN_OROR, TOKEN_INC, TOKEN_DEC,
    TOKEN_EQ, TOKEN_NE, TOKEN_LT, TOKEN_LE, TOKEN_GT, TOKEN_GE,
    TOKEN_ASSIGN,   /* compound assignments follow in this order: */
    TOKEN_PLUS_ASSIGN, TOKEN_MINUS_ASSIGN, TOKEN_MUL_ASSIGN, TOKEN_DIV_ASSIGN, TOKEN_MOD_ASSIGN,
    TOKEN_AND_ASSIGN, TOKEN_OR_ASSIGN, TOKEN_XOR_ASSIGN, TOKEN_SHL_ASSIGN, TOKEN_SHR_ASSIGN,
    TOKEN_EOF
} TokenType;

enum { NUM_UNSIGNED = 1, NUM_LONG = 2, NUM_FLOAT32 = 4, NUM_BOOL = 8, NUM_UCHAR = 16 };

typedef struct {
    TokenType type;
    long long val;
    double float_val;
    int num_flags;    /* NUM_*: literal suffixes */
    char text[64];
    char *str;        /* TOKEN_STR: decoded bytes (escapes resolved, adjacent literals joined) */
    int str_len;      /* in bytes, without the terminator */
    int wide;         /* TOKEN_STR: L"..." (str holds wchar_t units, then a whole zero unit) */
} Token;

/* wchar_t as in the host C library: 16-bit UTF-16 units on Windows, 32-bit code points elsewhere */
#ifdef _WIN32
#define WCHAR_SIZE 2
#else
#define WCHAR_SIZE 4
#endif

static const char *src;
static Token current_token;

/* 1 while compiling a .c2 file: the C² extensions are on (a .c file is plain C).  Also defines
   __C2_EXT__ for the script, so shared headers can test for them. */
static int c2_ext;

static void utf8_append(Buf *b, long cp) {
    char u[4];
    int n;
    if (cp < 0x80) { u[0] = (char)cp; n = 1; }
    else if (cp < 0x800) { u[0] = (char)(0xC0 | (cp >> 6)); u[1] = (char)(0x80 | (cp & 0x3F)); n = 2; }
    else if (cp < 0x10000) {
        u[0] = (char)(0xE0 | (cp >> 12)); u[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        u[2] = (char)(0x80 | (cp & 0x3F)); n = 3;
    } else {
        u[0] = (char)(0xF0 | (cp >> 18)); u[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        u[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); u[3] = (char)(0x80 | (cp & 0x3F)); n = 4;
    }
    buf_append(b, u, n);
}

/* one (possibly escaped) character; *uni is set for \u/\U (a code point, not a byte) */
static long char_escape(const char **ps, int *uni);
static int wide_escapes;   /* in L"..." / L'x': \x and octal escapes are not cut to a byte */

/* one UTF-8 encoded character of the source; *raw is set for a byte that isn't valid UTF-8 */
static long utf8_decode(const char **ps, int *raw) {
    const unsigned char *s = (const unsigned char *)*ps;
    int n = s[0] < 0x80 ? 0 : (s[0] >> 5) == 6 ? 1 : (s[0] >> 4) == 14 ? 2 : (s[0] >> 3) == 30 ? 3 : -1;
    long c = n == 0 ? s[0] : n == 1 ? s[0] & 0x1F : n == 2 ? s[0] & 0x0F : s[0] & 0x07;
    for (int i = 1; i <= n; i++) {
        if ((s[i] & 0xC0) != 0x80) { n = -1; break; }
        c = c << 6 | (s[i] & 0x3F);
    }
    *raw = n < 0;
    if (n < 0) { *ps += 1; return s[0]; }
    *ps += n + 1;
    return c;
}

/* string literal pieces; src is at the opening quote of the first one.  Adjacent literals are
   joined, and one L"..." piece makes the whole literal wide (C11). */
static void lex_string(int wide) {
    long *val = NULL;
    char *raw = NULL;
    int n = 0, cap = 0;
    for (;;) {
        src++;
        wide_escapes = 1;
        while (*src && *src != '"') {
            if (*src == '\n') error("newline in string literal");
            int uni = 0, bad = 0, esc = *src == '\\';
            long c = esc ? char_escape(&src, &uni) : utf8_decode(&src, &bad);
            if (n == cap) { cap = cap ? cap * 2 : 64; val = realloc(val, cap * sizeof *val); raw = realloc(raw, cap); }
            val[n] = c;
            raw[n++] = bad || (esc && !uni);           /* \xff, \377: a byte (a unit, in L"...") */
        }
        wide_escapes = 0;
        if (*src != '"') error("unterminated string literal");
        src++;
        const char *q = src;
        while (isspace((unsigned char)*q)) q++;
        int next_wide = q[0] == 'L' && q[1] == '"';
        if (*q != '"' && !next_wide) break;
        while (src < q) { if (*src == '\n') cur_line++; src++; }
        if (next_wide) { wide = 1; src++; }
    }
    Buf b = {0};
    for (int i = 0; i < n; i++) {
        long c = val[i];
        if (!wide) {
            if (raw[i]) { char ch = (char)c; buf_append(&b, &ch, 1); }
            else utf8_append(&b, c);
        } else if (WCHAR_SIZE == 2 && c > 0xFFFF && !raw[i]) {   /* UTF-16 surrogate pair */
            unsigned short u[2] = {(unsigned short)(0xD800 + ((c - 0x10000) >> 10)), (unsigned short)(0xDC00 + ((c - 0x10000) & 0x3FF))};
            buf_append(&b, (char *)u, 4);
        } else if (WCHAR_SIZE == 2) {
            unsigned short u = (unsigned short)c;
            buf_append(&b, (char *)&u, 2);
        } else {
            int u = (int)c;
            buf_append(&b, (char *)&u, 4);
        }
    }
    free(val);
    free(raw);
    int len = (int)b.len;
    static const char zeros[4] = {0};
    buf_append(&b, zeros, wide ? WCHAR_SIZE : 1);    /* a whole terminator after the data */
    current_token.type = TOKEN_STR;
    current_token.str = b.p;
    current_token.str_len = len;
    current_token.wide = wide;
}

/* character constant; *ps is just past the opening quote, left past the closing one.
   'a' is a char value (a code point for '\u...').  Multi-character constants pack their bytes in
   source order, the first one highest: '12' == 0x3132, '1234' == 0x31323334, so '12' < '1234' on
   every target.  Up to 4 characters (int) in .c files, up to 8 (long long) in .c2 files. */
static long long char_constant(const char **ps, int *is_long) {
    const char *s = *ps;
    int uni, n = 0;
    unsigned long long v = 0;
    long first = 0;
    while (*s && *s != '\'' && *s != '\n') {
        long c = char_escape(&s, &uni);
        if (n == 0) first = uni ? c : (signed char)c;
        v = v << 8 | (unsigned char)c;
        n++;
    }
    if (*s != '\'' || n == 0) error(n ? "unterminated character constant" : "empty character constant");
    *ps = s + 1;
    *is_long = 0;
    if (n == 1) return first;
    if (n > 8 || (n > 4 && !c2_ext))
        error("multi-character constant too long: up to 4 characters in .c files, 8 in .c2 files");
    if (n > 4) { *is_long = 1; return (long long)v; }
    return (int)(unsigned)v;
}

static long char_escape(const char **ps, int *uni) {
    const char *s = *ps;
    long c;
    *uni = 0;
    if (*s != '\\') { c = (unsigned char)*s++; *ps = s; return c; }
    s++;
    switch (*s) {
    case 'n': c = '\n'; break;
    case 't': c = '\t'; break;
    case 'r': c = '\r'; break;
    case 'a': c = '\a'; break;
    case 'b': c = '\b'; break;
    case 'f': c = '\f'; break;
    case 'v': c = '\v'; break;
    case 'e': c = 27; break;
    case '\\': c = '\\'; break;
    case '\'': c = '\''; break;
    case '"': c = '"'; break;
    case '?': c = '?'; break;
    case 'x':                                        /* \x20 */
        c = 0;
        if (!isxdigit((unsigned char)s[1])) error("\\x needs hex digits");
        while (isxdigit((unsigned char)s[1])) {
            s++;
            c = c * 16 + (isdigit((unsigned char)*s) ? *s - '0' : tolower((unsigned char)*s) - 'a' + 10);
        }
        if (!wide_escapes) c &= 0xFF;
        break;
    case 'u':                                        /* \u137a, \U0001F600 */
    case 'U': {
        int n = *s == 'u' ? 4 : 8;
        c = 0;
        for (int k = 0; k < n; k++) {
            if (!isxdigit((unsigned char)s[1])) error("\\%c needs %d hex digits", n == 4 ? 'u' : 'U', n);
            s++;
            c = c * 16 + (isdigit((unsigned char)*s) ? *s - '0' : tolower((unsigned char)*s) - 'a' + 10);
        }
        *uni = 1;
        break;
    }
    default:
        if (*s >= '0' && *s <= '7') {                /* \0, \12, \101: up to 3 octal digits */
            c = *s - '0';
            for (int k = 0; k < 2 && s[1] >= '0' && s[1] <= '7'; k++) c = c * 8 + (*++s - '0');
        } else {
            error("unknown escape sequence '\\%c'", *s);
            c = 0;
        }
    }
    *ps = s + 1;
    return c;
}

/* .c2: 21.twice() is twice(21); 21.0.twice() and 21.f.twice() are twice() of a float.  After a
   decimal number's '.', a name that cannot continue a float literal (e5, e+5, f, L) is a member or
   method name.  p is at the '.'; num at the number's start. */
static int dot_starts_member(const char *num, const char *p) {
    if (!c2_ext || p[0] != '.' || !(isalpha((unsigned char)p[1]) || p[1] == '_')) return 0;
    const char *r = p + 1, *e = r;
    while (isalnum((unsigned char)*e) || *e == '_') e++;
    if (num[0] == '0' && (num[1] == 'x' || num[1] == 'X')) {   /* hex float: 0x1.ap3 needs its p */
        while (r < e && isxdigit((unsigned char)*r)) r++;
        return !(r < e && (*r == 'p' || *r == 'P'));
    }
    if (*r == 'e' || *r == 'E') {                    /* exponent: 21.e5, 21.e+5 */
        r++;
        if (r == e && (*e == '+' || *e == '-') && isdigit((unsigned char)e[1])) return 0;
        if (!isdigit((unsigned char)*r)) return 1;
        while (r < e && isdigit((unsigned char)*r)) r++;
    }
    if (r < e && strchr("fFlL", *r)) r++;           /* suffix: 21.f */
    return r != e;
}

static void lex_number(void) {
    const char *b = src;
    int is_float = 0;
    unsigned long long v = 0;
    if (src[0] == '0' && (src[1] == 'x' || src[1] == 'X')) {
        const char *q = src + 2;
        while (isxdigit((unsigned char)*q)) q++;
        if ((*q == '.' && !dot_starts_member(src, q)) || *q == 'p' || *q == 'P') {      /* hex float: 0x1.8p3 */
            is_float = 1;
            current_token.float_val = strtod(src, (char **)&src);
        } else {
            src += 2;
            if (!isxdigit((unsigned char)*src)) error("bad hex literal");
            v = strtoull(src, (char **)&src, 16);
        }
    } else if (src[0] == '0' && (src[1] == 'b' || src[1] == 'B')) {
        src += 2;
        while (*src == '0' || *src == '1') v = v * 2 + (*src++ - '0');
    } else {
        const char *q = src;
        while (isdigit((unsigned char)*q)) q++;
        if ((*q == '.' && !dot_starts_member(b, q)) || *q == 'e' || *q == 'E') is_float = 1;
        if (is_float) {
            current_token.float_val = strtod(src, (char **)&src);
        } else if (src[0] == '0') {
            v = strtoull(src, (char **)&src, 8);
        } else {
            v = strtoull(src, (char **)&src, 10);
        }
    }
    int flags = 0;
    for (;;) {
        if (*src == 'u' || *src == 'U') { flags |= NUM_UNSIGNED; src++; }
        else if (*src == 'l' || *src == 'L') { flags |= NUM_LONG; src++; }
        else if ((*src == 'f' || *src == 'F') && is_float) { flags |= NUM_FLOAT32; src++; }
        else break;
    }
    if (isalnum((unsigned char)*src) || *src == '_' || (*src == '.' && !dot_starts_member(b, src)))
        error("bad number '%.*s'", (int)(src - b + 1), b);
    current_token.num_flags = flags;
    if (is_float) {
        if (flags & NUM_UNSIGNED) error("bad suffix on floating literal");
        flags &= ~NUM_LONG;                            /* 1.0L: long double is double */
        current_token.type = TOKEN_FLOAT_NUM;
    } else {
        current_token.type = TOKEN_NUM;
        current_token.val = (long long)v;
        /* C: an unsuffixed literal that doesn't fit int is long; one that doesn't fit long is unsigned */
        if (v > 0x7FFFFFFFull && !(flags & NUM_UNSIGNED) && b[0] != '0') flags |= NUM_LONG;
        if (v > 0x7FFFFFFFFFFFFFFFull) flags |= NUM_UNSIGNED | NUM_LONG;
        if (v > 0xFFFFFFFFull) flags |= NUM_LONG;
        if (v > 0x7FFFFFFFull && v <= 0xFFFFFFFFull && b[0] == '0' && !(flags & NUM_LONG)) flags |= NUM_UNSIGNED; /* hex/octal */
        current_token.num_flags = flags;
    }
}

static void next_token(void) {
    for (;;) {
        while (*src && isspace((unsigned char)*src)) { if (*src == '\n') cur_line++; src++; }
        if (*src == '#') {                       /* line marker from the preprocessor: # N "file" */
            const char *p = src + 1;
            while (*p == ' ') p++;
            if (isdigit((unsigned char)*p)) {
                int line = (int)strtol(p, (char **)&p, 10);
                while (*p == ' ') p++;
                if (*p == '"') {
                    const char *q = ++p;
                    while (*q && *q != '"' && *q != '\n') q++;
                    snprintf(cur_file, sizeof cur_file, "%.*s", (int)(q - p), p);
                    p = q;
                }
                while (*p && *p != '\n') p++;
                if (*p) p++;
                src = p;
                cur_line = line;
                continue;
            }
        }
        if (src[0] == '/' && src[1] == '/') { while (*src && *src != '\n') src++; continue; }
        const char *q2 = src + (src[0] == '[');        /* the preprocessor may have spaced '[ [' apart */
        while (*q2 == ' ' || *q2 == '\t') q2++;
        if (src[0] == '[' && *q2 == '[') {             /* C23 [[attribute]]: accepted and ignored */
            int depth = 0;
            for (; *src; src++) {
                if (*src == '"' || *src == '\'') {      /* [[deprecated("x]]")]] */
                    char q = *src++;
                    while (*src && *src != q) { if (*src == '\\' && src[1]) src++; src++; }
                    if (!*src) break;
                } else if (*src == '[') depth++;
                else if (*src == ']' && --depth == 0) { src++; break; }
                else if (*src == '\n') cur_line++;
            }
            continue;
        }
        if (src[0] == '/' && src[1] == '*') {
            src += 2;
            while (*src && !(src[0] == '*' && src[1] == '/')) { if (*src == '\n') cur_line++; src++; }
            if (*src) src += 2;
            continue;
        }
        break;
    }
    if (!*src) { current_token.type = TOKEN_EOF; return; }

    if (isdigit((unsigned char)*src) || (src[0] == '.' && isdigit((unsigned char)src[1]))) { lex_number(); return; }
    if (*src == '"') { lex_string(0); return; }
    if (src[0] == 'L' && (src[1] == '"' || src[1] == '\'')) {   /* wide literals */
        src++;
        if (*src == '"') { lex_string(1); return; }
        src++;
        int uni = 0, bad = 0;
        wide_escapes = 1;
        long c = *src == '\\' ? char_escape(&src, &uni) : *src == '\'' ? (error("empty character constant"), 0) : utf8_decode(&src, &bad);
        wide_escapes = 0;
        if (*src != '\'') error("a wide character constant holds one character");
        src++;
        current_token.type = TOKEN_NUM;
        current_token.num_flags = 0;
        current_token.val = WCHAR_SIZE == 2 ? (long long)(unsigned short)c : (long long)(int)c;
        return;
    }
    if (*src == '\'') {
        src++;
        int is_long;
        current_token.val = char_constant(&src, &is_long);   /* 'é' is a code point; '\xff' is (char)-1 */
        current_token.type = TOKEN_NUM;
        current_token.num_flags = is_long ? NUM_LONG : 0;
        return;
    }
    if (isalpha((unsigned char)*src) || *src == '_') {
        char buf[64];
        int i = 0;
        while ((isalnum((unsigned char)*src) || *src == '_') && i < 63) buf[i++] = *src++;
        buf[i] = '\0';
        static const struct { const char *kw; TokenType t; } kws[] = {
            {"int", TOKEN_INT}, {"char", TOKEN_CHAR}, {"short", TOKEN_SHORT}, {"long", TOKEN_LONG},
            {"unsigned", TOKEN_UNSIGNED}, {"signed", TOKEN_SIGNED}, {"float", TOKEN_FLOAT},
            {"double", TOKEN_DOUBLE}, {"void", TOKEN_VOID}, {"struct", TOKEN_STRUCT}, {"union", TOKEN_UNION},
            {"enum", TOKEN_ENUM}, {"static", TOKEN_STATIC}, {"extern", TOKEN_EXTERN}, {"register", TOKEN_REGISTER},
            {"typedef", TOKEN_TYPEDEF}, {"const", TOKEN_CONST}, {"NULL", TOKEN_NULL},
            {"nullptr", TOKEN_NULL}, {"return", TOKEN_RETURN}, {"if", TOKEN_IF}, {"else", TOKEN_ELSE},
            {"while", TOKEN_WHILE}, {"for", TOKEN_FOR}, {"do", TOKEN_DO}, {"switch", TOKEN_SWITCH},
            {"case", TOKEN_CASE}, {"default", TOKEN_DEFAULT}, {"break", TOKEN_BREAK},
            {"continue", TOKEN_CONTINUE}, {"goto", TOKEN_GOTO}, {"sizeof", TOKEN_SIZEOF},
            {"_Bool", TOKEN_BOOL}, {"bool", TOKEN_BOOL}, {"_Alignas", TOKEN_ALIGNAS}, {"_Alignof", TOKEN_ALIGNOF},
            {"alignas", TOKEN_ALIGNAS}, {"alignof", TOKEN_ALIGNOF}, {"constexpr", TOKEN_CONSTEXPR},
            {"_Static_assert", TOKEN_STATIC_ASSERT}, {"static_assert", TOKEN_STATIC_ASSERT}, {"_Generic", TOKEN_GENERIC},
            {"typeof", TOKEN_TYPEOF}, {"__typeof__", TOKEN_TYPEOF}, {"__typeof", TOKEN_TYPEOF},
            {"typeof_unqual", TOKEN_TYPEOF_UNQUAL}, {"__typeof_unqual__", TOKEN_TYPEOF_UNQUAL}, {"auto", TOKEN_AUTO},
        };
        /* accepted and ignored */
        static const char *ignored[] = {"volatile", "inline", "restrict", "_Noreturn",
                                        "_Thread_local", "thread_local", "__inline", "__restrict", "__volatile__"};   /* one thread */
        for (size_t k = 0; k < sizeof ignored / sizeof ignored[0]; k++)
            if (strcmp(buf, ignored[k]) == 0) { next_token(); return; }
        const char *q8 = src;                          /* the preprocessor may have spaced u8 "x" apart */
        while (*q8 == ' ' || *q8 == '\t') q8++;
        if (strcmp(buf, "u8") == 0 && *q8 == '"') { next_token(); return; }   /* u8"text": already UTF-8 */
        if (strcmp(buf, "u8") == 0 && *q8 == '\'') {                       /* C23 u8'x': unsigned char */
            next_token();
            current_token.num_flags |= NUM_UCHAR;
            current_token.val = (unsigned char)current_token.val;
            return;
        }
        if (!strcmp(buf, "true") || !strcmp(buf, "false")) {   /* C23: constants of type bool */
            current_token.type = TOKEN_NUM;
            current_token.num_flags = NUM_BOOL;
            current_token.val = buf[0] == 't';
            return;
        }
        for (size_t k = 0; k < sizeof kws / sizeof kws[0]; k++)
            if (strcmp(buf, kws[k].kw) == 0) { current_token.type = kws[k].t; return; }
        strcpy(current_token.text, buf);
        current_token.type = TOKEN_IDENT;
        return;
    }
    if (src[0] == '.' && src[1] == '.' && src[2] == '.') { src += 3; current_token.type = TOKEN_ELLIPSIS; return; }
    if (src[0] == '<' && src[1] == '<' && src[2] == '=') { src += 3; current_token.type = TOKEN_SHL_ASSIGN; return; }
    if (src[0] == '>' && src[1] == '>' && src[2] == '=') { src += 3; current_token.type = TOKEN_SHR_ASSIGN; return; }
#define TWO(a, b, t) if (src[0] == a && src[1] == b) { src += 2; current_token.type = t; return; }
    TWO('+', '+', TOKEN_INC) TWO('-', '-', TOKEN_DEC) TWO('-', '>', TOKEN_ARROW)
    TWO('+', '=', TOKEN_PLUS_ASSIGN) TWO('-', '=', TOKEN_MINUS_ASSIGN) TWO('*', '=', TOKEN_MUL_ASSIGN)
    TWO('/', '=', TOKEN_DIV_ASSIGN) TWO('%', '=', TOKEN_MOD_ASSIGN) TWO('&', '=', TOKEN_AND_ASSIGN)
    TWO('|', '=', TOKEN_OR_ASSIGN) TWO('^', '=', TOKEN_XOR_ASSIGN)
    TWO('<', '<', TOKEN_SHL) TWO('>', '>', TOKEN_SHR)
    TWO('=', '=', TOKEN_EQ) TWO('!', '=', TOKEN_NE) TWO('<', '=', TOKEN_LE) TWO('>', '=', TOKEN_GE)
    TWO('&', '&', TOKEN_ANDAND) TWO('|', '|', TOKEN_OROR)
#undef TWO
    switch (*src) {
    case '(': current_token.type = TOKEN_LPAREN; break;
    case ')': current_token.type = TOKEN_RPAREN; break;
    case '{': current_token.type = TOKEN_LBRACE; break;
    case '}': current_token.type = TOKEN_RBRACE; break;
    case '[': current_token.type = TOKEN_LBRACKET; break;
    case ']': current_token.type = TOKEN_RBRACKET; break;
    case '.': current_token.type = TOKEN_DOT; break;
    case ':': current_token.type = TOKEN_COLON; break;
    case '?': current_token.type = TOKEN_QUESTION; break;
    case ',': current_token.type = TOKEN_COMMA; break;
    case ';': current_token.type = TOKEN_SEMI; break;
    case '+': current_token.type = TOKEN_PLUS; break;
    case '-': current_token.type = TOKEN_MINUS; break;
    case '*': current_token.type = TOKEN_ASTERISK; break;
    case '/': current_token.type = TOKEN_SLASH; break;
    case '%': current_token.type = TOKEN_PERCENT; break;
    case '&': current_token.type = TOKEN_AMPERSAND; break;
    case '|': current_token.type = TOKEN_PIPE; break;
    case '^': current_token.type = TOKEN_CARET; break;
    case '~': current_token.type = TOKEN_TILDE; break;
    case '!': current_token.type = TOKEN_NOT; break;
    case '=': current_token.type = TOKEN_ASSIGN; break;
    case '<': current_token.type = TOKEN_LT; break;
    case '>': current_token.type = TOKEN_GT; break;
    default: error("unexpected character '%c'", *src);
    }
    src++;
}

static void expect(TokenType type) {
    static const char *names[] = {
        [TOKEN_LPAREN] = "'('", [TOKEN_RPAREN] = "')'", [TOKEN_LBRACE] = "'{'", [TOKEN_RBRACE] = "'}'",
        [TOKEN_LBRACKET] = "'['", [TOKEN_RBRACKET] = "']'", [TOKEN_SEMI] = "';'", [TOKEN_COLON] = "':'",
        [TOKEN_IDENT] = "identifier", [TOKEN_WHILE] = "'while'", [TOKEN_ASSIGN] = "'='",
    };
    if (current_token.type != type) {
        if (type < (int)(sizeof names / sizeof names[0]) && names[type]) error("%s expected", names[type]);
        error("unexpected token");
    }
    next_token();
}

/* lexer snapshots: used for peeking, dry runs and for re-parsing the for-loop increment */
typedef struct { const char *src; Token tok; int line; } LexState;
static LexState save_lex(void) { LexState s = {src, current_token, cur_line}; return s; }
static void restore_lex(LexState s) { src = s.src; current_token = s.tok; cur_line = s.line; }

static Token peek_token(void) {
    LexState s = save_lex();
    next_token();
    Token t = current_token;
    restore_lex(s);
    return t;
}
/* ============================================================ built-in headers
 * #include <x.h> is not read from disk: these declarations are parsed instead.  Every function
 * declared here resolves (import_resolver below) to the C runtime this compiler is linked with,
 * or to a small wrapper where the C ABI differs from ours (long is 64-bit here, even on Windows). */
static const char *prelude_base =
    /* va_list must have the host C library's layout so it can be passed to vprintf & co. */
    "#if defined(_WIN32) || (defined(__APPLE__) && defined(__aarch64__))\n"
    "typedef char *__c2_va_list;\n"
    "#elif defined(__aarch64__)\n"
    "typedef struct { void *__stack, *__gr_top, *__vr_top; int __gr_offs, __vr_offs; } __c2_va_list[1];\n"
    "#else\n"
    "typedef struct { unsigned gp_offset, fp_offset; void *overflow_arg_area, *reg_save_area; } __c2_va_list[1];\n"
    "#endif\n"
    /* compiler extensions common in library headers (C2_load): accepted, no effect */
    "#define __declspec(x)\n#define __attribute__(x)\n#define __attribute(x)\n#define __cdecl\n"
    "#define __stdcall\n#define __fastcall\n#define __extension__\n#define __inline__ inline\n"
    "#define __forceinline inline\n#define __restrict__\n#define __asm__(x)\n"
    "typedef unsigned long size_t;\n"
    "typedef long ptrdiff_t;\n"
    "void *__c2_memset(void *p, int c, size_t n);\n";      /* used for zero-initialisation */

#define WCHAR_T_DEF "#ifndef __C2_WCHAR_T\n#define __C2_WCHAR_T\n#ifdef _WIN32\ntypedef unsigned short wchar_t;\n#else\ntypedef int wchar_t;\n#endif\n#endif\n"
static const struct { const char *name, *text; } prelude_headers[] = {
    {"stdio.h",
     "typedef struct __c2_FILE FILE;\n"
     "#define EOF (-1)\n#define SEEK_SET 0\n#define SEEK_CUR 1\n#define SEEK_END 2\n"
     "#define NULL ((void *)0)\n#define stdin __c2_stdin()\n#define stdout __c2_stdout()\n#define stderr __c2_stderr()\n"
     "FILE *__c2_stdin(void); FILE *__c2_stdout(void); FILE *__c2_stderr(void);\n"
     "int printf(const char *fmt, ...); int fprintf(FILE *f, const char *fmt, ...);\n"
     "int sprintf(char *buf, const char *fmt, ...); int snprintf(char *buf, size_t n, const char *fmt, ...);\n"
     "int vprintf(const char *fmt, __c2_va_list ap); int vfprintf(FILE *f, const char *fmt, __c2_va_list ap);\n"
     "int vsprintf(char *buf, const char *fmt, __c2_va_list ap);\n"
     "int vsnprintf(char *buf, size_t n, const char *fmt, __c2_va_list ap);\n"
     "void perror(const char *s); int scanf(const char *fmt, ...); int sscanf(const char *s, const char *fmt, ...);\n"
     "int puts(const char *s); int putchar(int c); int getchar(void);\n"
     "FILE *fopen(const char *path, const char *mode); int fclose(FILE *f); int fflush(FILE *f);\n"
     "size_t fread(void *p, size_t size, size_t n, FILE *f); size_t fwrite(const void *p, size_t size, size_t n, FILE *f);\n"
     "char *fgets(char *s, int n, FILE *f); int fputs(const char *s, FILE *f); int fputc(int c, FILE *f);\n"
     "int putc(int c, FILE *f); int getc(FILE *f);\n"
     "int fgetc(FILE *f); int feof(FILE *f); int fseek(FILE *f, long off, int whence); long ftell(FILE *f);\n"
     "int remove(const char *path); int rename(const char *from, const char *to);\n"
     "typedef struct { long long __pos[2]; } fpos_t;\n#define FILENAME_MAX 4096\n#define FOPEN_MAX 20\n"
     "FILE *tmpfile(void); char *tmpnam(char *s); FILE *freopen(const char *path, const char *mode, FILE *f);\n"
     "void setbuf(FILE *f, char *buf); int setvbuf(FILE *f, char *buf, int mode, size_t size);\n"
     "int fscanf(FILE *f, const char *fmt, ...); int vfscanf(FILE *f, const char *fmt, __c2_va_list ap);\n"
     "int vscanf(const char *fmt, __c2_va_list ap); int vsscanf(const char *s, const char *fmt, __c2_va_list ap);\n"
     "int ungetc(int c, FILE *f); int fgetpos(FILE *f, fpos_t *pos); int fsetpos(FILE *f, const fpos_t *pos);\n"
     "void rewind(FILE *f); void clearerr(FILE *f); int ferror(FILE *f);\n"
     "#include <__c2_host_stdio.h>\n"},
    {"stdlib.h",
     WCHAR_T_DEF "#define MB_CUR_MAX __c2_mb_cur_max()\nint __c2_mb_cur_max(void);\n"
     "int mblen(const char *s, size_t n); int mbtowc(wchar_t *w, const char *s, size_t n); int wctomb(char *s, wchar_t w);\n"
     "size_t mbstowcs(wchar_t *w, const char *s, size_t n); size_t wcstombs(char *s, const wchar_t *w, size_t n);\n"
     "#define EXIT_SUCCESS 0\n#define EXIT_FAILURE 1\n"
     "void *malloc(size_t n); void *calloc(size_t n, size_t size); void *realloc(void *p, size_t n);\n"
     "void free(void *p);\n"
     "void exit(int code); void abort(void); int atoi(const char *s); long atol(const char *s); double atof(const char *s);\n"
     "long strtol(const char *s, char **end, int base); double strtod(const char *s, char **end);\n"
     "int abs(int x); long labs(long x); int rand(void); void srand(unsigned seed); char *getenv(const char *name);\n"
     "void qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *));\n"
     "void *bsearch(const void *key, const void *base, size_t n, size_t size, int (*cmp)(const void *, const void *));\n"
     "int atexit(void (*fn)(void));\n"
     "long long atoll(const char *s); float strtof(const char *s, char **end); double strtold(const char *s, char **end);\n"
     "long long strtoll(const char *s, char **end, int base); unsigned long strtoul(const char *s, char **end, int base);\n"
     "unsigned long long strtoull(const char *s, char **end, int base);\n"
     "int system(const char *cmd); long long llabs(long long x);\n"
     "typedef struct { int quot, rem; } div_t; typedef struct { long quot, rem; } ldiv_t;\n"
     "typedef struct { long long quot, rem; } lldiv_t;\n"
     "div_t div(int a, int b); ldiv_t ldiv(long a, long b); lldiv_t lldiv(long long a, long long b);\n"
     "void _Exit(int code); void quick_exit(int code); int at_quick_exit(void (*fn)(void));\n"
     "char *strdup(const char *s);\n"
     "#define NULL ((void *)0)\n#include <__c2_host_stdlib.h>\n"},
    {"string.h",
     "size_t strlen(const char *s); int strcmp(const char *a, const char *b); int strncmp(const char *a, const char *b, size_t n);\n"
     "char *strcpy(char *d, const char *s); char *strncpy(char *d, const char *s, size_t n);\n"
     "char *strcat(char *d, const char *s); char *strncat(char *d, const char *s, size_t n);\n"
     "char *strchr(const char *s, int c); char *strrchr(const char *s, int c); char *strstr(const char *h, const char *n);\n"
     "void *memset(void *p, int c, size_t n); void *memcpy(void *d, const void *s, size_t n);\n"
     "void *memmove(void *d, const void *s, size_t n); int memcmp(const void *a, const void *b, size_t n);\n"
     "void *memchr(const void *p, int c, size_t n); size_t strspn(const char *s, const char *set);\n"
     "size_t strcspn(const char *s, const char *set); char *strpbrk(const char *s, const char *set);\n"
     "char *strtok(char *s, const char *sep); char *strerror(int err); int strcoll(const char *a, const char *b);\n"
     "size_t strxfrm(char *d, const char *s, size_t n); char *strdup(const char *s);\n"
     "#define NULL ((void *)0)\n"},
    {"math.h",
     "#define M_PI 3.14159265358979323846\n#define M_E 2.7182818284590452354\n"
     "#define HUGE_VAL __c2_huge_val()\n#define INFINITY __c2_huge_valf()\n#define NAN __c2_nanf()\n"
     "double __c2_huge_val(void); float __c2_huge_valf(void); float __c2_nanf(void);\n"
     "double sqrt(double x); double sin(double x); double cos(double x); double tan(double x);\n"
     "double asin(double x); double acos(double x); double atan(double x); double atan2(double y, double x);\n"
     "double exp(double x); double log(double x); double log10(double x); double log2(double x); double pow(double x, double y);\n"
     "double fabs(double x); double floor(double x); double ceil(double x); double round(double x); double trunc(double x);\n"
     "double fmod(double x, double y); double hypot(double x, double y); double fmin(double a, double b); double fmax(double a, double b);\n"
     "float sqrtf(float x); float sinf(float x); float cosf(float x); float tanf(float x); float atan2f(float y, float x);\n"
     "float expf(float x); float logf(float x); float powf(float x, float y); float fabsf(float x);\n"
     "float floorf(float x); float ceilf(float x); float roundf(float x); float fmodf(float x, float y);\n"
     "double sinh(double x); double cosh(double x); double tanh(double x); double asinh(double x);\n"
     "double acosh(double x); double atanh(double x); double exp2(double x); double expm1(double x);\n"
     "double log1p(double x); double logb(double x); int ilogb(double x); double frexp(double x, int *e);\n"
     "double ldexp(double x, int e); double modf(double x, double *ip); double scalbn(double x, int n);\n"
     "double scalbln(double x, long n); double cbrt(double x); double erf(double x); double erfc(double x);\n"
     "double lgamma(double x); double tgamma(double x); double nearbyint(double x); double rint(double x);\n"
     "long lrint(double x); long long llrint(double x); long lround(double x); long long llround(double x);\n"
     "double remainder(double x, double y); double remquo(double x, double y, int *q);\n"
     "double copysign(double x, double y); double nan(const char *tag); double nextafter(double x, double y);\n"
     "double nexttoward(double x, double y); double fdim(double x, double y); double fma(double x, double y, double z);\n"
     "float asinf(float x); float acosf(float x); float atanf(float x); float exp2f(float x); float log2f(float x);\n"
     "float log10f(float x); float truncf(float x); float hypotf(float x, float y); float fminf(float a, float b);\n"
     "float fmaxf(float a, float b); float cbrtf(float x); float copysignf(float x, float y);\n"
     "int __c2_signbit(double x);\n"
     "#define HUGE_VALF __c2_huge_valf()\n#define isnan(x) ((x) != (x))\n"
     "#define isinf(x) (!isnan(x) && isnan((x) - (x)))\n#define isfinite(x) (((x) - (x)) == ((x) - (x)))\n"
     "#define isnormal(x) (isfinite(x) && (x) != 0)\n#define signbit(x) __c2_signbit(x)\n"
     "#define M_LN2 0.69314718055994530942\n#define M_LN10 2.30258509299404568402\n"
     "#define M_PI_2 1.57079632679489661923\n#define M_PI_4 0.78539816339744830962\n"
     "#define M_SQRT2 1.41421356237309504880\n#define M_SQRT1_2 0.70710678118654752440\n"},
    {"limits.h",
     "#define CHAR_BIT 8\n#define SCHAR_MIN (-128)\n#define SCHAR_MAX 127\n#define UCHAR_MAX 255\n"
     "#define CHAR_MIN (-128)\n#define CHAR_MAX 127\n#define SHRT_MIN (-32768)\n#define SHRT_MAX 32767\n"
     "#define USHRT_MAX 65535\n#define INT_MIN (-2147483647-1)\n#define INT_MAX 2147483647\n"
     "#define UINT_MAX 4294967295u\n#define LONG_MIN (-9223372036854775807L-1)\n#define LONG_MAX 9223372036854775807L\n"
     "#define ULONG_MAX 18446744073709551615UL\n#define LLONG_MIN (-9223372036854775807L-1)\n"
     "#define LLONG_MAX 9223372036854775807L\n#define ULLONG_MAX 18446744073709551615UL\n"},
    {"float.h",
     "#define FLT_MAX 3.40282347e+38f\n#define FLT_MIN 1.17549435e-38f\n#define FLT_EPSILON 1.19209290e-7f\n"
     "#define DBL_MAX 1.7976931348623157e+308\n#define DBL_MIN 2.2250738585072014e-308\n"
     "#define DBL_EPSILON 2.2204460492503131e-16\n"
     "#define FLT_RADIX 2\n#define FLT_DIG 6\n#define DBL_DIG 15\n#define FLT_MANT_DIG 24\n#define DBL_MANT_DIG 53\n"
     "#define FLT_MAX_EXP 128\n#define DBL_MAX_EXP 1024\n#define FLT_MIN_EXP (-125)\n#define DBL_MIN_EXP (-1021)\n"
     "#define FLT_TRUE_MIN 1.40129846e-45f\n#define DBL_TRUE_MIN 4.9406564584124654e-324\n#define DECIMAL_DIG 17\n"
     "#define FLT_EVAL_METHOD 0\n#define LDBL_MAX DBL_MAX\n#define LDBL_MIN DBL_MIN\n#define LDBL_EPSILON DBL_EPSILON\n"
     "#define LDBL_DIG DBL_DIG\n#define LDBL_MANT_DIG DBL_MANT_DIG\n"},
    {"stdint.h",
     "typedef signed char int8_t; typedef unsigned char uint8_t; typedef short int16_t; typedef unsigned short uint16_t;\n"
     "typedef int int32_t; typedef unsigned uint32_t; typedef long int64_t; typedef unsigned long uint64_t;\n"
     "typedef long intptr_t; typedef unsigned long uintptr_t;\n"
     "#define INT8_MIN (-128)\n#define INT8_MAX 127\n#define UINT8_MAX 255\n#define INT16_MIN (-32768)\n"
     "#define INT16_MAX 32767\n#define UINT16_MAX 65535\n#define INT32_MIN (-2147483647-1)\n#define INT32_MAX 2147483647\n"
     "#define UINT32_MAX 4294967295u\n#define INT64_MIN (-9223372036854775807L-1)\n#define INT64_MAX 9223372036854775807L\n"
     "#define UINT64_MAX 18446744073709551615UL\n"
     "typedef signed char int_least8_t; typedef short int_least16_t; typedef int int_least32_t; typedef long int_least64_t;\n"
     "typedef unsigned char uint_least8_t; typedef unsigned short uint_least16_t; typedef unsigned uint_least32_t;\n"
     "typedef unsigned long uint_least64_t; typedef signed char int_fast8_t; typedef long int_fast16_t;\n"
     "typedef long int_fast32_t; typedef long int_fast64_t; typedef unsigned char uint_fast8_t;\n"
     "typedef unsigned long uint_fast16_t; typedef unsigned long uint_fast32_t; typedef unsigned long uint_fast64_t;\n"
     "typedef long intmax_t; typedef unsigned long uintmax_t;\n"
     "#define INTPTR_MIN INT64_MIN\n#define INTPTR_MAX INT64_MAX\n#define UINTPTR_MAX UINT64_MAX\n"
     "#define INTMAX_MIN INT64_MIN\n#define INTMAX_MAX INT64_MAX\n#define UINTMAX_MAX UINT64_MAX\n"
     "#define SIZE_MAX UINT64_MAX\n#define PTRDIFF_MIN INT64_MIN\n#define PTRDIFF_MAX INT64_MAX\n"
     "#define INT8_C(c) c\n#define INT16_C(c) c\n#define INT32_C(c) c\n#define INT64_C(c) c ## L\n"
     "#define UINT8_C(c) c\n#define UINT16_C(c) c\n#define UINT32_C(c) c ## U\n#define UINT64_C(c) c ## UL\n"
     "#define INTMAX_C(c) c ## L\n#define UINTMAX_C(c) c ## UL\n"},
    {"stdarg.h",
     "typedef __c2_va_list va_list;\ntypedef __c2_va_list __gnuc_va_list;\n"
     "#define va_start(ap, last) __builtin_va_start(ap, last)\n#define va_arg(ap, type) __builtin_va_arg(ap, type)\n"
     "#define va_end(ap) __builtin_va_end(ap)\n#define va_copy(dst, src) __builtin_va_copy(dst, src)\n"},
    {"stddef.h", "#define offsetof(type, member) offsetof(type, member)\n#define NULL ((void *)0)\n"
                 "typedef struct { long long __ll; double __d; } max_align_t;\n" WCHAR_T_DEF},
    {"wchar.h",                         /* the wide string basics; wide I/O is missing */
     "#include <stddef.h>\n#define WEOF ((wint_t)-1)\n"
     "#ifdef _WIN32\ntypedef unsigned short wint_t;\n#define WCHAR_MIN 0\n#define WCHAR_MAX 0xffff\n"
     "#else\ntypedef unsigned wint_t;\n#define WCHAR_MIN (-2147483647 - 1)\n#define WCHAR_MAX 2147483647\n#endif\n"
     "size_t wcslen(const wchar_t *s); int wcscmp(const wchar_t *a, const wchar_t *b);\n"
     "int wcsncmp(const wchar_t *a, const wchar_t *b, size_t n); wchar_t *wcscpy(wchar_t *d, const wchar_t *s);\n"
     "wchar_t *wcsncpy(wchar_t *d, const wchar_t *s, size_t n); wchar_t *wcscat(wchar_t *d, const wchar_t *s);\n"
     "wchar_t *wcsncat(wchar_t *d, const wchar_t *s, size_t n); wchar_t *wcschr(const wchar_t *s, wchar_t c);\n"
     "wchar_t *wcsrchr(const wchar_t *s, wchar_t c); wchar_t *wcsstr(const wchar_t *h, const wchar_t *n);\n"
     "wchar_t *wmemcpy(wchar_t *d, const wchar_t *s, size_t n); wchar_t *wmemmove(wchar_t *d, const wchar_t *s, size_t n);\n"
     "wchar_t *wmemset(wchar_t *d, wchar_t c, size_t n); int wmemcmp(const wchar_t *a, const wchar_t *b, size_t n);\n"
     "wchar_t *wmemchr(const wchar_t *s, wchar_t c, size_t n);\n"
     "double wcstod(const wchar_t *s, wchar_t **end); long long wcstoll(const wchar_t *s, wchar_t **end, int base);\n"
     "unsigned long long wcstoull(const wchar_t *s, wchar_t **end, int base);\n"},
    {"assert.h",
     "void __c2_assert_fail(const char *expr, const char *file, int line);\n"
     "#ifdef NDEBUG\n#define assert(x) ((void)0)\n#else\n"
     "#define assert(x) ((x) ? (void)0 : __c2_assert_fail(#x, __FILE__, __LINE__))\n#endif\n"},
    {"stdbool.h", "#define bool _Bool\n#define true 1\n#define false 0\n#define __bool_true_false_are_defined 1\n"},
    {"ctype.h",
     "int isalpha(int c); int isdigit(int c); int isalnum(int c); int isspace(int c); int isupper(int c);\n"
     "int islower(int c); int isxdigit(int c); int ispunct(int c); int toupper(int c); int tolower(int c);\n"
     "int isblank(int c); int iscntrl(int c); int isgraph(int c); int isprint(int c);\n"},
    {"time.h",
     "typedef long time_t; typedef long clock_t;\n#define CLOCKS_PER_SEC __c2_clocks_per_sec()\n"
     "long __c2_clocks_per_sec(void); long clock(void); long time(long *t);\n"
     "struct tm { int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year, tm_wday, tm_yday, tm_isdst;\n"
     "#ifndef _WIN32\n long tm_gmtoff; const char *tm_zone;\n#endif\n };\n"
     "struct timespec { time_t tv_sec; long tv_nsec; };\n#define TIME_UTC 1\n#define NULL ((void *)0)\n"
     "double difftime(time_t a, time_t b); time_t mktime(struct tm *t); char *asctime(const struct tm *t);\n"
     "char *ctime(const time_t *t); struct tm *gmtime(const time_t *t); struct tm *localtime(const time_t *t);\n"
     "size_t strftime(char *s, size_t n, const char *fmt, const struct tm *t);\n"
     "int timespec_get(struct timespec *ts, int base);\n"},
    {"inttypes.h",
     "#include <stdint.h>\n"
     "intmax_t imaxabs(intmax_t x); intmax_t strtoimax(const char *s, char **end, int base);\n"
     "uintmax_t strtoumax(const char *s, char **end, int base);\n"
     "#define PRId8 \"d\"\n#define PRId16 \"d\"\n#define PRId32 \"d\"\n#define PRId64 \"lld\"\n#define PRIdPTR \"lld\"\n#define PRIdMAX \"lld\"\n#define PRIdLEAST8 \"d\"\n#define PRIdLEAST16 \"d\"\n#define PRIdLEAST32 \"d\"\n#define PRIdLEAST64 \"lld\"\n#define PRIdFAST8 \"d\"\n#define PRIdFAST16 \"lld\"\n#define PRIdFAST32 \"lld\"\n#define PRIdFAST64 \"lld\"\n#define PRIi8 \"i\"\n#define PRIi16 \"i\"\n#define PRIi32 \"i\"\n#define PRIi64 \"lli\"\n#define PRIiPTR \"lli\"\n#define PRIiMAX \"lli\"\n#define PRIiLEAST8 \"i\"\n#define PRIiLEAST16 \"i\"\n#define PRIiLEAST32 \"i\"\n#define PRIiLEAST64 \"lli\"\n#define PRIiFAST8 \"i\"\n#define PRIiFAST16 \"lli\"\n#define PRIiFAST32 \"lli\"\n#define PRIiFAST64 \"lli\"\n#define PRIu8 \"u\"\n#define PRIu16 \"u\"\n#define PRIu32 \"u\"\n#define PRIu64 \"llu\"\n#define PRIuPTR \"llu\"\n#define PRIuMAX \"llu\"\n#define PRIuLEAST8 \"u\"\n#define PRIuLEAST16 \"u\"\n#define PRIuLEAST32 \"u\"\n#define PRIuLEAST64 \"llu\"\n#define PRIuFAST8 \"u\"\n#define PRIuFAST16 \"llu\"\n#define PRIuFAST32 \"llu\"\n#define PRIuFAST64 \"llu\"\n#define PRIx8 \"x\"\n#define PRIx16 \"x\"\n#define PRIx32 \"x\"\n#define PRIx64 \"llx\"\n#define PRIxPTR \"llx\"\n#define PRIxMAX \"llx\"\n#define PRIxLEAST8 \"x\"\n#define PRIxLEAST16 \"x\"\n#define PRIxLEAST32 \"x\"\n#define PRIxLEAST64 \"llx\"\n#define PRIxFAST8 \"x\"\n#define PRIxFAST16 \"llx\"\n#define PRIxFAST32 \"llx\"\n#define PRIxFAST64 \"llx\"\n#define PRIX8 \"X\"\n#define PRIX16 \"X\"\n#define PRIX32 \"X\"\n#define PRIX64 \"llX\"\n#define PRIXPTR \"llX\"\n#define PRIXMAX \"llX\"\n#define PRIXLEAST8 \"X\"\n#define PRIXLEAST16 \"X\"\n#define PRIXLEAST32 \"X\"\n#define PRIXLEAST64 \"llX\"\n#define PRIXFAST8 \"X\"\n#define PRIXFAST16 \"llX\"\n#define PRIXFAST32 \"llX\"\n#define PRIXFAST64 \"llX\"\n#define PRIo8 \"o\"\n#define PRIo16 \"o\"\n#define PRIo32 \"o\"\n#define PRIo64 \"llo\"\n#define PRIoPTR \"llo\"\n#define PRIoMAX \"llo\"\n#define PRIoLEAST8 \"o\"\n#define PRIoLEAST16 \"o\"\n#define PRIoLEAST32 \"o\"\n#define PRIoLEAST64 \"llo\"\n#define PRIoFAST8 \"o\"\n#define PRIoFAST16 \"llo\"\n#define PRIoFAST32 \"llo\"\n#define PRIoFAST64 \"llo\"\n#define SCNd8 \"hhd\"\n#define SCNd16 \"hd\"\n#define SCNd32 \"d\"\n#define SCNd64 \"lld\"\n#define SCNdPTR \"lld\"\n#define SCNdMAX \"lld\"\n#define SCNi8 \"hhi\"\n#define SCNi16 \"hi\"\n#define SCNi32 \"i\"\n#define SCNi64 \"lli\"\n#define SCNiPTR \"lli\"\n#define SCNiMAX \"lli\"\n#define SCNu8 \"hhu\"\n#define SCNu16 \"hu\"\n#define SCNu32 \"u\"\n#define SCNu64 \"llu\"\n#define SCNuPTR \"llu\"\n#define SCNuMAX \"llu\"\n#define SCNx8 \"hhx\"\n#define SCNx16 \"hx\"\n#define SCNx32 \"x\"\n#define SCNx64 \"llx\"\n#define SCNxPTR \"llx\"\n#define SCNxMAX \"llx\"\n#define SCNo8 \"hho\"\n#define SCNo16 \"ho\"\n#define SCNo32 \"o\"\n#define SCNo64 \"llo\"\n#define SCNoPTR \"llo\"\n#define SCNoMAX \"llo\"\n"},
    {"iso646.h",
     "#define and &&\n#define and_eq &=\n#define bitand &\n#define bitor |\n#define compl ~\n#define not !\n"
     "#define not_eq !=\n#define or ||\n#define or_eq |=\n#define xor ^\n#define xor_eq ^=\n"},
    {"stdalign.h", "#define alignas _Alignas\n#define alignof _Alignof\n#define __alignas_is_defined 1\n#define __alignof_is_defined 1\n"},
    {"stdnoreturn.h", "#define noreturn _Noreturn\n"},
    {"errno.h", "#define errno (*__c2_errno())\nint *__c2_errno(void);\n#include <__c2_host_errno.h>\n"},
    {"signal.h",
     "typedef int sig_atomic_t;\n#define SIG_DFL ((void (*)(int))0)\n#define SIG_IGN ((void (*)(int))1)\n"
     "#define SIG_ERR ((void (*)(int))-1)\nvoid (*signal(int sig, void (*handler)(int)))(int);\nint raise(int sig);\n"
     "#include <__c2_host_signal.h>\n"},
    {"locale.h", "char *setlocale(int category, const char *locale);\n#define NULL ((void *)0)\n#include <__c2_host_locale.h>\n"},
    {"dlfcn.h",   /* on Windows: LoadLibrary / GetProcAddress / FreeLibrary */
     "#define RTLD_LAZY 1\n#define RTLD_NOW 2\n#define RTLD_GLOBAL 256\n#define RTLD_LOCAL 0\n"
     "void *dlopen(const char *path, int flags); void *dlsym(void *handle, const char *name);\n"
     "int dlclose(void *handle); char *dlerror(void);\n"},
};

/* constants whose values come from the host C library (they differ between platforms) */
static const char *host_header(const char *name) {
    static char b[2048];
    int n = 0;
#define HD(macro) n += snprintf(b + n, sizeof b - n, "#define " #macro " %d\n", (int)(macro))
    if (strcmp(name, "__c2_host_stdio.h") == 0) {
        HD(_IOFBF); HD(_IOLBF); HD(_IONBF); HD(L_tmpnam); HD(TMP_MAX);
        n += snprintf(b + n, sizeof b - n, "#define BUFSIZ %d\n", BUFSIZ > 8192 ? BUFSIZ : 8192);
    } else if (strcmp(name, "__c2_host_stdlib.h") == 0) {
        HD(RAND_MAX);
    } else if (strcmp(name, "__c2_host_errno.h") == 0) {
        HD(EDOM); HD(ERANGE); HD(EILSEQ); HD(EPERM); HD(ENOENT); HD(EINTR); HD(EIO); HD(EBADF); HD(EAGAIN);
        HD(ENOMEM); HD(EACCES); HD(EEXIST); HD(ENOTDIR); HD(EISDIR); HD(EINVAL); HD(ENFILE); HD(EMFILE);
        HD(ENOSPC); HD(EPIPE); HD(ENAMETOOLONG); HD(ENOSYS); HD(ENOTEMPTY); HD(EBUSY); HD(EXDEV); HD(ESPIPE);
    } else if (strcmp(name, "__c2_host_signal.h") == 0) {
        HD(SIGABRT); HD(SIGFPE); HD(SIGILL); HD(SIGINT); HD(SIGSEGV); HD(SIGTERM);
    } else if (strcmp(name, "__c2_host_locale.h") == 0) {
        HD(LC_ALL); HD(LC_COLLATE); HD(LC_CTYPE); HD(LC_MONETARY); HD(LC_NUMERIC); HD(LC_TIME);
    } else return NULL;
#undef HD
    return b;
}

static const char *builtin_header(const char *name) {
    for (size_t k = 0; k < sizeof prelude_headers / sizeof prelude_headers[0]; k++)
        if (strcmp(prelude_headers[k].name, name) == 0) return prelude_headers[k].text;
    return host_header(name);
}

/* ============================================================ PREPROCESSOR
 * Token based (in the style of chibicc): the source is split into pp-tokens, directives are
 * executed, macros are expanded with hide sets (so recursive macros stop), and the result is
 * written back as text for the lexer.  Line numbers survive: the output keeps one source line per
 * output line, and `# N "file"` markers (read by the lexer) where files change or lines jump.
 *
 *   #define (object-like, function-like, # and ##, __VA_ARGS__, `, ## __VA_ARGS__`), #undef,
 *   #if #ifdef #ifndef #elif #else #endif with defined(), #include "file" (relative to the
 *   including file, then -I dirs) and <builtin.h>, #pragma once, #error, #warning, #line,
 *   __FILE__ __LINE__ __DATE__ __TIME__ __COUNTER__ and the usual predefined macros. */

enum { PK_IDENT, PK_NUM, PK_STR, PK_CHAR, PK_PUNCT, PK_EOF };

typedef struct Hide { int macro; struct Hide *next; } Hide;

typedef struct PPTok {
    int kind;
    char *s;            /* NUL-terminated text */
    int len;
    int bol, space;     /* at beginning of line / preceded by white space */
    int line, file;
    Hide *hs;           /* macros this token came out of: not expanded again */
    struct PPTok *next;
} PPTok;

typedef struct {
    char name[64];
    int is_func, nparams, variadic;
    char params[64][64];
    PPTok *body;        /* ends with PK_EOF */
    int builtin;        /* 1 __LINE__, 2 __FILE__, 3 __COUNTER__ */
    int deleted;
    int owner;          /* -1: visible everywhere; else the file key of the .h2/.c2 header that owns it */
    int exported;       /* an owned macro made visible everywhere by #export */
} Macro;

static Macro *macros;
static int nmacros, cap_macros;

static char *pp_files[1024];
static int pp_file_key[1024];   /* the first pp_files index with the same path: one key per file */
static int npp_files;
static PPTok *pp_ctx;           /* the token being expanded or the directive: decides macro visibility */
static int pp_main_file = -1;   /* the file being compiled: its macros are never file-scoped */
static char *include_dirs[64];
static int ninclude_dirs;
static char *pending_defines[64][2];   /* -D: name, value; applied after the predefined macros */
static int npending_defines;
static char *pragma_once[256];
static int npragma_once;
static int pp_counter;

static void pp_error(PPTok *t, const char *fmt, ...) {
    char msg[1024];
    va_list a;
    va_start(a, fmt);
    vsnprintf(msg, sizeof msg, fmt, a);
    va_end(a);
    diag("%s:%d: error: %s\n", t ? pp_files[t->file] : "?", t ? t->line : 0, msg);
    fatal();
}

static PPTok *new_tok(int kind, const char *s, int len, PPTok *pos) {
    PPTok *t = calloc(1, sizeof *t);
    t->kind = kind;
    t->s = malloc(len + 1);
    memcpy(t->s, s, len);
    t->s[len] = '\0';
    t->len = len;
    if (pos) { t->line = pos->line; t->file = pos->file; t->space = pos->space; t->bol = pos->bol; t->hs = pos->hs; }
    return t;
}

static PPTok *copy_tok(PPTok *t) {
    PPTok *c = malloc(sizeof *c);
    *c = *t;
    c->next = NULL;
    return c;
}

static PPTok *eof_tok(PPTok *pos) { PPTok *t = new_tok(PK_EOF, "", 0, pos); t->bol = 1; return t; }

static int is_punct(PPTok *t, const char *s) { return t->kind == PK_PUNCT && strcmp(t->s, s) == 0; }
static int is_ident(PPTok *t, const char *s) { return t->kind == PK_IDENT && strcmp(t->s, s) == 0; }

/* ---------------- tokenizer */
static const char *pp_puncts[] = {
    "<<=", ">>=", "...", "->", "++", "--", "<<", ">>", "<=", ">=", "==", "!=", "&&", "||", "+=", "-=",
    "*=", "/=", "%=", "&=", "|=", "^=", "##",
    "%:%:", "<:", ":>", "<%", "%>", "%:",        /* digraphs (C95): ## [ ] { } # */
};
static const char *digraph_of(const char *s, int len) {
    static const char *from[] = {"%:%:", "<:", ":>", "<%", "%>", "%:"}, *to[] = {"##", "[", "]", "{", "}", "#"};
    for (int i = 0; i < 6; i++) if ((int)strlen(from[i]) == len && !strncmp(s, from[i], len)) return to[i];
    return NULL;
}

/* trigraphs (C89-C17; C23 removed them): ??= # ??/ \ ??' ^ ??( [ ??) ] ??! | ??< { ??> } ??- ~.
   Translated in .c files, before line splicing; .c2 files follow C23 and keep them as text. */
static char *trigraphs(const char *text) {
    if (c2_ext || !strstr(text, "??")) return NULL;
    static const char from[] = "=/'()!<>-", to[] = "#\\^[]|{}~";
    char *s = malloc(strlen(text) + 1), *o = s;
    for (const char *p = text; *p;) {
        const char *k = p[0] == '?' && p[1] == '?' && p[2] ? strchr(from, p[2]) : NULL;
        if (k) { *o++ = to[k - from]; p += 3; }
        else *o++ = *p++;
    }
    *o = '\0';
    return s;
}

static PPTok *pp_tokenize(const char *text, int file) {
    /* splice backslash-newlines, keeping line numbers: the removed newlines are put back after
       the end of the logical line */
    char *tri = file > 0 ? trigraphs(text) : NULL;     /* (file 0: the built-in prelude) */
    if (tri) text = tri;
    size_t n = strlen(text);
    char *s = malloc(n + 2), *o = s;
    int pending = 0;
    for (const char *p = text; *p;) {
        if (p[0] == '\r' && p[1] == '\n') { p++; continue; }
        if (p[0] == '\\' && p[1] == '\n') { p += 2; pending++; continue; }
        if (p[0] == '\\' && p[1] == '\r' && p[2] == '\n') { p += 3; pending++; continue; }
        *o++ = *p;
        if (*p++ == '\n') for (; pending > 0; pending--) *o++ = '\n';
    }
    *o++ = '\n';
    *o = '\0';

    PPTok head = {0}, *cur = &head;
    int line = 1, bol = 1, space = 0;
    const char *p = s;
    while (*p) {
        if (*p == '\n') { line++; bol = 1; space = 0; p++; continue; }
        if (isspace((unsigned char)*p)) { space = 1; p++; continue; }
        if (p[0] == '/' && p[1] == '/') { while (*p && *p != '\n') p++; space = 1; continue; }
        if (p[0] == '/' && p[1] == '*') {
            int start = line;
            p += 2;
            while (*p && !(p[0] == '*' && p[1] == '/')) { if (*p == '\n') line++; p++; }
            if (!*p) { PPTok e = {0}; e.file = file; e.line = start; pp_error(&e, "unterminated comment"); }
            p += 2;
            space = 1;
            continue;
        }
        const char *b = p;
        int kind;
        if (isalpha((unsigned char)*p) || *p == '_') {
            while (isalnum((unsigned char)*p) || *p == '_') p++;
            kind = PK_IDENT;
            if ((*p == '"' || *p == '\'') && (p - b == 1 && (*b == 'L' || *b == 'u' || *b == 'U'))) goto quoted;
        } else if (isdigit((unsigned char)*p) || (p[0] == '.' && isdigit((unsigned char)p[1]))) {
            p++;
            for (;;) {
                if (*p == '.' && dot_starts_member(b, p)) break;   /* .c2: 21.twice() */
                if ((*p == 'e' || *p == 'E' || *p == 'p' || *p == 'P') && (p[1] == '+' || p[1] == '-')) p += 2;
                else if (isalnum((unsigned char)*p) || *p == '.' || *p == '_') p++;
                else if (*p == '\'' && isalnum((unsigned char)p[1])) p++;   /* C23 digit separator: 1'000'000 */
                else break;
            }
            kind = PK_NUM;
        } else if (*p == '"' || *p == '\'') {
        quoted:;
            char q = *p++;
            while (*p && *p != q && *p != '\n') { if (*p == '\\' && p[1]) p++; p++; }
            if (*p != q) { PPTok e = {0}; e.file = file; e.line = line; pp_error(&e, "unterminated %s literal", q == '"' ? "string" : "character"); }
            p++;
            kind = q == '"' ? PK_STR : PK_CHAR;
        } else {
            int len = 1;
            for (size_t k = 0; k < sizeof pp_puncts / sizeof pp_puncts[0]; k++) {
                int l = (int)strlen(pp_puncts[k]);
                if (strncmp(p, pp_puncts[k], l) == 0) { len = l; break; }
            }
            p += len;
            kind = PK_PUNCT;
        }
        const char *dg = kind == PK_PUNCT ? digraph_of(b, (int)(p - b)) : NULL;
        PPTok *t = dg ? new_tok(kind, dg, (int)strlen(dg), NULL) : new_tok(kind, b, (int)(p - b), NULL);
        if (kind == PK_NUM && memchr(t->s, '\'', t->len)) {   /* drop digit separators */
            int k = 0;
            for (int i = 0; i < t->len; i++) if (t->s[i] != '\'') t->s[k++] = t->s[i];
            t->s[k] = '\0';
            t->len = k;
        }
        t->line = line;
        t->file = file;
        t->bol = bol;
        t->space = space;
        cur = cur->next = t;
        bol = space = 0;
    }
    PPTok *e = new_tok(PK_EOF, "", 0, NULL);
    e->line = line;
    e->file = file;
    e->bol = 1;
    cur->next = e;
    free(s);
    free(tri);
    return head.next;
}

/* ---------------- macros and hide sets */
/* File-scoped macros (.c2 builds): a macro defined in an included .h2 or .c2 file is visible only in
   that file (every inclusion of it), and in the expansions of that file's own macros, unless the file
   lists it in `#export`.  So a header can't leak its helpers (MIN, MAX...) into its includers. */
static int macro_visible(Macro *m) {   /* 2: inside its owner (wins), 1: visible, 0: hidden */
    if (m->owner < 0) return 1;
    if (pp_ctx) {
        if (pp_file_key[pp_ctx->file] == m->owner) return 2;
        for (Hide *h = pp_ctx->hs; h; h = h->next)  /* a token out of one of the owner's macros */
            if (macros[h->macro].owner == m->owner) return 2;
    }
    return m->exported;
}

static Macro *find_macro(const char *name) {   /* inside a header, its own macros shadow the others */
    Macro *other = NULL;
    for (int i = nmacros - 1; i >= 0; i--)
        if (!macros[i].deleted && strcmp(macros[i].name, name) == 0) {
            int v = macro_visible(&macros[i]);
            if (v == 2) return &macros[i];
            if (v && !other) other = &macros[i];
        }
    return other;
}

static int file_scopes_macros(int file) {          /* an included .h2 / .c2 file of a .c2 build */
    if (!c2_ext || file <= 0 || pp_file_key[file] == pp_main_file) return 0;
    const char *f = pp_files[file];
    size_t n = strlen(f);
    return n > 3 && (!strcmp(f + n - 3, ".h2") || !strcmp(f + n - 3, ".c2"));
}

static Macro *add_macro(const char *name) {
    int owner = pp_ctx && file_scopes_macros(pp_ctx->file) ? pp_file_key[pp_ctx->file] : -1;
    Macro *old = find_macro(name);
    if (old && old->owner == owner) old->deleted = 1;   /* redefinition (another file's macro stays) */
    if (nmacros == cap_macros) {
        cap_macros = cap_macros ? cap_macros * 2 : 256;
        macros = realloc(macros, cap_macros * sizeof *macros);
    }
    Macro *m = &macros[nmacros++];
    memset(m, 0, sizeof *m);
    m->owner = owner;
    snprintf(m->name, sizeof m->name, "%s", name);
    return m;
}

static void define_text_macro(const char *name, const char *value) {   /* -D, predefined */
    Macro *m = add_macro(name);
    PPTok *body = pp_tokenize(value, 0), *t;
    for (t = body; t->kind != PK_EOF; t = t->next) t->bol = 0;
    m->body = body;
}

static int hs_has(Hide *h, int m) { for (; h; h = h->next) if (h->macro == m) return 1; return 0; }
static Hide *hs_add(Hide *h, int m) { Hide *n = malloc(sizeof *n); n->macro = m; n->next = h; return n; }
static Hide *hs_union(Hide *a, Hide *b) { for (; a; a = a->next) if (!hs_has(b, a->macro)) b = hs_add(b, a->macro); return b; }
static Hide *hs_intersect(Hide *a, Hide *b) {
    Hide *r = NULL;
    for (; a; a = a->next) if (hs_has(b, a->macro)) r = hs_add(r, a->macro);
    return r;
}

/* copy list `t` (up to EOF) in front of `rest`, adding hide set `hs` and the position of `pos` */
static PPTok *splice(PPTok *t, PPTok *rest, Hide *hs, PPTok *pos) {
    PPTok head = {0}, *cur = &head;
    for (; t && t->kind != PK_EOF; t = t->next) {
        PPTok *c = copy_tok(t);
        c->hs = hs_union(hs, c->hs);
        if (pos) { c->line = pos->line; c->file = pos->file; c->bol = 0; }
        cur = cur->next = c;
    }
    if (head.next && pos) { head.next->space = pos->space; head.next->bol = pos->bol; }
    cur->next = rest;
    return head.next ? head.next : rest;
}

static PPTok *stringize(PPTok *arg, PPTok *pos) {
    Buf b = {0};
    buf_append(&b, "\"", 1);
    for (PPTok *t = arg; t->kind != PK_EOF; t = t->next) {
        if (t != arg && t->space) buf_append(&b, " ", 1);
        for (int i = 0; i < t->len; i++) {
            if ((t->kind == PK_STR || t->kind == PK_CHAR) && (t->s[i] == '"' || t->s[i] == '\\')) buf_append(&b, "\\", 1);
            buf_append(&b, &t->s[i], 1);
        }
    }
    buf_append(&b, "\"", 1);
    PPTok *r = new_tok(PK_STR, b.p, (int)b.len, pos);
    free(b.p);
    return r;
}

static PPTok *paste(PPTok *a, PPTok *b) {
    Buf buf = {0};
    buf_append(&buf, a->s, a->len);
    buf_append(&buf, b->s, b->len);
    PPTok *t = pp_tokenize(buf.p, a->file);
    if (t->next->kind != PK_EOF) pp_error(a, "pasting \"%s\" and \"%s\" does not give a valid token", a->s, b->s);
    t->line = a->line;
    t->space = a->space;
    t->bol = 0;
    t->hs = a->hs;
    free(buf.p);
    return t;
}

static int expand_macro(PPTok **rest, PPTok *tok);

static PPTok *expand_all(PPTok *t) {   /* fully expand a token list (macro arguments) */
    PPTok head = {0}, *cur = &head;
    while (t->kind != PK_EOF) {
        if (expand_macro(&t, t)) continue;
        cur = cur->next = copy_tok(t);
        t = t->next;
    }
    cur->next = t;
    return head.next;
}

static int param_index(Macro *m, PPTok *t) {
    if (t->kind != PK_IDENT) return -1;
    for (int i = 0; i < m->nparams; i++)
        if (strcmp(m->params[i], t->s) == 0) return i;
    return -1;
}

/* arguments of a function-like macro call; *rparen gets the closing ')' */
static PPTok **read_args(PPTok *lparen, Macro *m, PPTok **rparen, int *nargs) {
    PPTok **args = calloc(m->nparams + 1, sizeof *args);
    PPTok *t = lparen->next;
    int n = 0;
    if (m->nparams == 0 && is_punct(t, ")")) { *rparen = t; *nargs = 0; return args; }
    for (;;) {
        PPTok head = {0}, *cur = &head;
        int depth = 0, last = n == m->nparams - 1 && m->variadic;
        for (;; t = t->next) {
            if (t->kind == PK_EOF) pp_error(lparen, "unterminated call to macro '%s'", m->name);
            if (depth == 0 && (is_punct(t, ")") || (is_punct(t, ",") && !last))) break;
            if (is_punct(t, "(")) depth++;
            else if (is_punct(t, ")")) depth--;
            cur = cur->next = copy_tok(t);
        }
        cur->next = eof_tok(t);
        if (n < m->nparams) args[n] = head.next;
        n++;
        if (is_punct(t, ")")) break;
        t = t->next;
    }
    if (m->variadic && n == m->nparams - 1) { args[n] = eof_tok(t); n++; }   /* f(a) for f(a, ...) */
    if (n != m->nparams) pp_error(lparen, "macro '%s' takes %d argument%s, got %d", m->name, m->nparams, m->nparams == 1 ? "" : "s", n);
    *rparen = t;
    *nargs = n;
    return args;
}

/* C23 __VA_OPT__(x): x when the variadic arguments expand to something, else nothing (a
   placeholder, so ## next to it still works).  Returns the body to substitute. */
static PPTok *va_opt_body(Macro *m, PPTok **args, PPTok **expanded, PPTok *pos) {
    PPTok *t = m->body;
    while (t->kind != PK_EOF && !is_ident(t, "__VA_OPT__")) t = t->next;
    if (t->kind == PK_EOF) return m->body;
    int va = m->nparams - 1;
    if (!expanded[va]) expanded[va] = expand_all(args[va]);
    int present = expanded[va]->kind != PK_EOF;
    PPTok head = {0}, *cur = &head;
    for (t = m->body; t->kind != PK_EOF; t = t->next) {
        if (!is_ident(t, "__VA_OPT__")) { cur = cur->next = copy_tok(t); continue; }
        PPTok *opt = t;
        t = t->next;
        if (!is_punct(t, "(")) pp_error(pos, "'(' expected after __VA_OPT__");
        int depth = 0, any = 0;
        for (t = t->next; t->kind != PK_EOF; t = t->next) {
            if (is_punct(t, "(")) depth++;
            else if (is_punct(t, ")") && depth-- == 0) break;
            if (present) { cur = cur->next = copy_tok(t); if (!any) cur->space = opt->space; any = 1; }
        }
        if (t->kind == PK_EOF) pp_error(pos, "unterminated __VA_OPT__(");
        if (!any) cur = cur->next = new_tok(PK_IDENT, "", 0, opt);   /* placeholder */
    }
    cur->next = eof_tok(pos);
    return head.next;
}

static PPTok *subst(Macro *m, PPTok **args, PPTok *pos) {
    PPTok head = {0}, *cur = &head;
    PPTok **expanded = calloc(m->nparams + 1, sizeof *expanded);
    PPTok *body = m->variadic ? va_opt_body(m, args, expanded, pos) : m->body;
    for (PPTok *t = body; t->kind != PK_EOF; t = t->next) {
        if (is_punct(t, "#")) {                      /* #param */
            int i = param_index(m, t->next);
            if (i < 0) pp_error(pos, "'#' is not followed by a macro parameter");
            cur = cur->next = stringize(args[i], t);
            cur->space = t->space;
            t = t->next;
            continue;
        }
        if (is_punct(t, "##")) {                     /* x ## y */
            if (cur == &head) pp_error(pos, "'##' cannot appear at either end of a macro");
            PPTok *rhs = t->next;
            if (rhs->kind == PK_EOF) pp_error(pos, "'##' cannot appear at either end of a macro");
            int i = param_index(m, rhs);
            if (i >= 0) {
                PPTok *a = args[i];
                /* GNU: , ## __VA_ARGS__ drops the comma when there are no variadic arguments */
                if (m->variadic && i == m->nparams - 1 && is_punct(cur, ",")) {
                    if (a->kind == PK_EOF) {             /* no arguments: drop the comma */
                        PPTok *p = &head;
                        while (p->next != cur) p = p->next;
                        p->next = NULL;
                        cur = p;
                    } else {                             /* arguments: keep the comma, no pasting */
                        if (!expanded[i]) expanded[i] = expand_all(args[i]);
                        for (a = expanded[i]; a->kind != PK_EOF; a = a->next) cur = cur->next = copy_tok(a);
                    }
                } else if (a->kind != PK_EOF) {
                    PPTok *pasted = paste(cur, a);
                    *cur = *pasted;
                    for (a = a->next; a->kind != PK_EOF; a = a->next) cur = cur->next = copy_tok(a);
                }
            } else {
                PPTok *pasted = paste(cur, rhs);
                *cur = *pasted;
            }
            cur->next = NULL;
            t = rhs;
            continue;
        }
        int i = param_index(m, t);
        if (i >= 0) {
            PPTok *a;
            if (is_punct(t->next, "##")) a = args[i];             /* operand of ##: not expanded */
            else {
                if (!expanded[i]) expanded[i] = expand_all(args[i]);
                a = expanded[i];
            }
            int first = 1;
            for (; a->kind != PK_EOF; a = a->next) {
                cur = cur->next = copy_tok(a);
                if (first) cur->space = t->space;
                first = 0;
            }
            if (first && is_punct(t->next, "##")) {      /* empty arg before ##: keep a placeholder */
                PPTok *ph = new_tok(PK_IDENT, "", 0, t);
                cur = cur->next = ph;
            }
            continue;
        }
        cur = cur->next = copy_tok(t);
    }
    /* drop empty placeholders */
    PPTok *r = &head;
    while (r->next) {
        if (r->next->kind == PK_IDENT && r->next->len == 0) r->next = r->next->next;
        else r = r->next;
    }
    PPTok *e = eof_tok(pos);
    r->next = e;
    free(expanded);
    return head.next;
}

static int expand_macro(PPTok **rest, PPTok *tok) {
    if (tok->kind != PK_IDENT) return 0;
    pp_ctx = tok;
    Macro *m = find_macro(tok->s);
    if (!m) return 0;
    int mi = (int)(m - macros);
    if (hs_has(tok->hs, mi)) return 0;
    if (m->builtin) {
        char buf[1100];
        PPTok *r;
        if (m->builtin == 2) {                       /* __FILE__: escape Windows backslashes */
            int k = 0;
            buf[k++] = '"';
            for (const char *f = pp_files[tok->file]; *f && k < (int)sizeof buf - 3; f++) {
                if (*f == '\\' || *f == '"') buf[k++] = '\\';
                buf[k++] = *f;
            }
            buf[k++] = '"';
            buf[k] = '\0';
            r = new_tok(PK_STR, buf, (int)strlen(buf), tok);
        } else {
            snprintf(buf, sizeof buf, "%d", m->builtin == 1 ? tok->line : pp_counter++);
            r = new_tok(PK_NUM, buf, (int)strlen(buf), tok);
        }
        r->next = tok->next;
        *rest = r;
        return 1;
    }
    if (!m->is_func) {
        *rest = splice(m->body, tok->next, hs_add(tok->hs, mi), tok);
        return 1;
    }
    if (!is_punct(tok->next, "(")) return 0;     /* a function-like macro name alone is not a call */
    PPTok *rparen;
    int nargs;
    PPTok **args = read_args(tok->next, m, &rparen, &nargs);
    Hide *hs = hs_add(hs_intersect(tok->hs, rparen->hs), mi);
    PPTok *body = subst(m, args, tok);
    *rest = splice(body, rparen->next, hs, tok);
    free(args);
    return 1;
}

/* ---------------- directives */
static PPTok *line_tokens(PPTok *t, PPTok **next_line) {   /* rest of the directive line, EOF-terminated */
    PPTok head = {0}, *cur = &head;
    for (; !t->bol; t = t->next) cur = cur->next = copy_tok(t);
    cur->next = eof_tok(t);
    *next_line = t;
    return head.next;
}

static PPTok *skip_line(PPTok *t) { while (!t->bol) t = t->next; return t; }

static void read_define(PPTok *t, PPTok *dir) {
    if (t->kind != PK_IDENT) pp_error(dir, "macro name expected after #define");
    if (strcmp(t->s, "defined") == 0) pp_error(t, "'defined' cannot be a macro name");
    Macro *m = add_macro(t->s);
    PPTok *n = t->next;
    if (is_punct(n, "(") && !n->space) {            /* function-like */
        m->is_func = 1;
        n = n->next;
        while (!is_punct(n, ")")) {
            if (m->nparams >= 64) pp_error(n, "too many macro parameters");
            if (is_punct(n, "...")) {
                m->variadic = 1;
                strcpy(m->params[m->nparams++], "__VA_ARGS__");
                n = n->next;
                if (!is_punct(n, ")")) pp_error(n, "')' expected after '...'");
                break;
            }
            if (n->kind != PK_IDENT) pp_error(n, "macro parameter name expected");
            snprintf(m->params[m->nparams++], 64, "%s", n->s);
            n = n->next;
            if (is_punct(n, "...")) {                /* GNU named variadic: args... */
                m->variadic = 1;
                n = n->next;
                if (!is_punct(n, ")")) pp_error(n, "')' expected after '...'");
                break;
            }
            if (is_punct(n, ",")) n = n->next;
            else if (!is_punct(n, ")")) pp_error(n, "',' or ')' expected in macro parameters");
        }
        n = n->next;
    }
    PPTok *rest;
    m->body = line_tokens(n, &rest);
    if (m->body->kind != PK_EOF) m->body->space = 0;
}

/* #if expression */
static PPTok *ce_tok;

static long long pp_char_value(const char *s) {   /* 'a', '\n', L'x', 'ab' */
    while (*s && *s != '\'') s++;
    s++;
    int is_long;
    return char_constant(&s, &is_long);
}

/* values in #if are intmax_t or uintmax_t; unsigned wins in mixed operations */
typedef struct { long long v; int u; } PV;
static PV pv(long long v, int u) { PV r = {v, u}; return r; }

static PV pp_expr(int prec);
static PV pp_comma(void) {
    PV v = pp_expr(0);
    while (is_punct(ce_tok, ",")) { ce_tok = ce_tok->next; v = pp_expr(0); }
    return v;
}

static PV pp_primary(void) {
    PPTok *t = ce_tok;
    if (is_punct(t, "(")) {
        ce_tok = t->next;
        PV v = pp_comma();
        if (!is_punct(ce_tok, ")")) pp_error(ce_tok, "')' expected in #if");
        ce_tok = ce_tok->next;
        return v;
    }
    if (is_punct(t, "!")) { ce_tok = t->next; PV a = pp_primary(); return pv(!a.v, 0); }
    if (is_punct(t, "~")) { ce_tok = t->next; PV a = pp_primary(); return pv(~a.v, a.u); }
    if (is_punct(t, "-")) { ce_tok = t->next; PV a = pp_primary(); return pv((long long)(0ull - (unsigned long long)a.v), a.u); }
    if (is_punct(t, "+")) { ce_tok = t->next; return pp_primary(); }
    ce_tok = t->next;
    if (t->kind == PK_NUM) {
        char *end;
        unsigned long long v = strtoull(t->s, &end, 0);
        int u = v > 0x7FFFFFFFFFFFFFFFull;
        while (*end == 'u' || *end == 'U' || *end == 'l' || *end == 'L') { if (*end == 'u' || *end == 'U') u = 1; end++; }
        if (*end) pp_error(t, "invalid number '%s' in #if", t->s);
        return pv((long long)v, u);
    }
    if (t->kind == PK_CHAR) return pv(pp_char_value(t->s), 0);
    if (t->kind == PK_IDENT) return pv(!strcmp(t->s, "true"), 0);   /* C23 true is 1; other identifiers 0 */
    pp_error(t, "unexpected '%s' in #if", t->kind == PK_EOF ? "end of line" : t->s);
    return pv(0, 0);
}

static int pp_binprec(PPTok *t) {
    static const struct { const char *op; int prec; } ops[] = {
        {"*", 10}, {"/", 10}, {"%", 10}, {"+", 9}, {"-", 9}, {"<<", 8}, {">>", 8}, {"<", 7}, {"<=", 7},
        {">", 7}, {">=", 7}, {"==", 6}, {"!=", 6}, {"&", 5}, {"^", 4}, {"|", 3}, {"&&", 2}, {"||", 1},
    };
    if (t->kind != PK_PUNCT) return 0;
    for (size_t i = 0; i < sizeof ops / sizeof ops[0]; i++)
        if (strcmp(t->s, ops[i].op) == 0) return ops[i].prec;
    return 0;
}

static PV pp_expr(int min_prec) {
    PV a = pp_primary();
    for (;;) {
        PPTok *op = ce_tok;
        if (min_prec == 0 && is_punct(op, "?")) {   /* ?: binds loosest */
            ce_tok = op->next;
            PV x = pp_comma();
            if (!is_punct(ce_tok, ":")) pp_error(ce_tok, "':' expected in #if");
            ce_tok = ce_tok->next;
            PV y = pp_expr(0);
            int u = x.u || y.u;
            return a.v ? pv(x.v, u) : pv(y.v, u);
        }
        int p = pp_binprec(op);
        if (!p || p < (min_prec ? min_prec : 1)) return a;
        ce_tok = op->next;
        PV b = pp_expr(p + 1);
        const char *s = op->s;
        int u = a.u || b.u;
        unsigned long long ua = (unsigned long long)a.v, ub = (unsigned long long)b.v;
        long long r;
        if (!strcmp(s, "*")) r = (long long)(ua * ub);
        else if (!strcmp(s, "/") || !strcmp(s, "%")) {
            if (b.v == 0) pp_error(op, "division by zero in #if");
            if (u) r = s[0] == '/' ? (long long)(ua / ub) : (long long)(ua % ub);
            else r = s[0] == '/' ? a.v / b.v : a.v % b.v;
        }
        else if (!strcmp(s, "+")) r = (long long)(ua + ub);
        else if (!strcmp(s, "-")) r = (long long)(ua - ub);
        else if (!strcmp(s, "<<")) { r = (long long)(ua << (b.v & 63)); u = a.u; }
        else if (!strcmp(s, ">>")) { r = a.u ? (long long)(ua >> (b.v & 63)) : a.v >> (b.v & 63); u = a.u; }
        else if (!strcmp(s, "&")) r = a.v & b.v;
        else if (!strcmp(s, "^")) r = a.v ^ b.v;
        else if (!strcmp(s, "|")) r = a.v | b.v;
        else if (!strcmp(s, "&&")) { r = a.v && b.v; u = 0; }
        else if (!strcmp(s, "||")) { r = a.v || b.v; u = 0; }
        else {                                       /* comparisons */
            int lt = u ? ua < ub : a.v < b.v, eq = a.v == b.v;
            if (!strcmp(s, "<")) r = lt;
            else if (!strcmp(s, "<=")) r = lt || eq;
            else if (!strcmp(s, ">")) r = !lt && !eq;
            else if (!strcmp(s, ">=")) r = !lt;
            else if (!strcmp(s, "==")) r = eq;
            else r = !eq;
            u = 0;
        }
        a = pv(r, u);
    }
}

static char *find_file(PPTok *dir, const char *name, int quoted, char *path, size_t pathsz, size_t *size);
static PPTok *header_name(PPTok *t, PPTok *dir, char *name, size_t n, int *quoted);

/* #embed parameters (C23): limit(n), prefix(...), suffix(...), if_empty(...), also as __limit__ etc. */
typedef struct { long long limit; PPTok *prefix, *suffix, *if_empty; } EmbedParams;

static PPTok *balanced(PPTok **pp, PPTok *dir) {   /* tokens inside (...), EOF-terminated; *pp after ')' */
    PPTok *p = *pp, head = {0}, *cur = &head;
    if (!is_punct(p, "(")) pp_error(dir, "'(' expected after #embed parameter");
    int depth = 0;
    for (p = p->next; p->kind != PK_EOF; p = p->next) {
        if (is_punct(p, "(")) depth++;
        else if (is_punct(p, ")") && depth-- == 0) break;
        cur = cur->next = copy_tok(p);
    }
    if (!is_punct(p, ")")) pp_error(dir, "')' expected in #embed parameter");
    cur->next = eof_tok(p);
    *pp = p->next;
    return head.next;
}

static int param_is(PPTok *p, const char *name) {
    char alt[32];
    snprintf(alt, sizeof alt, "__%s__", name);
    return p->kind == PK_IDENT && (!strcmp(p->s, name) || !strcmp(p->s, alt));
}

/* parse parameters from p up to EOF or (inside __has_embed) the closing ')'; 0 if one is unknown */
static int embed_params(PPTok **pp, PPTok *dir, EmbedParams *ep) {
    PPTok *p = *pp;
    ep->limit = -1;
    ep->prefix = ep->suffix = ep->if_empty = NULL;
    int ok = 1;
    while (p->kind != PK_EOF && !is_punct(p, ")")) {
        if (p->kind != PK_IDENT) pp_error(dir, "#embed parameter expected, not '%s'", p->s);
        PPTok *name = p;
        p = p->next;
        if (param_is(name, "limit")) {
            PPTok *e = balanced(&p, dir);
            PPTok *save = ce_tok;
            ce_tok = expand_all(e);
            ep->limit = pp_comma().v;
            ce_tok = save;
            if (ep->limit < 0) pp_error(dir, "#embed limit must not be negative");
        } else if (param_is(name, "prefix")) ep->prefix = balanced(&p, dir);
        else if (param_is(name, "suffix")) ep->suffix = balanced(&p, dir);
        else if (param_is(name, "if_empty")) ep->if_empty = balanced(&p, dir);
        else {
            ok = 0;                                  /* unknown: skip it, and its (...) if any */
            if (is_punct(p, "(")) balanced(&p, dir);
        }
    }
    *pp = p;
    return ok;
}

static long long eval_if(PPTok *t, PPTok **next_line, PPTok *dir) {
    PPTok *line = line_tokens(t, next_line);
    /* defined X / defined(X) first, then expand macros */
    PPTok head = {0}, *cur = &head;
    for (PPTok *p = line; p->kind != PK_EOF; p = p->next) {
        if (is_ident(p, "defined")) {
            PPTok *start = p;
            int paren = is_punct(p->next, "(");
            if (paren) p = p->next;
            p = p->next;
            if (p->kind != PK_IDENT) pp_error(start, "macro name expected after 'defined'");
            int d = find_macro(p->s) != NULL;
            if (paren) { p = p->next; if (!is_punct(p, ")")) pp_error(start, "')' expected after defined("); }
            cur = cur->next = new_tok(PK_NUM, d ? "1" : "0", 1, start);
            continue;
        }
        if (is_ident(p, "__has_include") || is_ident(p, "__has_embed")) {   /* C23 */
            PPTok *start = p;
            int embed = p->s[6] == 'e', quoted, r;
            char name[1024], path[1200];
            p = p->next;
            if (!is_punct(p, "(")) pp_error(start, "'(' expected after %s", start->s);
            p = header_name(p->next, dir, name, sizeof name, &quoted);
            size_t size = 0;
            char *data = find_file(dir, name, quoted, path, sizeof path, &size);
            if (!embed) r = data || builtin_header(name);
            else {
                EmbedParams ep;
                int ok = embed_params(&p, dir, &ep);
                r = !data || !ok ? 0 : (size == 0 || ep.limit == 0) ? 2 : 1;
            }
            free(data);
            if (!is_punct(p, ")")) pp_error(start, "')' expected after %s(...", start->s);
            cur = cur->next = new_tok(PK_NUM, r == 2 ? "2" : r ? "1" : "0", 1, start);
            continue;
        }
        cur = cur->next = copy_tok(p);
    }
    cur->next = eof_tok(dir);
    if (!head.next) pp_error(dir, "#if with no expression");
    ce_tok = expand_all(head.next);
    PV v = pp_comma();
    if (ce_tok->kind != PK_EOF) pp_error(ce_tok, "extra '%s' in #if", ce_tok->s);
    return v.v;
}

/* skip a false group: stop at the #elif/#else/#endif that belongs to it */
static PPTok *skip_cond(PPTok *t) {
    int depth = 0;
    while (t->kind != PK_EOF) {
        if (t->bol && is_punct(t, "#") && t->next->kind == PK_IDENT && !t->next->bol) {
            const char *d = t->next->s;
            if (!strcmp(d, "if") || !strcmp(d, "ifdef") || !strcmp(d, "ifndef")) depth++;
            else if (!strcmp(d, "endif")) { if (depth == 0) return t; depth--; }
            else if (depth == 0 && (!strcmp(d, "elif") || !strcmp(d, "else") || !strcmp(d, "elifdef") || !strcmp(d, "elifndef"))) return t;
        }
        t = t->next;
    }
    return t;
}

typedef struct { int taken, in_else; PPTok *tok; } Cond;
static Cond conds[256];
static int nconds;


static char *dir_of(const char *path) {
    char *d = malloc(strlen(path) + 2);
    strcpy(d, path);
    char *s1 = strrchr(d, '/'), *s2 = strrchr(d, '\\');
    char *s = s1 > s2 ? s1 : s2;
    if (s) s[1] = '\0';
    else d[0] = '\0';
    return d;
}

static char *read_file_n(const char *path, size_t *size) {   /* NUL-terminated; *size = bytes read */
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len < 0) len = 0;
    char *s = malloc(len + 1);
    size_t n = fread(s, 1, len, f);
    s[n] = '\0';
    fclose(f);
    if (size) *size = n;
    return s;
}

static char *read_text_file(const char *path) { return read_file_n(path, NULL); }

/* find "name" (next to the including file, then -I dirs) or <name> (-I dirs); NULL if not on disk.
   path receives where it was found. */
static char *find_file(PPTok *dir, const char *name, int quoted, char *path, size_t pathsz, size_t *size) {
    char *text = NULL;
    if (quoted) {
        char *d = dir_of(pp_files[dir->file]);
        snprintf(path, pathsz, "%s%s", d, name);
        free(d);
        text = read_file_n(path, size);
    }
    for (int i = 0; !text && i < ninclude_dirs; i++) {
        snprintf(path, pathsz, "%s/%s", include_dirs[i], name);
        text = read_file_n(path, size);
    }
    return text;
}

/* "file" or <file> at t (a directive's or __has_include's operand); returns the token after it */
static PPTok *header_name(PPTok *t, PPTok *dir, char *name, size_t n, int *quoted) {
    if (t->kind == PK_STR) {
        *quoted = 1;
        snprintf(name, n, "%.*s", t->len - 2, t->s + 1);
        return t->next;
    }
    if (is_punct(t, "<")) {
        *quoted = 0;
        name[0] = '\0';
        PPTok *p = t->next;
        for (; p->kind != PK_EOF && !p->bol && !is_punct(p, ">"); p = p->next) strncat(name, p->s, n - strlen(name) - 1);
        if (!is_punct(p, ">")) pp_error(dir, "'>' expected after <%s", name);
        return p->next;
    }
    pp_error(dir, "\"file\" or <file> expected");
    return t;
}

static int add_pp_file(const char *name) {
    if (npp_files >= 1024) error("too many included files");
    pp_files[npp_files] = malloc(strlen(name) + 1);
    strcpy(pp_files[npp_files], name);
    pp_file_key[npp_files] = npp_files;
    for (int i = 0; i < npp_files; i++) if (!strcmp(pp_files[i], name)) { pp_file_key[npp_files] = i; break; }
    return npp_files++;
}

static void pragma_once_file(int file) {
    if (npragma_once >= 256) return;
    pragma_once[npragma_once] = malloc(strlen(pp_files[file]) + 1);
    strcpy(pragma_once[npragma_once++], pp_files[file]);
}

static PPTok *include_file(PPTok *after, PPTok *dir, const char *name, int quoted) {
    char path[1200];
    char *text = find_file(dir, name, quoted, path, sizeof path, NULL);   /* "x.h": next to the includer, then -I */
    if (!text) {                                     /* the built-in C library headers */
        const char *b = builtin_header(name);
        if (!b) {
            if (!quoted) { diag("%s:%d: note: <%s> is not a built-in header, ignored\n", pp_files[dir->file], dir->line, name); return after; }
            pp_error(dir, "cannot open include file \"%s\"", name);
        }
        snprintf(path, sizeof path, "<%s>", name);
        text = malloc(strlen(b) + 1);
        strcpy(text, b);
    }
    for (int i = 0; i < npragma_once; i++) if (!strcmp(pragma_once[i], path)) { free(text); return after; }
    if (path[0] == '<' && npragma_once < 256) {     /* built-in headers are included once */
        pragma_once[npragma_once] = malloc(strlen(path) + 1);
        strcpy(pragma_once[npragma_once++], path);
    }
    if (npp_files > 900) pp_error(dir, "#include nested too deeply");
    PPTok *t = pp_tokenize(text, add_pp_file(path));
    free(text);
    if (t->kind == PK_EOF) return after;
    PPTok *last = t;
    while (last->next->kind != PK_EOF) last = last->next;
    last->next = after;
    return t;
}

static PPTok *preprocess_tokens(PPTok *tok) {
    PPTok head = {0}, *cur = &head;
    while (tok->kind != PK_EOF) {
        pp_ctx = tok;
        if (!(tok->bol && is_punct(tok, "#"))) {
            if (is_ident(tok, "_Pragma") && is_punct(tok->next, "(") && tok->next->next->kind == PK_STR
                && is_punct(tok->next->next->next, ")")) {     /* C99 _Pragma("..."): like #pragma */
                PPTok *str = tok->next->next;
                if (str->len == 6 && !strncmp(str->s, "\"once\"", 6)) pragma_once_file(tok->file);
                tok = str->next->next;
                continue;
            }
            if (expand_macro(&tok, tok)) continue;
            cur = cur->next = tok;
            tok = tok->next;
            continue;
        }
        PPTok *dir = tok;
        tok = tok->next;
        if (tok->bol) continue;                      /* null directive */
        if (tok->kind != PK_IDENT && tok->kind != PK_NUM) pp_error(tok, "invalid preprocessing directive");
        const char *d = tok->s;
        PPTok *t = tok->next, *next;
        if (!strcmp(d, "define")) { read_define(t, dir); tok = skip_line(t); }
        else if (!strcmp(d, "undef")) {
            if (t->kind != PK_IDENT) pp_error(dir, "macro name expected after #undef");
            Macro *m = find_macro(t->s);
            if (m) m->deleted = 1;
            tok = skip_line(t);
        } else if (!strcmp(d, "include")) {
            PPTok *line = line_tokens(t, &next);
            if (line->kind == PK_IDENT) line = expand_all(line);    /* #include MACRO */
            char name[1024];
            int quoted;
            if (line->kind == PK_STR) {
                quoted = 1;
                snprintf(name, sizeof name, "%.*s", line->len - 2, line->s + 1);
            } else if (is_punct(line, "<")) {
                quoted = 0;
                name[0] = '\0';
                PPTok *p = line->next;
                for (; p->kind != PK_EOF && !is_punct(p, ">"); p = p->next) strncat(name, p->s, sizeof name - strlen(name) - 1);
                if (p->kind == PK_EOF) pp_error(dir, "'>' expected in #include");
            } else {
                pp_error(dir, "#include expects \"file\" or <file>");
                quoted = 0;
            }
            tok = include_file(next, dir, name, quoted);
        } else if (!strcmp(d, "embed")) {           /* C23: the file's bytes as 1,2,3,... */
            PPTok *line = line_tokens(t, &next);
            if (line->kind == PK_IDENT) line = expand_all(line);
            char name[1024], path[1200];
            int quoted;
            PPTok *p = header_name(line, dir, name, sizeof name, &quoted);
            EmbedParams ep;
            if (!embed_params(&p, dir, &ep)) pp_error(dir, "unknown #embed parameter");
            if (p->kind != PK_EOF) pp_error(dir, "unexpected '%s' in #embed", p->s);
            size_t size = 0;
            unsigned char *data = (unsigned char *)find_file(dir, name, quoted, path, sizeof path, &size);
            if (!data) pp_error(dir, "cannot open embed file \"%s\"", name);
            if (ep.limit >= 0 && (size_t)ep.limit < size) size = (size_t)ep.limit;
            PPTok ehead = {0}, *ec = &ehead;
            #define EMBED_TOKENS(list) for (PPTok *q = (list); q && q->kind != PK_EOF; q = q->next) ec = ec->next = copy_tok(q)
            if (size == 0) { EMBED_TOKENS(ep.if_empty); }
            else {
                EMBED_TOKENS(ep.prefix);
                for (size_t i = 0; i < size; i++) {
                    char num[8];
                    int len = snprintf(num, sizeof num, "%u", data[i]);
                    if (i) { ec = ec->next = new_tok(PK_PUNCT, ",", 1, dir); ec->bol = 0; ec->space = 0; }
                    ec = ec->next = new_tok(PK_NUM, num, len, dir);
                    ec->bol = 0;
                    ec->space = i > 0;
                }
                EMBED_TOKENS(ep.suffix);
            }
            #undef EMBED_TOKENS
            free(data);
            for (PPTok *q = ehead.next; q; q = q->next) q->bol = 0;
            ec->next = next;
            tok = ehead.next ? ehead.next : next;
        } else if (!strcmp(d, "if") || !strcmp(d, "ifdef") || !strcmp(d, "ifndef")) {
            long long v;
            if (!strcmp(d, "if")) v = eval_if(t, &next, dir);
            else {
                if (t->kind != PK_IDENT) pp_error(dir, "macro name expected after #%s", d);
                v = (find_macro(t->s) != NULL) == (d[2] == 'd');
                next = skip_line(t);
            }
            if (nconds >= 256) pp_error(dir, "#if nested too deeply");
            conds[nconds].taken = v != 0;
            conds[nconds].in_else = 0;
            conds[nconds++].tok = dir;
            tok = v ? next : skip_cond(next);
        } else if (!strcmp(d, "elif") || !strcmp(d, "elifdef") || !strcmp(d, "elifndef")) {   /* C23: #elifdef X */
            if (nconds == 0 || conds[nconds - 1].in_else) pp_error(dir, "#%s without #if", d);
            Cond *c = &conds[nconds - 1];
            if (c->taken) tok = skip_cond(skip_line(t));
            else {
                long long v;
                if (d[4] == '\0') v = eval_if(t, &next, dir);
                else {
                    if (t->kind != PK_IDENT) pp_error(dir, "macro name expected after #%s", d);
                    v = (find_macro(t->s) != NULL) == (d[4] == 'd');
                    next = skip_line(t);
                }
                c->taken = v != 0;
                tok = v ? next : skip_cond(next);
            }
        } else if (!strcmp(d, "else")) {
            if (nconds == 0 || conds[nconds - 1].in_else) pp_error(dir, "#else without #if");
            Cond *c = &conds[nconds - 1];
            c->in_else = 1;
            next = skip_line(t);
            tok = c->taken ? skip_cond(next) : next;
            c->taken = 1;
        } else if (!strcmp(d, "endif")) {
            if (nconds == 0) pp_error(dir, "#endif without #if");
            nconds--;
            tok = skip_line(t);
        } else if (!strcmp(d, "error") || !strcmp(d, "warning")) {
            Buf b = {0};
            for (PPTok *p = t; !p->bol; p = p->next) { if (b.len) buf_append(&b, " ", 1); buf_append(&b, p->s, p->len); }
            if (d[0] == 'e') pp_error(dir, "#error %s", b.p ? b.p : "");
            diag("%s:%d: warning: %s\n", pp_files[dir->file], dir->line, b.p ? b.p : "");
            tok = skip_line(t);
        } else if (!strcmp(d, "pragma")) {
            if (is_ident(t, "once")) pragma_once_file(dir->file);
            tok = skip_line(t);
        } else if (!strcmp(d, "export") && c2_ext) {   /* C²: #export NAME... or #export define ... */
            if (is_ident(t, "define")) {
                read_define(t->next, dir);
                Macro *m = find_macro(t->next->s);
                if (m) m->exported = 1;
            } else
                for (PPTok *p = t; !p->bol; p = p->next) {
                    Macro *m = p->kind == PK_IDENT ? find_macro(p->s) : NULL;
                    if (!m) pp_error(p, "#export: '%s' is not a macro defined here", p->s);
                    m->exported = 1;
                }
            tok = skip_line(t);
        } else if (!strcmp(d, "line") || tok->kind == PK_NUM) {
            PPTok *line = line_tokens(tok->kind == PK_NUM ? tok : t, &next);
            line = expand_all(line);
            if (line->kind != PK_NUM) pp_error(dir, "line number expected");
            int delta = atoi(line->s) - (dir->line + 1);
            int file = dir->file;
            if (line->next->kind == PK_STR) {
                char nm[1024];
                snprintf(nm, sizeof nm, "%.*s", line->next->len - 2, line->next->s + 1);
                file = add_pp_file(nm);
            }
            for (PPTok *p = next; p->kind != PK_EOF && p->file == dir->file; p = p->next) { p->line += delta; p->file = file; }
            tok = next;
        } else {
            pp_error(dir, "unknown directive #%s", d);
        }
    }
    if (nconds) pp_error(conds[nconds - 1].tok, "unterminated #if");
    cur->next = tok;
    return head.next;
}

static void predefine(void) {
    static const char *defs[][2] = {
        {"__STDC__", "1"}, {"__STDC_VERSION__", "201112L"}, {"__STDC_HOSTED__", "1"}, {"__C2__", "1"},
        {"__has_include", "__has_include"}, {"__has_embed", "__has_embed"},   /* so #ifdef finds them */
        {"__STDC_EMBED_NOT_FOUND__", "0"}, {"__STDC_EMBED_FOUND__", "1"}, {"__STDC_EMBED_EMPTY__", "2"},
        {"__CHAR_BIT__", "8"}, {"__SIZEOF_SHORT__", "2"}, {"__SIZEOF_INT__", "4"}, {"__SIZEOF_LONG__", "8"},
        {"__SIZEOF_LONG_LONG__", "8"}, {"__SIZEOF_POINTER__", "8"}, {"__SIZEOF_FLOAT__", "4"},
        {"__SIZEOF_DOUBLE__", "8"}, {"__LP64__", "1"}, {"__INT_MAX__", "2147483647"},
        {"__LONG_MAX__", "9223372036854775807L"}, {"__ORDER_LITTLE_ENDIAN__", "1234"},
        {"__ORDER_BIG_ENDIAN__", "4321"}, {"__BYTE_ORDER__", "__ORDER_LITTLE_ENDIAN__"},
#if defined(_WIN32)
        {"_WIN32", "1"}, {"_WIN64", "1"},
#elif defined(__APPLE__)
        {"__APPLE__", "1"}, {"__MACH__", "1"}, {"__unix__", "1"},
#else
        {"__linux__", "1"}, {"__linux", "1"}, {"__unix__", "1"}, {"__unix", "1"},
#endif
#if defined(__x86_64__) || defined(_M_X64)
        {"__x86_64__", "1"}, {"__x86_64", "1"}, {"__amd64__", "1"},
#elif defined(__aarch64__) || defined(_M_ARM64)
        {"__aarch64__", "1"},
#endif
    };
    for (size_t i = 0; i < sizeof defs / sizeof defs[0]; i++) define_text_macro(defs[i][0], defs[i][1]);
    static const char *builtins[] = {"__LINE__", "__FILE__", "__COUNTER__"};
    for (int i = 0; i < 3; i++) add_macro(builtins[i])->builtin = i + 1;
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    char b[64];
    static const char *mon[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    snprintf(b, sizeof b, "\"%s %2d %d\"", mon[tm->tm_mon], tm->tm_mday, tm->tm_year + 1900);
    define_text_macro("__DATE__", b);
    snprintf(b, sizeof b, "\"%02d:%02d:%02d\"", tm->tm_hour, tm->tm_min, tm->tm_sec);
    define_text_macro("__TIME__", b);
}

/* whole pipeline: built-in base header + the file -> text with line markers */
static char *preprocess_source(const char *text, const char *path, const char *base_prelude) {
    add_pp_file("<built-in>");                      /* file 0 */
    predefine();
    for (int i = 0; i < npending_defines; i++) define_text_macro(pending_defines[i][0], pending_defines[i][1]);
    PPTok *base = pp_tokenize(base_prelude, 0);
    pp_main_file = add_pp_file(path);
    PPTok *user = pp_tokenize(text, pp_main_file);
    PPTok *all = base;
    if (all->kind == PK_EOF) all = user;
    else { PPTok *l = all; while (l->next->kind != PK_EOF) l = l->next; l->next = user; }
    PPTok *out = preprocess_tokens(all);

    Buf b = {0};
    int out_file = -1, out_line = 0;
    for (PPTok *t = out; t->kind != PK_EOF; t = t->next) {
        if (t->file != out_file || t->line < out_line || t->line > out_line + 8) {
            buf_emit(&b, "\n# %d \"%s\"\n", t->line, pp_files[t->file]);
            out_file = t->file;
            out_line = t->line;
        } else {
            while (out_line < t->line) { buf_append(&b, "\n", 1); out_line++; }
        }
        buf_append(&b, " ", 1);
        buf_append(&b, t->s, t->len);
    }
    buf_append(&b, "\n", 1);
    return b.p;
}

/* ============================================================ BACK END
 * One code generator, two back ends.  The X-macro below is the single list of opcodes. */
static int use_api = 0;   /* 1: MIR C API, 0: MIR text */

#define INSN_LIST(X)                                                                              \
    X(MOV, "mov") X(FMOV, "fmov") X(DMOV, "dmov")                                                \
    X(EXT8, "ext8") X(EXT16, "ext16") X(EXT32, "ext32")                                           \
    X(UEXT8, "uext8") X(UEXT16, "uext16") X(UEXT32, "uext32")                                     \
    X(I2F, "i2f") X(I2D, "i2d") X(UI2F, "ui2f") X(UI2D, "ui2d") X(F2I, "f2i") X(D2I, "d2i")       \
    X(F2D, "f2d") X(D2F, "d2f")                                                                   \
    X(NEG, "neg") X(FNEG, "fneg") X(DNEG, "dneg")                                                 \
    X(ADD, "add") X(SUB, "sub") X(MUL, "mul") X(DIV, "div") X(UDIV, "udiv")                       \
    X(MOD, "mod") X(UMOD, "umod")                                                                 \
    X(FADD, "fadd") X(FSUB, "fsub") X(FMUL, "fmul") X(FDIV, "fdiv")                               \
    X(DADD, "dadd") X(DSUB, "dsub") X(DMUL, "dmul") X(DDIV, "ddiv")                               \
    X(AND, "and") X(OR, "or") X(XOR, "xor") X(LSH, "lsh") X(RSH, "rsh") X(URSH, "ursh")           \
    X(EQ, "eq") X(NE, "ne") X(LT, "lt") X(LE, "le") X(GT, "gt") X(GE, "ge")                       \
    X(ULT, "ult") X(ULE, "ule") X(UGT, "ugt") X(UGE, "uge")                                       \
    X(FEQ, "feq") X(FNE, "fne") X(FLT, "flt") X(FLE, "fle") X(FGT, "fgt") X(FGE, "fge")           \
    X(DEQ, "deq") X(DNE, "dne") X(DLT, "dlt") X(DLE, "dle") X(DGT, "dgt") X(DGE, "dge")           \
    X(ADDS, "adds") X(SUBS, "subs") X(MULS, "muls") X(DIVS, "divs") X(UDIVS, "udivs")              \
    X(MODS, "mods") X(UMODS, "umods") X(ANDS, "ands") X(ORS, "ors") X(XORS, "xors")                \
    X(LSHS, "lshs") X(RSHS, "rshs") X(URSHS, "urshs") X(NEGS, "negs")                             \
    X(EQS, "eqs") X(NES, "nes") X(LTS, "lts") X(LES, "les") X(GTS, "gts") X(GES, "ges")           \
    X(ULTS, "ults") X(ULES, "ules") X(UGTS, "ugts") X(UGES, "uges")                               \
    X(VA_START, "va_start") X(VA_ARG, "va_arg") X(VA_END, "va_end")                               \
    X(JMP, "jmp") X(BT, "bt") X(BF, "bf") X(BEQ, "beq") X(BNE, "bne") X(BLT, "blt") X(BLE, "ble")    \
    X(BGT, "bgt") X(BGE, "bge") X(UBLT, "ublt") X(UBLE, "uble") X(UBGT, "ubgt") X(UBGE, "ubge")     \
    X(BEQS, "beqs") X(BNES, "bnes") X(BLTS, "blts") X(BLES, "bles") X(BGTS, "bgts") X(BGES, "bges") \
    X(UBLTS, "ublts") X(UBLES, "ubles") X(UBGTS, "ubgts") X(UBGES, "ubges")                         \
    X(FBEQ, "fbeq") X(FBNE, "fbne") X(FBLT, "fblt") X(FBLE, "fble") X(FBGT, "fbgt") X(FBGE, "fbge") \
    X(DBEQ, "dbeq") X(DBNE, "dbne") X(DBLT, "dblt") X(DBLE, "dble") X(DBGT, "dbgt") X(DBGE, "dbge") \
    X(CALL, "call") X(RET, "ret") X(ALLOCA, "alloca") X(BSTART, "bstart") X(BEND, "bend")        \
    X(SWITCH, "switch")

typedef enum {
#define X(name, text) I_##name,
    INSN_LIST(X)
#undef X
    I_COUNT
} Insn;

static const char *insn_text[] = {
#define X(name, text) text,
    INSN_LIST(X)
#undef X
};

#ifndef NO_MIR
static const MIR_insn_code_t insn_mir[] = {
#define X(name, text) MIR_##name,
    INSN_LIST(X)
#undef X
};
#endif

/* MT_BLKn / MT_RBLK: a struct passed by value (MIR block types; disp holds the struct size) */
typedef enum { MT_I8, MT_U8, MT_I16, MT_U16, MT_I32, MT_U32, MT_I64, MT_F, MT_D,
               MT_BLK0, MT_BLK1, MT_BLK2, MT_BLK3, MT_BLK4, MT_RBLK } MemT;
static const char *mt_text[] = {"i8", "u8", "i16", "u16", "i32", "u32", "i64", "f", "d",
                                "blk0", "blk1", "blk2", "blk3", "blk4", "rblk"};
#ifndef NO_MIR
static const MIR_type_t mt_mir[] = {MIR_T_I8, MIR_T_U8, MIR_T_I16, MIR_T_U16, MIR_T_I32, MIR_T_U32,
                                    MIR_T_I64, MIR_T_F, MIR_T_D, MIR_T_BLK, MIR_T_BLK + 1, MIR_T_BLK + 2,
                                    MIR_T_BLK + 3, MIR_T_BLK + 4, MIR_T_RBLK};
#endif

/* register kinds: MIR registers are i64, f or d */
typedef enum { RK_I, RK_F, RK_D } RegKind;
static const char *rk_text[] = {"i64", "f", "d"};
#ifndef NO_MIR
static const MIR_type_t rk_mir[] = {MIR_T_I64, MIR_T_F, MIR_T_D};
#endif

typedef enum { OK_REG, OK_INT, OK_FLT, OK_DBL, OK_MEM, OK_REF, OK_LABEL } OpKind;
typedef struct {
    OpKind k;
    int r;            /* REG: reg index; MEM: base reg; LABEL: label index */
    long long i;      /* INT */
    double d;         /* FLT, DBL */
    MemT mt;          /* MEM */
    int disp;         /* MEM */
    int ix, sc;       /* MEM: index reg + 1 (0 = none) and scale */
    char ref[40];     /* REF: item name */
} Opnd;

static Opnd R(int r)                    { Opnd o; memset(&o, 0, sizeof o); o.k = OK_REG; o.r = r; return o; }
static Opnd IMM(long long i)            { Opnd o; memset(&o, 0, sizeof o); o.k = OK_INT; o.i = i; return o; }
static Opnd FIMM(double d)              { Opnd o; memset(&o, 0, sizeof o); o.k = OK_FLT; o.d = (float)d; return o; }
static Opnd DIMM(double d)              { Opnd o; memset(&o, 0, sizeof o); o.k = OK_DBL; o.d = d; return o; }
static Opnd MEM(MemT mt, int d, int b)  { Opnd o; memset(&o, 0, sizeof o); o.k = OK_MEM; o.mt = mt; o.disp = d; o.r = b; return o; }
static Opnd MEM_ANY(MemT mt)            { Opnd o = MEM(mt, 0, 0); o.r = -1; return o; }   /* type-only operand (va_arg) */
static Opnd LBL(int l)                  { Opnd o; memset(&o, 0, sizeof o); o.k = OK_LABEL; o.r = l; return o; }
static Opnd REF(const char *name)       { Opnd o; memset(&o, 0, sizeof o); o.k = OK_REF; snprintf(o.ref, sizeof o.ref, "%s", name); return o; }

/* per-function registers and labels */
typedef struct {
    char name[48];
    RegKind rk;
#ifndef NO_MIR
    MIR_reg_t mreg;
#endif
} Reg;

#define MAX_REGS 16384
#define MAX_LABELS 16384
static Reg regs[MAX_REGS];
static int nregs, nlabels, temp_counter;
static int label_base;   /* text mode: MIR text labels are module-wide, so number them across functions */

static Buf out_text;                   /* text mode: the whole module */
static Buf fn_head, fn_pro, fn_body;   /* text mode: current function's header, prologue, body.  Buffered so
                                   data items (strings, protos) can be emitted while inside a function. */
static int string_counter;

#ifndef NO_MIR
static MIR_context_t ctx;
static MIR_module_t mir_module;
static MIR_item_t cur_fn;
static MIR_insn_t prologue_last;   /* allocas are inserted after this one, ahead of the body */
static MIR_insn_t labels[MAX_LABELS];
typedef struct { char name[40]; MIR_item_t item; } ItemEnt;
static ItemEnt items[2048];
static int nitems;

static void register_item(const char *name, MIR_item_t it) {
    for (int i = 0; i < nitems; i++)
        if (strcmp(items[i].name, name) == 0) { items[i].item = it; return; }
    if (nitems >= 2048) error("too many items");
    snprintf(items[nitems].name, sizeof items[nitems].name, "%s", name);
    items[nitems++].item = it;
}

static MIR_item_t find_item(const char *name) {
    for (int i = 0; i < nitems; i++)
        if (strcmp(items[i].name, name) == 0) return items[i].item;
    error("internal: unknown MIR item '%s'", name);
    return NULL;
}

static MIR_op_t mir_opnd(const Opnd *o) {
    switch (o->k) {
    case OK_REG: return MIR_new_reg_op(ctx, regs[o->r].mreg);
    case OK_INT: return MIR_new_int_op(ctx, o->i);
    case OK_FLT: return MIR_new_float_op(ctx, (float)o->d);
    case OK_DBL: return MIR_new_double_op(ctx, o->d);
    case OK_MEM:
        if (o->r < 0) return MIR_new_mem_op(ctx, mt_mir[o->mt], 0, 0, 0, 1);
        return MIR_new_mem_op(ctx, mt_mir[o->mt], o->disp, regs[o->r].mreg,
                                       o->ix ? regs[o->ix - 1].mreg : 0, o->ix ? o->sc : 1);
    case OK_REF: return MIR_new_ref_op(ctx, find_item(o->ref));
    default: return MIR_new_label_op(ctx, labels[o->r]);
    }
}
#endif

static void fmt_double(char *b, double d) {
    sprintf(b, "%.17g", d);
    if (!strpbrk(b, ".eEni")) strcat(b, ".0");
}

static void text_opnd(Buf *b, const Opnd *o) {
    char tmp[64];
    switch (o->k) {
    case OK_REG: buf_emit(b, "%s", regs[o->r].name); break;
    case OK_INT: buf_emit(b, "%lld", o->i); break;
    case OK_FLT:
        sprintf(tmp, "%.9g", o->d);
        if (!strpbrk(tmp, ".eEni")) strcat(tmp, ".0");
        buf_emit(b, "%sf", tmp);                      /* MIR text: 1.5f is a float immediate */
        break;
    case OK_DBL: fmt_double(tmp, o->d); buf_emit(b, "%s", tmp); break;
    case OK_MEM:
        if (o->r < 0) { buf_emit(b, "%s:0", mt_text[o->mt]); break; }
        if (o->disp) buf_emit(b, "%s:%d(%s", mt_text[o->mt], o->disp, regs[o->r].name);
        else buf_emit(b, "%s:(%s", mt_text[o->mt], regs[o->r].name);
        if (o->ix) buf_emit(b, ", %s, %d", regs[o->ix - 1].name, o->sc);
        buf_emit(b, ")");
        break;
    case OK_REF: buf_emit(b, "%s", o->ref); break;
    case OK_LABEL: buf_emit(b, "L%d", label_base + o->r); break;
    }
}

/* compare-and-branch fusion (peephole): `lts t, a, b` directly followed by `bf L, t`, where t is a
   temporary, becomes `bges L, a, b` */
static struct {
    int valid, dst;
    Insn code;
    Opnd a, b;
    size_t text_pos;
#ifndef NO_MIR
    MIR_insn_t insn;
#endif
} last_cmp;

static Insn fused_branch(Insn cmp, int on_true) {
    static const struct { Insn cmp, if_true, if_false; } tab[] = {
        {I_EQ, I_BEQ, I_BNE}, {I_NE, I_BNE, I_BEQ}, {I_LT, I_BLT, I_BGE}, {I_LE, I_BLE, I_BGT},
        {I_GT, I_BGT, I_BLE}, {I_GE, I_BGE, I_BLT}, {I_ULT, I_UBLT, I_UBGE}, {I_ULE, I_UBLE, I_UBGT},
        {I_UGT, I_UBGT, I_UBLE}, {I_UGE, I_UBGE, I_UBLT},
        {I_EQS, I_BEQS, I_BNES}, {I_NES, I_BNES, I_BEQS}, {I_LTS, I_BLTS, I_BGES}, {I_LES, I_BLES, I_BGTS},
        {I_GTS, I_BGTS, I_BLES}, {I_GES, I_BGES, I_BLTS}, {I_ULTS, I_UBLTS, I_UBGES}, {I_ULES, I_UBLES, I_UBGTS},
        {I_UGTS, I_UBGTS, I_UBLES}, {I_UGES, I_UBGES, I_UBLTS},
        /* floating point: "not less" is not "greater or equal" when NaNs are around */
        {I_FEQ, I_FBEQ, I_FBNE}, {I_FNE, I_FBNE, I_FBEQ}, {I_FLT, I_FBLT, I_COUNT}, {I_FLE, I_FBLE, I_COUNT},
        {I_FGT, I_FBGT, I_COUNT}, {I_FGE, I_FBGE, I_COUNT},
        {I_DEQ, I_DBEQ, I_DBNE}, {I_DNE, I_DBNE, I_DBEQ}, {I_DLT, I_DBLT, I_COUNT}, {I_DLE, I_DBLE, I_COUNT},
        {I_DGT, I_DBGT, I_COUNT}, {I_DGE, I_DBGE, I_COUNT},
    };
    for (size_t i = 0; i < sizeof tab / sizeof tab[0]; i++)
        if (tab[i].cmp == cmp) return on_true ? tab[i].if_true : tab[i].if_false;
    return I_COUNT;
}

static int is_compare_insn(Insn c) { return fused_branch(c, 1) != I_COUNT; }

static void emit_raw(int prologue, Insn code, const Opnd *ops, int n);
static int new_label(void);
static void place_label(int l);

static void emit_ops(int prologue, Insn code, const Opnd *ops, int n) {
    if (!prologue && (code == I_BT || code == I_BF) && last_cmp.valid && ops[1].k == OK_REG && ops[1].r == last_cmp.dst) {
        Insn f = fused_branch(last_cmp.code, code == I_BT);
        if (f != I_COUNT) {
            if (!use_api) { fn_body.len = last_cmp.text_pos; fn_body.p[fn_body.len] = '\0'; }
#ifndef NO_MIR
            else MIR_remove_insn(ctx, cur_fn, last_cmp.insn);
#endif
            Opnd fops[3] = {ops[0], last_cmp.a, last_cmp.b};
            last_cmp.valid = 0;
            emit_raw(0, f, fops, 3);
            return;
        }
        Insn ft = fused_branch(last_cmp.code, 1);      /* fp "branch if not less": over a jump */
        if (code == I_BF && ft != I_COUNT) {
            if (!use_api) { fn_body.len = last_cmp.text_pos; fn_body.p[fn_body.len] = '\0'; }
#ifndef NO_MIR
            else MIR_remove_insn(ctx, cur_fn, last_cmp.insn);
#endif
            int skip = new_label();
            Opnd fops[3] = {LBL(skip), last_cmp.a, last_cmp.b};
            last_cmp.valid = 0;
            emit_raw(0, ft, fops, 3);
            Opnd j[1] = {ops[0]};
            emit_raw(0, I_JMP, j, 1);
            place_label(skip);
            return;
        }
    }
    size_t pos = fn_body.len;
    emit_raw(prologue, code, ops, n);
    if (prologue) return;
    last_cmp.valid = is_compare_insn(code) && ops[0].k == OK_REG && regs[ops[0].r].name[0] == '_';
    if (last_cmp.valid) {
        last_cmp.code = code;
        last_cmp.dst = ops[0].r;
        last_cmp.a = ops[1];
        last_cmp.b = ops[2];
        last_cmp.text_pos = pos;
#ifndef NO_MIR
        if (use_api) last_cmp.insn = DLIST_TAIL(MIR_insn_t, cur_fn->u.func->insns);
#endif
    }
}

/* the one place an instruction is produced */
static void emit_raw(int prologue, Insn code, const Opnd *ops, int n) {
    if (!use_api) {
        Buf *b = prologue ? &fn_pro : &fn_body;
        buf_emit(b, "    %s", insn_text[code]);
        for (int i = 0; i < n; i++) { buf_emit(b, i ? ", " : " "); text_opnd(b, &ops[i]); }
        buf_emit(b, "\n");
        return;
    }
#ifndef NO_MIR
    MIR_op_t mops[64];
    for (int i = 0; i < n; i++) mops[i] = mir_opnd(&ops[i]);
    MIR_insn_t insn = MIR_new_insn_arr(ctx, insn_mir[code], n, mops);
    if (prologue) {
        if (prologue_last) MIR_insert_insn_after(ctx, cur_fn, prologue_last, insn);
        else MIR_prepend_insn(ctx, cur_fn, insn);
        prologue_last = insn;
    } else {
        MIR_append_insn(ctx, cur_fn, insn);
    }
#endif
}

#define OPS_(...) ((const Opnd[]){__VA_ARGS__}), (int)(sizeof((Opnd[]){__VA_ARGS__}) / sizeof(Opnd))
#define EMIT(code, ...)          emit_ops(0, I_##code, OPS_(__VA_ARGS__))   /* fixed opcode */
#define EMIT_OP(code_var, ...)   emit_ops(0, code_var, OPS_(__VA_ARGS__))   /* computed opcode */
#define EMIT_PROLOGUE(code, ...) emit_ops(1, I_##code, OPS_(__VA_ARGS__))

static int name_is_item(const char *name);

/* A register name must be unique in its function and, in MIR text, must not shadow a module item
   (global, function, proto): "mov t, x" would read the local reg x instead of global x's address. */
static int reg_exists(const char *name) {
    for (int i = 0; i < nregs; i++)
        if (strcmp(regs[i].name, name) == 0) return 1;
    return name_is_item(name);
}

/* create a register with a name unique in the current function (shadowed vars get x_2, x_3...) */
static int new_reg(const char *base, RegKind rk, int is_param) {
    if (nregs >= MAX_REGS) error("function too large");
    Reg *r = &regs[nregs];
    snprintf(r->name, sizeof r->name, "%s", base);
    for (int k = 2; !is_param && reg_exists(r->name); k++) snprintf(r->name, sizeof r->name, "%.30s_%d", base, k);
    r->rk = rk;
    if (!use_api) {
        if (!is_param) buf_emit(&fn_pro, "    local %s:%s\n", rk_text[rk], r->name);
    } else {
#ifndef NO_MIR
        r->mreg = is_param ? MIR_reg(ctx, r->name, cur_fn->u.func)
                           : MIR_new_func_reg(ctx, cur_fn->u.func, rk_mir[rk], r->name);
#endif
    }
    return nregs++;
}

static int new_temp(RegKind rk) {
    char name[32];
    sprintf(name, "_t%d", temp_counter++);
    return new_reg(name, rk, 0);
}

static int new_label(void) {
    if (nlabels >= MAX_LABELS) error("function too large");
#ifndef NO_MIR
    if (use_api) labels[nlabels] = MIR_new_label(ctx);
#endif
    return nlabels++;
}

static void place_label(int l) {
    last_cmp.valid = 0;
    if (!use_api) { buf_emit(&fn_body, "L%d:\n", label_base + l); return; }
#ifndef NO_MIR
    MIR_append_insn(ctx, cur_fn, labels[l]);
#endif
}

/* stack slot of `size` bytes, allocated once in the function prologue; returns the address reg */
static int new_slot(const char *name, int size) {
    int r = new_reg(name, RK_I, 0);
    EMIT_PROLOGUE(ALLOCA, R(r), IMM(size < 8 ? 8 : (size + 7) / 8 * 8));
    return r;
}

/* lexer-independent "dry run": sizeof expr and ?: look at an expression's type without keeping
   its code.  Text mode truncates the buffers; API mode removes the instructions. */
typedef struct {
    size_t body_len, pro_len;
    int nregs, temp_counter;
#ifndef NO_MIR
    MIR_insn_t tail;
#endif
} EmitMark;

static EmitMark emit_mark(void) {
    EmitMark m;
    memset(&m, 0, sizeof m);
    m.body_len = fn_body.len;
    m.pro_len = fn_pro.len;
    m.nregs = nregs;
    m.temp_counter = temp_counter;
#ifndef NO_MIR
    if (use_api && cur_fn) m.tail = DLIST_TAIL(MIR_insn_t, cur_fn->u.func->insns);
#endif
    return m;
}

static void emit_rollback(EmitMark m) {
    last_cmp.valid = 0;
    if (!use_api) {
        fn_body.len = m.body_len; if (fn_body.p) fn_body.p[m.body_len] = '\0';
        fn_pro.len = m.pro_len; if (fn_pro.p) fn_pro.p[m.pro_len] = '\0';
        nregs = m.nregs;
        temp_counter = m.temp_counter;
        return;
    }
#ifndef NO_MIR
    /* API: registers can't be deleted (they stay unused); instructions can */
    MIR_insn_t insn = m.tail ? DLIST_NEXT(MIR_insn_t, m.tail) : DLIST_HEAD(MIR_insn_t, cur_fn->u.func->insns);
    while (insn) {
        MIR_insn_t next = DLIST_NEXT(MIR_insn_t, insn);
        if (insn == prologue_last) prologue_last = DLIST_PREV(MIR_insn_t, insn);
        MIR_remove_insn(ctx, cur_fn, insn);
        insn = next;
    }
#endif
}

/* ============================================================ types
 * A type is a base (char, int, struct ...) plus a chain of derivations applied to it, innermost
 * first: `int *a[3]` = int, PTR, ARR(3).  `int (*fp)(int)` = int, FUNC(sig), PTR.
 * cq: const qualifier of the object at that level (bcq for the base). */
typedef enum { TY_VOID, TY_CHAR, TY_SHORT, TY_INT, TY_LONG, TY_FLOAT, TY_DOUBLE, TY_STRUCT } Base;
enum { D_PTR, D_ARR, D_FUNC };
typedef struct { unsigned char kind, cq; int n; } Deriv;   /* ARR: length (-1 = unknown); FUNC: sig index */
#define MAX_DERIV 8
/* bfw/bfp: a bit-field member's width and bit position inside its storage unit (an object of the
   declared type at the member's offset); 0 width = not a bit-field */
typedef struct { Base base; unsigned char uns, bcq, bfw, bfp; short nd; int sidx; Deriv d[MAX_DERIV]; } Type;

typedef struct Member { char name[32]; Type ty; int off; } Member;   /* name "" = anonymous struct/union member */
typedef struct { char name[32]; Member m[64]; int n, size, align, complete, is_union, depth, hidden; } Struct;
static Struct structs[512];
static int nstructs;

#define MAX_PARAMS 32
typedef struct {
    Type ret;
    int np, vararg, unprototyped;
    int knr;             /* K&R identifier list f(a, b): the types follow the declarator (int by default) */
    Type pt[MAX_PARAMS];
    char pn[MAX_PARAMS][32];
    char **pdef;         /* .c2 optional parameters: default value source per parameter (NULL: none) */
    int *plen;           /* .c2 int a[3] parameters: the exact array length a call must pass (0: any) */
    int proto_emitted;   /* "proto_sigN" for indirect calls */
} Sig;
static Sig sigs[2048];
static int nsigs;

static NORETURN void error(const char *fmt, ...);

static Type mk_type(Base b) { Type t; memset(&t, 0, sizeof t); t.base = b; t.sidx = -1; return t; }
static Type int_type(void)  { return mk_type(TY_INT); }
static Type long_type(void) { return mk_type(TY_LONG); }
static int top_kind(Type t)  { return t.nd ? t.d[t.nd - 1].kind : -1; }
static int is_scalar_obj(Type t)  { return t.nd == 0; }
static int is_integer(Type t)     { return t.nd == 0 && t.base >= TY_CHAR && t.base <= TY_LONG; }
static int is_bool(Type t)        { return t.nd == 0 && t.base == TY_CHAR && t.uns == 2; }   /* _Bool: unsigned char, 0 or 1 */
static int is_float(Type t)       { return t.nd == 0 && t.base == TY_FLOAT; }
static int is_double(Type t)      { return t.nd == 0 && t.base == TY_DOUBLE; }
static Type mk_type(Base b);
static Type wchar_type(void) {          /* int; unsigned short on Windows */
    Type t = mk_type(WCHAR_SIZE == 2 ? TY_SHORT : TY_INT);
    t.uns = WCHAR_SIZE == 2;
    return t;
}
static int is_fp(Type t)          { return is_float(t) || is_double(t); }
static int is_arith(Type t)       { return is_integer(t) || is_fp(t); }
static int is_pointer(Type t)     { return top_kind(t) == D_PTR; }
static int is_array(Type t)       { return top_kind(t) == D_ARR; }
static int is_func(Type t)        { return top_kind(t) == D_FUNC; }
static int arr_len(Type t)        { return t.d[t.nd - 1].n; }
static int func_sig(Type t)       { return t.d[t.nd - 1].n; }
static int is_struct_val(Type t)  { return t.base == TY_STRUCT && t.nd == 0; }
static int is_void(Type t)        { return t.base == TY_VOID && t.nd == 0; }
static int is_aggregate(Type t)   { return is_array(t) || is_struct_val(t); }
static int is_func_ptr(Type t)    { return is_pointer(t) && t.nd >= 2 && t.d[t.nd - 2].kind == D_FUNC; }
static int is_const_obj(Type t)   { return t.nd ? t.d[t.nd - 1].cq : t.bcq; }
static Type add_const(Type t)     { if (t.nd) t.d[t.nd - 1].cq = 1; else t.bcq = 1; return t; }
static Type unconst(Type t)       { if (t.nd) t.d[t.nd - 1].cq = 0; else t.bcq = 0; return t; }
static Type push_deriv(Type t, int kind, int n) {
    if (t.nd >= MAX_DERIV) error("type too complex");
    t.d[t.nd].kind = (unsigned char)kind;
    t.d[t.nd].cq = 0;
    t.d[t.nd++].n = n;
    return t;
}
static Type ptr_to(Type t)            { return push_deriv(t, D_PTR, 0); }
static Type array_of(Type t, int n)   { return push_deriv(t, D_ARR, n); }
static Type func_of(Type ret, int s)  { return push_deriv(ret, D_FUNC, s); }
static Type elem_of(Type t)           { if (t.nd) t.nd--; return t; }
static Type set_arr_len(Type t, int n) { t.d[t.nd - 1].n = n; return t; }

static int same_type(Type a, Type b) {
    if (a.base != b.base || a.uns != b.uns || a.nd != b.nd) return 0;
    if (a.base == TY_STRUCT && a.sidx != b.sidx) return 0;
    for (int i = 0; i < a.nd; i++) {
        if (a.d[i].kind != b.d[i].kind) return 0;
        if (a.d[i].kind == D_ARR && a.d[i].n != b.d[i].n && a.d[i].n >= 0 && b.d[i].n >= 0) return 0;
        if (a.d[i].kind == D_FUNC) {
            Sig *x = &sigs[a.d[i].n], *y = &sigs[b.d[i].n];
            if (x->unprototyped || y->unprototyped) continue;
            if (x->np != y->np || x->vararg != y->vararg || !same_type(x->ret, y->ret)) return 0;
            for (int k = 0; k < x->np; k++) if (!same_type(x->pt[k], y->pt[k])) return 0;
        }
    }
    return 1;
}

static int size_of(Type t) {
    if (t.nd) {
        switch (top_kind(t)) {
        case D_PTR: return 8;
        case D_FUNC: error("sizeof a function"); return 1;
        default:
            if (arr_len(t) <= -2) error("internal: size of a variable-length array");
            if (arr_len(t) < 0) error("array size unknown");
            return arr_len(t) * size_of(elem_of(t));
        }
    }
    switch (t.base) {
    case TY_CHAR: return 1;
    case TY_SHORT: return 2;
    case TY_INT: case TY_FLOAT: return 4;
    case TY_STRUCT:
        if (!structs[t.sidx].complete) error("incomplete type 'struct %s'", structs[t.sidx].name);
        return structs[t.sidx].size;
    case TY_VOID: return 1;
    default: return 8;
    }
}

static int align_of(Type t) {
    while (is_array(t)) t = elem_of(t);
    if (t.nd) return 8;
    if (t.base == TY_STRUCT) return structs[t.sidx].align;
    return size_of(t);
}

static int elem_size(Type ptr) { Type e = elem_of(ptr); return is_void(e) || is_func(e) ? 1 : size_of(e); }

static int int_width(Type t) { return t.nd ? 64 : size_of(t) * 8; }

static MemT mem_type(Type t) {
    if (t.nd == 0) switch (t.base) {
        case TY_CHAR: return t.uns ? MT_U8 : MT_I8;
        case TY_SHORT: return t.uns ? MT_U16 : MT_I16;
        case TY_INT: return t.uns ? MT_U32 : MT_I32;
        case TY_FLOAT: return MT_F;
        case TY_DOUBLE: return MT_D;
        default: break;
        }
    return MT_I64;
}

static RegKind reg_kind(Type t) { return is_float(t) ? RK_F : is_double(t) ? RK_D : RK_I; }

static const char *type_name(Type t) {   /* for error messages: "const char *", "int *const", "int (*)(...)" */
    static char b[4][160];
    static int k;
    char *s = b[k++ & 3];
    static const char *names[] = {"void", "char", "short", "int", "long", "float", "double", "struct"};
    int is_union = t.base == TY_STRUCT && structs[t.sidx].is_union;
    snprintf(s, 160, "%s%s%s", t.bcq ? "const " : "", t.uns == 1 ? "unsigned " : "",
             is_bool(t) ? "_Bool" : is_union ? "union" : names[t.base]);
    if (t.base == TY_STRUCT) {
        strcat(s, " ");
        strncat(s, structs[t.sidx].name, 40);
    }
    for (int i = 0; i < t.nd && strlen(s) < 120; i++) {
        if (t.d[i].kind == D_PTR) strcat(s, t.d[i].cq ? " *const" : " *");
        else if (t.d[i].kind == D_ARR) { char a[16]; snprintf(a, sizeof a, "[%d]", t.d[i].n < 0 ? 0 : t.d[i].n); strcat(s, a); }
        else strcat(s, "(...)");
    }
    return s;
}

/* struct/union tags are block scoped */
static int scope_depth;

static int find_struct(const char *name) {
    for (int i = nstructs - 1; i >= 0; i--)
        if (!structs[i].hidden && strcmp(structs[i].name, name) == 0) return i;
    return -1;
}

static int anon_struct_counter;
static int new_struct(const char *name, int is_union) {
    if (nstructs >= 512) error("too many structs");
    Struct *s = &structs[nstructs];
    memset(s, 0, sizeof *s);
    if (name[0]) snprintf(s->name, sizeof s->name, "%s", name);
    else snprintf(s->name, sizeof s->name, "<anonymous %d>", anon_struct_counter++);
    s->is_union = is_union;
    s->depth = scope_depth;
    return nstructs++;
}

static void hide_structs(int depth) {
    for (int i = 0; i < nstructs; i++)
        if (structs[i].depth >= depth) structs[i].hidden = 1;
}

static int new_sig(void) {
    if (nsigs >= 2048) error("too many function types");
    memset(&sigs[nsigs], 0, sizeof sigs[nsigs]);
    return nsigs++;
}

/* ============================================================ symbols */
typedef struct { char name[32]; Type ty; } Typedef;
static Typedef typedefs[256];
static int ntypedefs;

static Typedef *find_typedef(const char *name) {
    for (int i = 0; i < ntypedefs; i++)
        if (strcmp(typedefs[i].name, name) == 0) return &typedefs[i];
    return NULL;
}

typedef struct { char name[32]; long long val; int depth; Type type; } EnumConst;
/* enum tags with a fixed underlying type (C23: enum E : unsigned char { ... }) */
typedef struct { char name[32]; int depth; Type base; } EnumTag;
static EnumTag enum_tags[256];
static int nenum_tags;
/* .c2 nested functions: `int f(void) { int g(int x) { ... } return g(1); }` defines g as a hidden
   function (named __nN_g), compiled after the enclosing one; g is visible by its own name in the
   enclosing block only.  It cannot use the enclosing function's variables (no captures). */
typedef struct { char name[32], target[32]; int depth; } FuncAlias;
static FuncAlias func_aliases[64];
static int nfunc_aliases;
typedef struct { char name[32], target[32], file[1024]; const char *body; int line; } NestedFunc;
static NestedFunc pending_nested[64];
static int npending_nested, nested_counter;
static int decl_saw_static;   /* parse_decl_suffixes saw [static N] */
static EnumConst enum_consts[1024];
static int nenum_consts;

static EnumConst *find_enum_const(const char *name) {
    for (int i = nenum_consts - 1; i >= 0; i--)          /* innermost first */
        if (strcmp(enum_consts[i].name, name) == 0) return &enum_consts[i];
    return NULL;
}

typedef struct { char name[32]; Type ty; int reg; int is_mem; int depth; int global;
                 int sra, sra_reg[8];
                 int uninit; } Var;   /* global >= 0: static local; sra: members kept in registers;
                                         uninit: a .c local declared without a value, not yet set */
static int no_uninit_warn;   /* > 0 inside sizeof / typeof / dry runs: nothing is read */
static int addr_of_pending;  /* the operand of a unary & is being parsed */
static Var vars[2048];
static int nvars;

static Var *find_var(const char *name) {
    for (int i = nvars - 1; i >= 0; i--)          /* innermost first: shadowing */
        if (strcmp(vars[i].name, name) == 0) return &vars[i];
    return NULL;
}

/* variable-length arrays: the outermost dimension's length lives in a register; the array is an
   alloca bracketed by bstart/bend (the stack is given back when its block ends) */
#define VLA_MARK(i) (-2 - (i))                      /* Deriv.n of a VLA dimension */
static int vla_len_reg[512], nvla;
typedef struct { int save_reg, depth; } VlaScope;
static VlaScope vla_scopes[256];
static int nvla_scopes;
static int vla_ok, param_depth;                           /* declarator context: local object / parameter list */
static int is_vla(Type t) { return is_array(t) && arr_len(t) <= -2; }

static void enter_scope(void) { scope_depth++; }
static void leave_scope(void) {
    while (nvla_scopes > 0 && vla_scopes[nvla_scopes - 1].depth == scope_depth)
        EMIT(BEND, R(vla_scopes[--nvla_scopes].save_reg));
    while (nvars > 0 && vars[nvars - 1].depth == scope_depth) nvars--;
    hide_structs(scope_depth);                     /* struct tags declared in this block */
    while (nenum_consts > 0 && enum_consts[nenum_consts - 1].depth >= scope_depth) nenum_consts--;
    while (nenum_tags > 0 && enum_tags[nenum_tags - 1].depth >= scope_depth) nenum_tags--;
    while (nfunc_aliases > 0 && func_aliases[nfunc_aliases - 1].depth >= scope_depth) nfunc_aliases--;
    scope_depth--;
}

typedef struct { char name[40]; Type ty; int has_init, deferred; } Global;   /* deferred: struct completed later */
static Global globals[1024];
static int nglobals;

static Global *find_global(const char *name) {
    for (int i = 0; i < nglobals; i++)
        if (strcmp(globals[i].name, name) == 0) return &globals[i];
    return NULL;
}

typedef struct {
    char name[32];
    Type ret;
    int np;
    Type pt[MAX_PARAMS];
    char pn[MAX_PARAMS][32];
    int defined;
    int vararg;          /* (fmt, ...) */
    int is_import;       /* declared, never defined in this file: resolved from the C runtime */
    int proto_emitted, item_emitted;   /* items are created lazily, on first call */
    int unprototyped;                  /* declared as f() */
    int sig;                           /* signature index for function pointers, -1 until needed */
    char **pdef;                       /* .c2: default values of optional parameters (see Sig) */
    int *plen;                         /* .c2: exact array lengths of array parameters (see Sig) */
    char (*un)[32];                    /* .c2 with defaults: the parameter names as written (pn may be
                                          renamed for MIR); defaults refer to the others by these */
} Func;
static Func funcs[1024];
static int nfuncs;
static Func *cur_func;
static int cur_sret_reg = -1;   /* struct-returning function: reg holding the caller's result buffer */


static Func *find_func(const char *name) {
    for (int i = nfunc_aliases - 1; i >= 0; i--)
        if (strcmp(func_aliases[i].name, name) == 0) { name = func_aliases[i].target; break; }
    for (int i = 0; i < nfuncs; i++)
        if (strcmp(funcs[i].name, name) == 0) return &funcs[i];
    return NULL;
}

static int name_is_item(const char *name) {
    if (find_global(name) || find_func(name)) return 1;
    if (strncmp(name, "proto_", 6) == 0 && find_func(name + 6)) return 1;
    return strncmp(name, "_str", 4) == 0 || strncmp(name, "__c2_", 5) == 0 || strncmp(name, "proto_", 6) == 0;
}

/* names whose address is taken in the current function body: these scalars need a stack slot */
static char addr_taken[512][32];
static int naddr_taken;

static int is_addr_taken(const char *name) {
    for (int i = 0; i < naddr_taken; i++)
        if (strcmp(addr_taken[i], name) == 0) return 1;
    return 0;
}

static void prescan_addr_taken(const char *s) {   /* s points just after the function's '{' */
    int depth = 1;
    naddr_taken = 0;
    for (; *s && depth > 0; s++) {
        if (*s == '"' || *s == '\'') {         /* skip literals */
            char q = *s++;
            while (*s && *s != q) { if (*s == '\\' && s[1]) s++; s++; }
            if (!*s) break;
            continue;
        }
        if (*s == '{') depth++;
        else if (*s == '}') depth--;
        else if (c2_ext && (isalpha((unsigned char)*s) || *s == '_') && !(isalnum((unsigned char)s[-1]) || s[-1] == '_' || s[-1] == '.')) {
            const char *q = s;                         /* .c2: name.f( may pass &name (method call) */
            char name[32];
            int n = 0;
            while ((isalnum((unsigned char)*q) || *q == '_') && n < 31) name[n++] = *q++;
            name[n] = '\0';
            const char *r = q;
            while (isspace((unsigned char)*r)) r++;
            if (*r == '.') {
                r++;
                while (isspace((unsigned char)*r)) r++;
                while (isalnum((unsigned char)*r) || *r == '_') r++;
                while (isspace((unsigned char)*r)) r++;
                if (*r == '(' && naddr_taken < 512) strcpy(addr_taken[naddr_taken++], name);
            }
            s = q - 1;
        } else if (strncmp(s, "__builtin_va_", 13) == 0) {   /* va_start(ap...) etc.: ap lives in memory */
            const char *q = s + 13;
            while (isalpha((unsigned char)*q)) q++;
            while (isspace((unsigned char)*q)) q++;
            for (int arg = 0; arg < 2 && *q == (arg ? ',' : '('); arg++) {
                q++;
                while (isspace((unsigned char)*q)) q++;
                char name[32];
                int n = 0;
                while ((isalnum((unsigned char)*q) || *q == '_') && n < 31) name[n++] = *q++;
                name[n] = '\0';
                if (n && naddr_taken < 512) strcpy(addr_taken[naddr_taken++], name);
                while (isspace((unsigned char)*q)) q++;
            }
            s += 12;
        } else if (*s == '&' && s[1] != '&' && s[1] != '=' && s[-1] != '&') {
            const char *q = s + 1;
            while (isspace((unsigned char)*q)) q++;
            if (!(isalpha((unsigned char)*q) || *q == '_')) continue;
            char name[32];
            int n = 0;
            while ((isalnum((unsigned char)*q) || *q == '_') && n < 31) name[n++] = *q++;
            name[n] = '\0';
            while (isspace((unsigned char)*q)) q++;
            /* &a[i], &s.f, &p->f don't need `a`/`s`/`p` itself in memory */
            if (*q == '[' || *q == '.' || (q[0] == '-' && q[1] == '>')) continue;
            if (naddr_taken < 512) strcpy(addr_taken[naddr_taken++], name);
        }
    }
}

/* ============================================================ module-level back end */
/* ============================================================ struct ABI (by value)
 * Structs are passed and returned by value following the platform C ABI, so script functions and
 * C functions can call each other with structs (Vector3 add(Vector3, Vector3)).  MIR does the
 * register/stack work; the front end only classifies:
 *   argument: a block type (MIR_T_BLK + k) with the struct size; the callee's parameter register
 *             holds the address of its own copy.
 *   result:   up to two registers (abi_ret_regs > 0) loaded/stored 8 bytes at a time, or a hidden
 *             result buffer (MIR_T_RBLK, first parameter).
 * SysV x86-64: <= 16 bytes, classified per eightbyte (INTEGER / SSE) from the members.
 * Win64: 1/2/4/8-byte structs in an integer register, others by reference (pointer to a copy). */
enum { CL_NONE, CL_INT, CL_SSE, CL_MEM };
static int cl_merge(int a, int b) {
    if (a == b) return a;
    if (a == CL_NONE) return b;
    if (b == CL_NONE) return a;
    if (a == CL_MEM || b == CL_MEM) return CL_MEM;
    if (a == CL_INT || b == CL_INT) return CL_INT;
    return CL_SSE;
}
static void cl_walk(Type t, int off, int cls[2]) {
    if (is_array(t)) {
        Type e = elem_of(t);
        int es = size_of(e), n = arr_len(t);
        for (int i = 0; i < n && es > 0; i++) cl_walk(e, off + i * es, cls);
        return;
    }
    if (is_struct_val(t)) {
        Struct *st = &structs[t.sidx];
        for (int i = 0; i < st->n; i++) cl_walk(st->m[i].ty, off + st->m[i].off, cls);
        return;
    }
    int q = off / 8;
    if (q > 1) { cls[0] = CL_MEM; return; }
    cls[q] = cl_merge(cls[q], is_fp(t) ? CL_SSE : CL_INT);
    if (off % size_of(t)) cls[0] = CL_MEM;                 /* unaligned member */
}
/* struct t in registers: returns the number of eightbytes (1-2) and their load/store types,
   or 0 = in memory */
static int abi_regs(Type t, MemT q[2]) {
    int size = size_of(t);
#if defined(_WIN32)
    switch (size) {
    case 1: q[0] = MT_I8; return 1;
    case 2: q[0] = MT_I16; return 1;
    case 4: q[0] = MT_I32; return 1;
    case 8: q[0] = MT_I64; return 1;
    default: return 0;
    }
#else
    if (size == 0 || size > 16) return 0;
    int cls[2] = {CL_NONE, CL_NONE}, n = (size + 7) / 8;
    cl_walk(t, 0, cls);
    for (int i = 0; i < n; i++) {
        if (cls[i] == CL_MEM) return 0;
        int last = size - 8 * i;                           /* bytes in this eightbyte */
        if (cls[i] == CL_SSE) q[i] = last <= 4 ? MT_F : MT_D;
        else q[i] = last == 1 ? MT_I8 : last == 2 ? MT_I16 : last <= 4 ? MT_I32 : MT_I64;
    }
    return n;
#endif
}
/* block type of a struct argument; MT_I64 = passed as a plain pointer to a copy (Win64 by reference) */
static MemT abi_arg_type(Type t) {
    MemT q[2];
    int n = abi_regs(t, q);
#if defined(_WIN32)
    return n ? MT_BLK1 : MT_I64;
#else
    if (n == 0) return MT_BLK0;
    int fp0 = q[0] == MT_F || q[0] == MT_D, fp1 = n > 1 && (q[1] == MT_F || q[1] == MT_D);
    if (n == 1) return fp0 ? MT_BLK2 : MT_BLK1;
    if (fp0 && fp1) return MT_BLK2;
    if (!fp0 && !fp1) return MT_BLK1;
    return fp0 ? MT_BLK4 : MT_BLK3;
#endif
}
/* number of result registers of a struct-returning function (0 = hidden result buffer) */
static int abi_ret_regs(Type t, MemT q[2]) { return is_struct_val(t) ? abi_regs(t, q) : 0; }
/* struct returned through a hidden result buffer (MIR_T_RBLK, first parameter).  The function also
   returns the buffer's address in the first integer register (rax), as both the SysV and Win64 ABIs
   require: MSVC callers read the result through rax. */
static int abi_sret(Type t) { MemT q[2]; return is_struct_val(t) && abi_regs(t, q) == 0; }
static RegKind mt_rk(MemT m) { return m == MT_F ? RK_F : m == MT_D ? RK_D : RK_I; }

/* MIR signature type of a value of type t.  Struct parameters are blocks (see abi_arg_type).
   C runtime functions return a 32-bit int: an imported function's narrow integer result is
   declared i32/u32 and normalised after the call.  Arguments are always passed in 64-bit regs. */
static const char *sig_type_text(Type t, int import_result) {
    if (is_float(t)) return "f";
    if (is_double(t)) return "d";
    if (import_result && is_integer(t) && int_width(t) <= 32) return t.uns && int_width(t) == 32 ? "u32" : "i32";
    return "i64";
}
#ifndef NO_MIR
static MIR_type_t sig_type_mir(Type t, int import_result) {
    if (is_float(t)) return MIR_T_F;
    if (is_double(t)) return MIR_T_D;
    if (import_result && is_integer(t) && int_width(t) <= 32) return t.uns && int_width(t) == 32 ? MIR_T_U32 : MIR_T_I32;
    return MIR_T_I64;
}
#endif

static Sig sig_of_func(Func *f) {
    Sig g;
    memset(&g, 0, sizeof g);
    g.ret = f->ret;
    g.np = f->np;
    g.vararg = f->vararg || (f->unprototyped && !f->defined);   /* f(): any arguments, passed like varargs */
    memcpy(g.pt, f->pt, sizeof g.pt);
    memcpy(g.pn, f->pn, sizeof g.pn);
    return g;
}

static int sig_returns_value(Sig *g) { return !is_void(g->ret) && !is_struct_val(g->ret); }
/* results of a signature: 0, 1, or 2 (a struct returned in registers); types in rt[] */
static int sig_results(Sig *g, int c_result, MemT *q) {
    if (is_void(g->ret)) return 0;
    if (abi_sret(g->ret)) { q[0] = MT_I64; return 1; }   /* the buffer's address */
    if (is_struct_val(g->ret)) return abi_ret_regs(g->ret, q);
    q[0] = is_float(g->ret) ? MT_F : is_double(g->ret) ? MT_D : MT_I64;
    if (c_result && is_integer(g->ret) && int_width(g->ret) <= 32) q[0] = g->ret.uns && int_width(g->ret) == 32 ? MT_U32 : MT_I32;
    return 1;
}
/* MIR types of results are register types: narrow integers widen to i64 except the C int results */
static const char *res_text(MemT m) { return m == MT_F ? "f" : m == MT_D ? "d" : m == MT_I32 ? "i32" : m == MT_U32 ? "u32" : "i64"; }
#ifndef NO_MIR
static MIR_type_t res_mir(MemT m) { return m == MT_F ? MIR_T_F : m == MT_D ? MIR_T_D : m == MT_I32 ? MIR_T_I32 : m == MT_U32 ? MIR_T_U32 : MIR_T_I64; }
#endif

/* c_result: narrow int results declared i32/u32 as in the C ABI (imports, indirect calls) */
static void text_signature(Buf *b, const char *name, const char *kind, Sig *g, int c_result) {
    buf_emit(b, "%s: %s", name, kind);
    int first = 1;
    MemT q[2];
    int nres = sig_results(g, c_result, q);
    for (int i = 0; i < nres; i++) { buf_emit(b, "%s%s", first ? " " : ", ", res_text(q[i])); first = 0; }
    if (abi_sret(g->ret)) { buf_emit(b, "%srblk:%d(_ret)", first ? " " : ", ", size_of(g->ret)); first = 0; }
    for (int i = 0; i < g->np; i++) {
        char pn[40];
        if (g->pn[i][0]) snprintf(pn, sizeof pn, "%s", g->pn[i]); else snprintf(pn, sizeof pn, "p%d", i);
        if (is_struct_val(g->pt[i]) && abi_arg_type(g->pt[i]) != MT_I64)
            buf_emit(b, "%s%s:%d(%s)", first ? " " : ", ", mt_text[abi_arg_type(g->pt[i])], size_of(g->pt[i]), pn);
        else buf_emit(b, "%s%s:%s", first ? " " : ", ", sig_type_text(g->pt[i], 0), pn);
        first = 0;
    }
    if (g->vararg) buf_emit(b, first ? " ..." : ", ...");
    buf_emit(b, "\n");
}

#ifndef NO_MIR
/* returns number of args; *nres results in rt[] */
static int mir_signature(Sig *g, int c_result, MIR_type_t *rt, size_t *nres, MIR_var_t *v) {
    static char pnames[MAX_PARAMS][8];
    int n = 0;
    MemT q[2];
    *nres = sig_results(g, c_result, q);
    for (size_t i = 0; i < *nres; i++) rt[i] = res_mir(q[i]);
    if (abi_sret(g->ret)) {
        memset(&v[n], 0, sizeof v[n]);
        v[n].type = MIR_T_RBLK;
        v[n].size = size_of(g->ret);
        v[n++].name = "_ret";
    }
    for (int i = 0; i < g->np; i++) {
        memset(&v[n], 0, sizeof v[n]);
        v[n].type = sig_type_mir(g->pt[i], 0);
        if (is_struct_val(g->pt[i]) && abi_arg_type(g->pt[i]) != MT_I64) {
            v[n].type = mt_mir[abi_arg_type(g->pt[i])];
            v[n].size = size_of(g->pt[i]);
        }
        if (g->pn[i][0]) v[n++].name = g->pn[i];
        else { sprintf(pnames[i], "p%d", i); v[n++].name = pnames[i]; }
    }
    return n;
}
#endif

static int mir_module_open;
static void be_module_begin(void) {
    if (!use_api) { buf_emit(&out_text, "C2: module\n"); return; }
#ifndef NO_MIR
    mir_module = MIR_new_module(ctx, "C2");
    mir_module_open = 1;
#endif
}

static void be_module_end(void) {
    if (!use_api) { buf_emit(&out_text, "endmodule\n"); return; }
#ifndef NO_MIR
    MIR_finish_module(ctx);
    mir_module_open = 0;
#endif
}

static void be_proto_sig(const char *pname, Sig *g, int c_result) {
    if (!use_api) { text_signature(&out_text, pname, "proto", g, c_result); return; }
#ifndef NO_MIR
    MIR_type_t rt[2];
    MIR_var_t v[MAX_PARAMS + 1];
    size_t nres;
    int n = mir_signature(g, c_result, rt, &nres, v);
    register_item(pname, g->vararg ? MIR_new_vararg_proto_arr(ctx, pname, nres, rt, n, v)
                                   : MIR_new_proto_arr(ctx, pname, nres, rt, n, v));
#endif
}

static void be_proto(Func *f) {
    char pname[40];
    snprintf(pname, sizeof pname, "proto_%s", f->name);
    Sig g = sig_of_func(f);
    be_proto_sig(pname, &g, f->is_import);
}

/* proto for calls through a function pointer of signature si */
static const char *sig_proto_name(int si) {
    static char pname[40];
    snprintf(pname, sizeof pname, "proto_sig%d", si);
    if (!sigs[si].proto_emitted) {
        be_proto_sig(pname, &sigs[si], 1);
        sigs[si].proto_emitted = 1;
    }
    return pname;
}

static void be_simple_item(const char *kind, const char *name) {   /* import / forward / export */
    if (!use_api) { buf_emit(&out_text, "%s %s\n", kind, name); return; }
#ifndef NO_MIR
    if (kind[0] == 'i') register_item(name, MIR_new_import(ctx, name));
    else if (kind[0] == 'f') register_item(name, MIR_new_forward(ctx, name));
    else MIR_new_export(ctx, name);
#endif
}

/* proto + import/forward of a function, created the first time it is called */
static void ensure_func_items(Func *f) {
    if (!f->proto_emitted) { be_proto(f); f->proto_emitted = 1; }
    if (!f->item_emitted && !f->defined) {
        be_simple_item(f->is_import ? "import" : "forward", f->name);
        f->item_emitted = 1;
    }
}

/* a string literal becomes a data item; returns its name */
static const char *be_string(const char *str, int len) {
    static char name[32];
    sprintf(name, "_str%d", string_counter++);
    if (!use_api) {
        buf_emit(&out_text, "%s: string \"", name);
        for (int i = 0; i < len; i++) {
            unsigned char c = (unsigned char)str[i];
            if (c == '\\' || c == '"') buf_emit(&out_text, "\\%c", c);
            else if (c >= 32 && c < 127) buf_emit(&out_text, "%c", c);
            else buf_emit(&out_text, "\\%03o", c);   /* MIR reads up to 3 octal digits */
        }
        if (len == 0) buf_emit(&out_text, "\\000");   /* MIR adds no terminator to an empty string */
        buf_emit(&out_text, "\"\n");
        return name;
    }
#ifndef NO_MIR
    MIR_str_t ms;
    ms.len = (size_t)len + 1;                      /* include the terminating NUL */
    ms.s = str;
    register_item(name, MIR_new_string_data(ctx, name, ms));
#endif
    return name;
}

static void be_bss(const char *name, int size) {
    if (!use_api) { buf_emit(&out_text, "%s: bss %d\n", name, size < 1 ? 1 : size); return; }
#ifndef NO_MIR
    register_item(name, MIR_new_bss(ctx, name, size < 1 ? 1 : size));
#endif
}

static void be_ref_data(const char *name, const char *target) {   /* char *msg = "..."; */
    if (!use_api) { buf_emit(&out_text, "%s: ref %s, 0\n", name, target); return; }
#ifndef NO_MIR
    register_item(name, MIR_new_ref_data(ctx, name, find_item(target), 0));
#endif
}

/* scalar global with a constant initializer */
static void be_scalar_data(const char *name, Type t, long long iv, double dv) {
    MemT mt = mem_type(t);
    if (!use_api) {
        char b[64];
        if (mt == MT_D) fmt_double(b, dv);
        else if (mt == MT_F) { sprintf(b, "%.9g", (float)dv); if (!strpbrk(b, ".eEni")) strcat(b, ".0"); }
        else sprintf(b, "%lld", iv);
        buf_emit(&out_text, "%s: %s %s\n", name, mt_text[mt], b);
        return;
    }
#ifndef NO_MIR
    union { int8_t i8; uint8_t u8; int16_t i16; uint16_t u16; int32_t i32; uint32_t u32; int64_t i64; float f; double d; } u;
    switch (mt) {
    case MT_I8: u.i8 = (int8_t)iv; break;
    case MT_U8: u.u8 = (uint8_t)iv; break;
    case MT_I16: u.i16 = (int16_t)iv; break;
    case MT_U16: u.u16 = (uint16_t)iv; break;
    case MT_I32: u.i32 = (int32_t)iv; break;
    case MT_U32: u.u32 = (uint32_t)iv; break;
    case MT_F: u.f = (float)dv; break;
    case MT_D: u.d = dv; break;
    default: u.i64 = iv;
    }
    register_item(name, MIR_new_data(ctx, name, mt_mir[mt], 1, &u));
#endif
}

static void be_func_begin(Func *f) {
    label_base += nlabels;
    nregs = nlabels = temp_counter = 0;
    if (!use_api) {
        buf_clear(&fn_head);
        buf_clear(&fn_pro);
        buf_clear(&fn_body);
        Sig g = sig_of_func(f);
        text_signature(&fn_head, f->name, "func", &g, 0);
    } else {
#ifndef NO_MIR
        MIR_type_t rt[2];
        MIR_var_t v[MAX_PARAMS + 1];
        size_t nres;
        Sig g = sig_of_func(f);
        int n = mir_signature(&g, 0, rt, &nres, v);
        cur_fn = f->vararg ? MIR_new_vararg_func_arr(ctx, f->name, nres, rt, n, v)
                           : MIR_new_func_arr(ctx, f->name, nres, rt, n, v);
        prologue_last = NULL;
        register_item(f->name, cur_fn);   /* recursion refers to the item being built */
#endif
    }
}

static void be_func_end(void) {
    if (!use_api) {
        buf_append(&out_text, fn_head.p, fn_head.len);
        if (fn_pro.p) buf_append(&out_text, fn_pro.p, fn_pro.len);
        if (fn_body.p) buf_append(&out_text, fn_body.p, fn_body.len);
        buf_emit(&out_text, "endfunc\n\n");
        return;
    }
#ifndef NO_MIR
    MIR_finish_func(ctx);
    cur_fn = NULL;
#endif
}

/* ============================================================ values
 * V_RVAL: a value in `op` (reg or immediate)
 * V_REG : an lvalue living in register `op.r`
 * V_MEM : an lvalue in memory at disp(base).  Struct values are always V_MEM.
 *
 * 8/16-bit integers in registers are always sign/zero-extended.  32-bit int/unsigned values may be
 * "dirty": only the low 32 bits are meaningful.  Arithmetic and comparisons on them use MIR's 32-bit
 * instructions (adds, lts, ...), which ignore the upper half, so no extension is needed after every
 * operation; clean32() extends just where a 64-bit view is needed (to long, pointer, double, a
 * condition, a switch).  `dirty` is a hint for values known to be clean (loads, constants). */
typedef enum { V_RVAL, V_REG, V_MEM, V_SRA } VKind;   /* V_SRA: struct local in registers (base: var index) */
typedef struct Val { VKind k; Opnd op; int base, disp; Type ty; int dirty; int ix, sc; } Val;   /* ix: index reg + 1 */

static Opnd val_mem(Val v, MemT mt) {   /* memory operand of a V_MEM value: disp(base, index, scale) */
    Opnd o = MEM(mt, v.disp, v.base);
    o.ix = v.ix;
    o.sc = v.sc;
    return o;
}

static int is_int32(Type t) { return is_integer(t) && int_width(t) == 32; }

static Val rv_op(Opnd o, Type t)       { Val v; memset(&v, 0, sizeof v); v.k = V_RVAL; v.op = o; v.ty = t; return v; }
static Val rv_reg(int r, Type t)       { return rv_op(R(r), t); }
static Val lv_mem(int base, int disp, Type t) {
    Val v; memset(&v, 0, sizeof v); v.k = V_MEM; v.base = base; v.disp = disp; v.ty = t; return v;
}

static Insn mov_insn(RegKind rk) { return rk == RK_F ? I_FMOV : rk == RK_D ? I_DMOV : I_MOV; }

static int to_reg(Val v) {   /* v is an rvalue */
    if (v.op.k == OK_REG) return v.op.r;
    RegKind rk = reg_kind(v.ty);
    int t = new_temp(rk);
    EMIT_OP(mov_insn(rk), R(t), v.op);
    return t;
}

/* address of base+disp in a register */
static int addr_reg(int base, int disp) {
    if (disp == 0) return base;
    int t = new_temp(RK_I);
    EMIT(ADD, R(t), R(base), IMM(disp));
    return t;
}

/* bit-fields: the storage unit is loaded whole and the field is shifted out of it */
static MemT bf_unit_type(Type t) {
    int sz = size_of(t);
    return sz == 1 ? MT_U8 : sz == 2 ? MT_U16 : sz == 4 ? MT_U32 : MT_I64;
}
static Type bf_plain(Type t) { t.bfw = t.bfp = 0; return t; }
static Val bf_load(Val v) {
    Type t = bf_plain(v.ty);
    int u = new_temp(RK_I), r = new_temp(RK_I);
    EMIT_OP(I_MOV, R(u), val_mem(v, bf_unit_type(t)));
    int left = 64 - v.ty.bfp - v.ty.bfw;
    if (left) EMIT(LSH, R(r), R(u), IMM(left)); else EMIT(MOV, R(r), R(u));
    if (t.uns) EMIT(URSH, R(r), R(r), IMM(64 - v.ty.bfw));
    else EMIT(RSH, R(r), R(r), IMM(64 - v.ty.bfw));
    if (!is_bool(t) && v.ty.bfw < 32) t = int_type();   /* promotes to int: every value fits */
    else if (!is_bool(t) && v.ty.bfw == 32 && int_width(t) > 32) { Type u = int_type(); u.uns = t.uns; t = u; }
    return rv_reg(r, t);
}

static Val sra_spill(Val v);
static void sra_load(Var *v, int base, int disp);
static Val rvalue(Val v) {
    if (v.k == V_SRA) return sra_spill(v);
    if (is_func(v.ty)) return rv_op(v.op, ptr_to(v.ty));   /* function designator -> pointer */
    if (is_array(v.ty)) {                    /* array decays to pointer to its first element */
        if (v.k != V_MEM) error("internal: array not in memory");
        return rv_reg(addr_reg(v.base, v.disp), ptr_to(elem_of(v.ty)));
    }
    if (is_struct_val(v.ty)) return v;      /* structs stay in memory */
    if (v.k == V_RVAL) return v;
    if (v.ty.bfw) return bf_load(v);
    if (v.k == V_REG) { Val r = rv_reg(v.op.r, v.ty); r.dirty = is_int32(v.ty); return r; }   /* may hold a dirty value */
    RegKind rk = reg_kind(v.ty);
    int t = new_temp(rk);
    EMIT_OP(mov_insn(rk), R(t), val_mem(v, mem_type(v.ty)));   /* narrow loads extend */
    return rv_reg(t, v.ty);
}

static Val address_of(Val v) {
    if (is_func(v.ty)) return rv_op(v.op, ptr_to(v.ty));   /* &function */
    if (v.ty.bfw) error("cannot take the address of a bit-field");
    if (v.k == V_SRA) v = sra_spill(v);   /* (not reached: address-taken structs stay in memory) */
    if (v.k != V_MEM) error("cannot take the address of this expression");
    if (v.ix) {                                      /* base + index*scale + disp */
        int t = new_temp(RK_I), a = new_temp(RK_I);
        EMIT(MUL, R(t), R(v.ix - 1), IMM(v.sc));
        EMIT(ADD, R(a), R(v.base), R(t));
        return rv_reg(addr_reg(a, v.disp), ptr_to(v.ty));
    }
    return rv_reg(addr_reg(v.base, v.disp), ptr_to(v.ty));
}

/* truncate an integer constant to type t */
static long long trunc_const(long long x, Type t) {
    if (!is_integer(t)) return x;
    switch (int_width(t)) {
    case 8: return t.uns ? (long long)(uint8_t)x : (long long)(int8_t)x;
    case 16: return t.uns ? (long long)(uint16_t)x : (long long)(int16_t)x;
    case 32: return t.uns ? (long long)(uint32_t)x : (long long)(int32_t)x;
    default: return x;
    }
}

/* make an int/unsigned value's register hold the proper 64-bit sign/zero extension */
static Val clean32(Val v) {
    if (!is_int32(v.ty) || !v.dirty || v.op.k != OK_REG) { v.dirty = 0; return v; }
    int r = new_temp(RK_I);
    EMIT_OP(v.ty.uns ? I_UEXT32 : I_EXT32, R(r), v.op);
    Val c = rv_reg(r, v.ty);
    return c;
}

/* re-type an integer value as t: 8/16-bit results are extended, 32-bit ones may stay dirty */
static Val normalize(Val v, Type t) {
    v.ty = t;
    if (!is_integer(t) || int_width(t) == 64) return v;
    if (v.op.k == OK_INT) { v.op.i = trunc_const(v.op.i, t); v.dirty = 0; return v; }
    if (int_width(t) == 32) { v.dirty = 1; return v; }
    static const Insn sext[] = {I_EXT8, I_EXT16, 0, I_EXT32}, zext[] = {I_UEXT8, I_UEXT16, 0, I_UEXT32};
    int w = int_width(t) / 8 - 1;
    int r = new_temp(RK_I);
    EMIT_OP(t.uns ? zext[w] : sext[w], R(r), v.op);
    return rv_reg(r, t);
}

/* integer promotions and the usual arithmetic conversions */
static Type promote(Type t) {
    if (is_integer(t) && int_width(t) < 32) return int_type();
    return unconst(t);
}

static Type common_type(Type a, Type b) {
    if (is_double(a) || is_double(b)) return mk_type(TY_DOUBLE);
    if (is_float(a) || is_float(b)) return mk_type(TY_FLOAT);
    a = promote(a);
    b = promote(b);
    Type r;
    if (a.base == TY_LONG || b.base == TY_LONG) {
        r = long_type();
        r.uns = (a.base == TY_LONG && a.uns) || (b.base == TY_LONG && b.uns);
    } else {
        r = int_type();
        r.uns = a.uns || b.uns;
    }
    return r;
}

/* const-correctness of an implicit pointer conversion: target must keep every const of the source */
static void check_ptr_const(Type from, Type to) {
    if (is_const_obj(elem_of(from)) && !is_const_obj(elem_of(to)))
        error("conversion from '%s' to '%s' discards const", type_name(from), type_name(to));
}

/* conversion of a value to type `to`; explicit = cast */
static Val convert_ex(Val v, Type to, int is_explicit) {
    v = rvalue(v);
    if (is_void(to)) { v.ty = to; return v; }
    if (is_struct_val(to) || is_struct_val(v.ty)) {
        if (is_struct_val(to) && is_struct_val(v.ty) && to.sidx == v.ty.sidx) { v.ty = to; return v; }
        error("cannot convert '%s' to '%s'", type_name(v.ty), type_name(to));
    }
    if (is_void(v.ty)) error("void value used");
    if (is_bool(to)) {                                 /* _Bool: any nonzero value becomes 1 */
        if (v.op.k == OK_INT) return rv_op(IMM(v.op.i != 0), to);
        if (v.op.k == OK_FLT || v.op.k == OK_DBL) return rv_op(IMM(v.op.d != 0), to);
        if (is_bool(v.ty)) { v.ty = to; return v; }
        int t = new_temp(RK_I);
        if (is_float(v.ty)) EMIT(FNE, R(t), v.op, FIMM(0.0));
        else if (is_double(v.ty)) EMIT(DNE, R(t), v.op, DIMM(0.0));
        else if (!is_pointer(v.ty) && int_width(v.ty) == 32) EMIT(NES, R(t), v.op, IMM(0));
        else EMIT(NE, R(t), v.op, IMM(0));
        return rv_reg(t, to);
    }
    if (is_pointer(to)) {
        if (is_fp(v.ty)) error("cannot convert '%s' to a pointer", type_name(v.ty));
        v = clean32(v);
        if (is_pointer(v.ty) && !is_explicit) check_ptr_const(v.ty, to);
        v.ty = to;
        return v;
    }
    if (is_pointer(v.ty)) {
        if (is_fp(to)) error("cannot convert a pointer to '%s'", type_name(to));
        return normalize(v, to);
    }
    RegKind fr = reg_kind(v.ty), tr = reg_kind(to);
    if (v.op.k == OK_INT || v.op.k == OK_FLT || v.op.k == OK_DBL) {   /* fold constants */
        int from_fp = v.op.k != OK_INT;
        double dv = from_fp ? v.op.d : (v.ty.uns && int_width(v.ty) == 64 ? (double)(unsigned long long)v.op.i : (double)v.op.i);
        if (tr == RK_F) return rv_op(FIMM(dv), to);
        if (tr == RK_D) return rv_op(DIMM(dv), to);
        long long iv = from_fp ? (to.uns ? (long long)(unsigned long long)dv : (long long)dv) : v.op.i;
        return rv_op(IMM(trunc_const(iv, to)), to);
    }
    if (fr == RK_I && tr == RK_I) {
        if (int_width(to) > int_width(v.ty)) {        /* widening: needs the real 64-bit value */
            v = clean32(v);
            v.ty = to;
            return v;
        }
        if (int_width(to) == 32) {                    /* long -> int keeps only the low half */
            v.dirty = v.dirty || int_width(v.ty) > 32 || to.uns != v.ty.uns;
            v.ty = to;
            return v;
        }
        if (int_width(to) < int_width(v.ty) || to.uns != v.ty.uns) return normalize(v, to);
        v.ty = to;
        return v;
    }
    if (fr == RK_I) v = clean32(v);
    int t = new_temp(tr);
    if (fr == RK_I) {
        int u64 = v.ty.uns && int_width(v.ty) == 64;
        EMIT_OP(tr == RK_F ? (u64 ? I_UI2F : I_I2F) : (u64 ? I_UI2D : I_I2D), R(t), v.op);
        return rv_reg(t, to);
    }
    if (tr == RK_I) {
        EMIT_OP(fr == RK_F ? I_F2I : I_D2I, R(t), v.op);
        return normalize(rv_reg(t, long_type()), to);
    }
    if (fr == tr) { v.ty = to; return v; }
    EMIT_OP(fr == RK_F ? I_F2D : I_D2F, R(t), v.op);
    return rv_reg(t, to);
}

static Val convert(Val v, Type to) { return convert_ex(v, to, 0); }

/* memory helpers: struct copy and zero-initialisation */
static void copy_mem(int dbase, int ddisp, int sbase, int sdisp, int size) {
    int off = 0;
    while (off < size) {
        int chunk = size - off >= 8 ? 8 : size - off >= 4 ? 4 : 1;
        MemT mt = chunk == 8 ? MT_I64 : chunk == 4 ? MT_U32 : MT_U8;
        int t = new_temp(RK_I);
        EMIT(MOV, R(t), MEM(mt, sdisp + off, sbase));
        EMIT(MOV, MEM(mt, ddisp + off, dbase), R(t));
        off += chunk;
    }
}

static Val call_builtin(const char *name, Val *args, int n);

static void zero_mem(int base, int disp, int size) {
    if (size > 128) {                        /* big: memset */
        Val a[3] = {rv_reg(addr_reg(base, disp), ptr_to(mk_type(TY_VOID))), rv_op(IMM(0), int_type()),
                    rv_op(IMM(size), long_type())};
        call_builtin("__c2_memset", a, 3);
        return;
    }
    for (int off = 0; off < size;) {
        int chunk = size - off >= 8 ? 8 : size - off >= 4 ? 4 : 1;
        EMIT(MOV, MEM(chunk == 8 ? MT_I64 : chunk == 4 ? MT_U32 : MT_U8, disp + off, base), IMM(0));
        off += chunk;
    }
}

/* assignment; init = 1 for initialisation (const objects may be initialised) */
static Val store_ex(Val lv, Val rhs, int init) {
    if (lv.k == V_RVAL) error("assignment to something that is not an lvalue");
    if (is_array(lv.ty)) error("cannot assign to an array");
    if (is_func(lv.ty)) error("cannot assign to a function");
    if (!init && is_const_obj(lv.ty)) error("assignment to a const object of type '%s'", type_name(lv.ty));
    Type dst = lv.ty;
    dst = unconst(dst);                              /* the stored value isn't const */
    if (dst.bfw) {                                   /* bit-field: read-modify-write of its unit */
        Type pt = bf_plain(dst);
        Val v = rvalue(convert(rhs, pt));
        unsigned long long mask = dst.bfw >= 64 ? ~0ull : (1ull << dst.bfw) - 1;
        int u = new_temp(RK_I), nv = new_temp(RK_I);
        MemT ut = bf_unit_type(pt);
        EMIT_OP(I_MOV, R(u), val_mem(lv, ut));
        EMIT(AND, R(u), R(u), IMM((long long)~(mask << dst.bfp)));
        EMIT(AND, R(nv), v.op, IMM((long long)mask));
        if (dst.bfp) EMIT(LSH, R(nv), R(nv), IMM(dst.bfp));
        EMIT(OR, R(u), R(u), R(nv));
        EMIT_OP(I_MOV, val_mem(lv, ut), R(u));
        return bf_load(lv);                          /* the value the field now holds */
    }
    Val v = convert(rhs, dst);
    if (lv.k == V_SRA) {                             /* struct local in registers: load each member */
        if (v.k != V_MEM) error("internal: struct value not in memory");
        sra_load(&vars[lv.base], v.base, v.disp);
        return lv;
    }
    if (is_struct_val(lv.ty)) {
        if (v.k != V_MEM) error("internal: struct value not in memory");
        copy_mem(lv.base, lv.disp, v.base, v.disp, size_of(lv.ty));
        return lv;
    }
    RegKind rk = reg_kind(lv.ty);
    if (lv.k == V_REG) EMIT_OP(mov_insn(rk), R(lv.op.r), v.op);
    else EMIT_OP(mov_insn(rk), val_mem(lv, mem_type(lv.ty)), v.op);
    return v;
}

static Val store(Val lv, Val rhs) { return store_ex(lv, rhs, 0); }

/* ---------------- arithmetic */
static Val ptr_add(TokenType op, Val p, Val n) {
    n = convert(n, long_type());
    int esz = elem_size(p.ty);
    Opnd off;
    if (n.op.k == OK_INT) off = IMM(n.op.i * esz);
    else if (esz == 1) off = n.op;
    else { int t = new_temp(RK_I); EMIT(MUL, R(t), n.op, IMM(esz)); off = R(t); }
    int t = new_temp(RK_I);
    if (op == TOKEN_PLUS) EMIT(ADD, R(t), p.op, off);
    else EMIT(SUB, R(t), p.op, off);
    return rv_reg(t, p.ty);
}

/* compile-time evaluation when both operands are integer constants */
static int fold_int(TokenType op, long long a, long long b, Type t, long long *res) {
    unsigned long long ua = (unsigned long long)a, ub = (unsigned long long)b;
    switch (op) {
    case TOKEN_PLUS: *res = (long long)(ua + ub); break;
    case TOKEN_MINUS: *res = (long long)(ua - ub); break;
    case TOKEN_ASTERISK: *res = (long long)(ua * ub); break;
    case TOKEN_SLASH: case TOKEN_PERCENT:
        if (b == 0) return 0;
        if (t.uns) *res = op == TOKEN_SLASH ? (long long)(ua / ub) : (long long)(ua % ub);
        else if (a == LLONG_MIN && b == -1) return 0;
        else *res = op == TOKEN_SLASH ? a / b : a % b;
        break;
    case TOKEN_AMPERSAND: *res = a & b; break;
    case TOKEN_PIPE: *res = a | b; break;
    case TOKEN_CARET: *res = a ^ b; break;
    default: return 0;
    }
    *res = trunc_const(*res, t);
    return 1;
}

static Val arith(TokenType op, Val a, Val b) {
    a = rvalue(a);
    b = rvalue(b);
    if (is_struct_val(a.ty) || is_struct_val(b.ty)) error("arithmetic on a struct");
    if (op == TOKEN_PLUS || op == TOKEN_MINUS) {
        if (is_pointer(a.ty) && is_integer(b.ty)) return ptr_add(op, a, b);
        if (op == TOKEN_PLUS && is_pointer(b.ty) && is_integer(a.ty)) return ptr_add(op, b, a);
        if (op == TOKEN_MINUS && is_pointer(a.ty) && is_pointer(b.ty)) {
            int t = new_temp(RK_I), esz = elem_size(a.ty);
            EMIT(SUB, R(t), a.op, b.op);
            if (esz > 1) EMIT(DIV, R(t), R(t), IMM(esz));
            return rv_reg(t, long_type());
        }
    }
    if (!is_arith(a.ty) || !is_arith(b.ty)) error("invalid operands '%s' and '%s'", type_name(a.ty), type_name(b.ty));
    Type ct = common_type(a.ty, b.ty);
    int bitwise = op == TOKEN_AMPERSAND || op == TOKEN_PIPE || op == TOKEN_CARET;
    if (is_fp(ct) && (bitwise || op == TOKEN_PERCENT)) error("invalid operand of type '%s'", type_name(ct));
    a = convert(a, ct);
    b = convert(b, ct);
    if (a.op.k == OK_INT && b.op.k == OK_INT) {
        long long r;
        if (fold_int(op, a.op.i, b.op.i, ct, &r)) return rv_op(IMM(r), ct);
    }
    RegKind rk = reg_kind(ct);
    Insn code;
    if (rk == RK_F) code = op == TOKEN_PLUS ? I_FADD : op == TOKEN_MINUS ? I_FSUB : op == TOKEN_ASTERISK ? I_FMUL : I_FDIV;
    else if (rk == RK_D) code = op == TOKEN_PLUS ? I_DADD : op == TOKEN_MINUS ? I_DSUB : op == TOKEN_ASTERISK ? I_DMUL : I_DDIV;
    else if (is_int32(ct)) switch (op) {              /* 32-bit: result is dirty */
        case TOKEN_PLUS: code = I_ADDS; break;
        case TOKEN_MINUS: code = I_SUBS; break;
        case TOKEN_ASTERISK: code = I_MULS; break;
        case TOKEN_SLASH: code = ct.uns ? I_UDIVS : I_DIVS; break;
        case TOKEN_PERCENT: code = ct.uns ? I_UMODS : I_MODS; break;
        case TOKEN_AMPERSAND: code = I_ANDS; break;
        case TOKEN_PIPE: code = I_ORS; break;
        default: code = I_XORS; break;
        }
    else switch (op) {
        case TOKEN_PLUS: code = I_ADD; break;
        case TOKEN_MINUS: code = I_SUB; break;
        case TOKEN_ASTERISK: code = I_MUL; break;
        case TOKEN_SLASH: code = ct.uns ? I_UDIV : I_DIV; break;
        case TOKEN_PERCENT: code = ct.uns ? I_UMOD : I_MOD; break;
        case TOKEN_AMPERSAND: code = I_AND; break;
        case TOKEN_PIPE: code = I_OR; break;
        default: code = I_XOR; break;
        }
    int t = new_temp(rk);
    EMIT_OP(code, R(t), a.op, b.op);
    Val r = rv_reg(t, ct);
    r.dirty = is_int32(ct);
    return r;
}

static Val shift(TokenType op, Val a, Val b) {
    a = rvalue(a);
    b = rvalue(b);
    if (!is_integer(a.ty) || !is_integer(b.ty)) error("shift needs integer operands");
    Type t = promote(a.ty);
    a = convert(a, t);
    b = convert(b, int_type());
    if (a.op.k == OK_INT && b.op.k == OK_INT && b.op.i >= 0 && b.op.i < 64) {
        long long r = op == TOKEN_SHL ? (long long)((unsigned long long)a.op.i << b.op.i)
                    : t.uns ? (long long)((unsigned long long)a.op.i >> b.op.i) : a.op.i >> b.op.i;
        return rv_op(IMM(trunc_const(r, t)), t);
    }
    int r = new_temp(RK_I);
    if (is_int32(t)) {
        EMIT_OP(op == TOKEN_SHL ? I_LSHS : t.uns ? I_URSHS : I_RSHS, R(r), a.op, b.op);
        Val v = rv_reg(r, t);
        v.dirty = 1;
        return v;
    }
    EMIT_OP(op == TOKEN_SHL ? I_LSH : t.uns ? I_URSH : I_RSH, R(r), a.op, b.op);
    return rv_reg(r, t);
}

static Val compare(TokenType op, Val a, Val b) {
    a = rvalue(a);
    b = rvalue(b);
    static const TokenType ops[] = {TOKEN_EQ, TOKEN_NE, TOKEN_LT, TOKEN_LE, TOKEN_GT, TOKEN_GE};
    static const Insn scode[] = {I_EQ, I_NE, I_LT, I_LE, I_GT, I_GE};
    static const Insn ucode[] = {I_EQ, I_NE, I_ULT, I_ULE, I_UGT, I_UGE};
    static const Insn fcode[] = {I_FEQ, I_FNE, I_FLT, I_FLE, I_FGT, I_FGE};
    static const Insn dcode[] = {I_DEQ, I_DNE, I_DLT, I_DLE, I_DGT, I_DGE};
    static const Insn s32code[] = {I_EQS, I_NES, I_LTS, I_LES, I_GTS, I_GES};
    static const Insn u32code[] = {I_EQS, I_NES, I_ULTS, I_ULES, I_UGTS, I_UGES};
    int k = 0;
    while (ops[k] != op) k++;
    const Insn *codes;
    if (is_pointer(a.ty) || is_pointer(b.ty)) {
        if (!(is_pointer(a.ty) || is_integer(a.ty)) || !(is_pointer(b.ty) || is_integer(b.ty)))
            error("invalid comparison");
        a = clean32(a);
        b = clean32(b);
        codes = ucode;
    } else {
        if (!is_arith(a.ty) || !is_arith(b.ty)) error("invalid comparison of '%s' and '%s'", type_name(a.ty), type_name(b.ty));
        Type ct = common_type(a.ty, b.ty);
        a = convert(a, ct);
        b = convert(b, ct);
        codes = is_float(ct) ? fcode : is_double(ct) ? dcode : is_int32(ct) ? (ct.uns ? u32code : s32code)
              : ct.uns ? ucode : scode;
    }
    int t = new_temp(RK_I);
    EMIT_OP(codes[k], R(t), a.op, b.op);
    return rv_reg(t, int_type());
}

/* integer register that is non-zero iff v is "true" */
static int cond_reg(Val v) {
    v = rvalue(v);
    if (is_struct_val(v.ty) || is_void(v.ty)) error("scalar value expected in a condition");
    if (is_fp(v.ty)) {
        int t = new_temp(RK_I);
        if (is_float(v.ty)) EMIT(FNE, R(t), v.op, FIMM(0.0));
        else EMIT(DNE, R(t), v.op, DIMM(0.0));
        return t;
    }
    if (is_int32(v.ty) && v.dirty) {                  /* only the low 32 bits count */
        int t = new_temp(RK_I);
        EMIT(NES, R(t), v.op, IMM(0));
        return t;
    }
    return to_reg(v);
}

/* ============================================================ constant expressions
 * Array sizes, case labels, enum values and global initializers.  Evaluated at compile time over
 * the token stream (no code emitted).  ce_fail is set instead of erroring so callers can fall back. */
typedef struct { int fp; long long i; double d; } CV;
static int ce_fail;

static CV cv_i(long long i) { CV c = {0, i, (double)i}; return c; }
static CV cv_d(double d)    { CV c = {1, (long long)d, d}; return c; }

static int is_type_start(Token t);
static Type parse_type_spec(void);
static Type parse_type_name(void);
static CV ce_cond(void);
static Val parse_unary(void);

static CV ce_unary(void) {
    Token t = current_token;
    switch (t.type) {
    case TOKEN_NUM: next_token(); return cv_i(t.val);
    case TOKEN_FLOAT_NUM: next_token(); return cv_d(t.num_flags & NUM_FLOAT32 ? (float)t.float_val : t.float_val);
    case TOKEN_NULL: next_token(); return cv_i(0);
    case TOKEN_MINUS: { next_token(); CV v = ce_unary(); return v.fp ? cv_d(-v.d) : cv_i((long long)(0ull - (unsigned long long)v.i)); }
    case TOKEN_PLUS: next_token(); return ce_unary();
    case TOKEN_TILDE: { next_token(); CV v = ce_unary(); if (v.fp) ce_fail = 1; return cv_i(~v.i); }
    case TOKEN_NOT: { next_token(); CV v = ce_unary(); return cv_i(v.fp ? v.d == 0 : v.i == 0); }
    case TOKEN_IDENT: {
        EnumConst *e = find_enum_const(t.text);
        if (e && !find_var(t.text)) { next_token(); return cv_i(e->val); }
        ce_fail = 1;
        return cv_i(0);
    }
    case TOKEN_ALIGNOF: {
        next_token();
        expect(TOKEN_LPAREN);
        Type ty = parse_type_name();
        expect(TOKEN_RPAREN);
        return cv_i(align_of(ty));
    }
    case TOKEN_SIZEOF:
        next_token();
        if (current_token.type == TOKEN_LPAREN && is_type_start(peek_token())) {
            next_token();
            Type ty = parse_type_name();
            expect(TOKEN_RPAREN);
            return cv_i(size_of(ty));
        }
        if (cur_func) {                              /* sizeof expr: type of a dry run */
            EmitMark em = emit_mark();
            no_uninit_warn++;
            Type ty = parse_unary().ty;
            no_uninit_warn--;
            emit_rollback(em);
            return cv_i(size_of(ty));
        }
        ce_fail = 1;
        return cv_i(0);
    case TOKEN_LPAREN:
        next_token();
        if (is_type_start(current_token)) {          /* cast */
            Type ty = parse_type_name();
            expect(TOKEN_RPAREN);
            CV v = ce_unary();
            if (is_fp(ty)) return cv_d(v.fp ? v.d : (double)v.i);
            if (is_integer(ty)) return cv_i(trunc_const(v.fp ? (long long)v.d : v.i, ty));
            return v;
        } else {
            CV v = ce_cond();
            if (current_token.type != TOKEN_RPAREN) { ce_fail = 1; return v; }
            next_token();
            return v;
        }
    default:
        ce_fail = 1;
        return cv_i(0);
    }
}

static int ce_prec(TokenType t) {
    switch (t) {
    case TOKEN_ASTERISK: case TOKEN_SLASH: case TOKEN_PERCENT: return 10;
    case TOKEN_PLUS: case TOKEN_MINUS: return 9;
    case TOKEN_SHL: case TOKEN_SHR: return 8;
    case TOKEN_LT: case TOKEN_LE: case TOKEN_GT: case TOKEN_GE: return 7;
    case TOKEN_EQ: case TOKEN_NE: return 6;
    case TOKEN_AMPERSAND: return 5;
    case TOKEN_CARET: return 4;
    case TOKEN_PIPE: return 3;
    case TOKEN_ANDAND: return 2;
    case TOKEN_OROR: return 1;
    default: return 0;
    }
}

static CV ce_binary(int min_prec) {
    CV a = ce_unary();
    for (;;) {
        TokenType op = current_token.type;
        int p = ce_prec(op);
        if (p == 0 || p < min_prec || ce_fail) return a;
        next_token();
        CV b = ce_binary(p + 1);
        if (a.fp || b.fp) {
            double x = a.fp ? a.d : (double)a.i, y = b.fp ? b.d : (double)b.i;
            switch (op) {
            case TOKEN_ASTERISK: a = cv_d(x * y); break;
            case TOKEN_SLASH: a = cv_d(x / y); break;
            case TOKEN_PLUS: a = cv_d(x + y); break;
            case TOKEN_MINUS: a = cv_d(x - y); break;
            case TOKEN_LT: a = cv_i(x < y); break;
            case TOKEN_LE: a = cv_i(x <= y); break;
            case TOKEN_GT: a = cv_i(x > y); break;
            case TOKEN_GE: a = cv_i(x >= y); break;
            case TOKEN_EQ: a = cv_i(x == y); break;
            case TOKEN_NE: a = cv_i(x != y); break;
            case TOKEN_ANDAND: a = cv_i(x && y); break;
            case TOKEN_OROR: a = cv_i(x || y); break;
            default: ce_fail = 1;
            }
            continue;
        }
        long long x = a.i, y = b.i;
        switch (op) {
        case TOKEN_ASTERISK: a = cv_i((long long)((unsigned long long)x * (unsigned long long)y)); break;
        case TOKEN_SLASH: if (y == 0) ce_fail = 1; else a = cv_i(x / y); break;
        case TOKEN_PERCENT: if (y == 0) ce_fail = 1; else a = cv_i(x % y); break;
        case TOKEN_PLUS: a = cv_i((long long)((unsigned long long)x + (unsigned long long)y)); break;
        case TOKEN_MINUS: a = cv_i((long long)((unsigned long long)x - (unsigned long long)y)); break;
        case TOKEN_SHL: a = cv_i((long long)((unsigned long long)x << (y & 63))); break;
        case TOKEN_SHR: a = cv_i(x >> (y & 63)); break;
        case TOKEN_LT: a = cv_i(x < y); break;
        case TOKEN_LE: a = cv_i(x <= y); break;
        case TOKEN_GT: a = cv_i(x > y); break;
        case TOKEN_GE: a = cv_i(x >= y); break;
        case TOKEN_EQ: a = cv_i(x == y); break;
        case TOKEN_NE: a = cv_i(x != y); break;
        case TOKEN_AMPERSAND: a = cv_i(x & y); break;
        case TOKEN_CARET: a = cv_i(x ^ y); break;
        case TOKEN_PIPE: a = cv_i(x | y); break;
        case TOKEN_ANDAND: a = cv_i(x && y); break;
        case TOKEN_OROR: a = cv_i(x || y); break;
        default: ce_fail = 1;
        }
    }
}

static CV ce_cond(void) {
    CV c = ce_binary(1);
    if (current_token.type != TOKEN_QUESTION || ce_fail) return c;
    next_token();
    CV a = ce_cond();
    if (current_token.type != TOKEN_COLON) { ce_fail = 1; return c; }
    next_token();
    CV b = ce_cond();
    return (c.fp ? c.d != 0 : c.i != 0) ? a : b;
}

static long long const_int_expr(void) {
    ce_fail = 0;
    CV v = ce_cond();
    if (ce_fail) error("integer constant expression expected");
    if (v.fp) error("integer constant expression expected (got a floating value)");
    return v.i;
}

/* try to read a constant initializer that is followed by ',' ';' or '}'; restores the lexer on failure */
static int try_const_init(CV *out) {
    LexState s = save_lex();
    ce_fail = 0;
    CV v = ce_cond();
    TokenType n = current_token.type;
    if (ce_fail || (n != TOKEN_COMMA && n != TOKEN_SEMI && n != TOKEN_RBRACE)) { restore_lex(s); return 0; }
    *out = v;
    return 1;
}

/* ============================================================ parser: types */
static int spec_static, spec_extern;   /* set by parse_type_spec when it saw `static` / `extern` */
static int spec_constexpr;             /* ... and C23 `constexpr` */
static int spec_typedef;               /* ... and `typedef` after other specifiers */
static int spec_align;                  /* _Alignas(n) of the declaration, 0 = none */

/* .c2: a struct/union tag is also a type name (`Point p;`), unless something else has the name */
static Func *find_func(const char *name);
static int implicit_struct(const char *name) {
    if (!c2_ext) return -1;
    int si = find_struct(name);
    if (si < 0 || find_var(name) || find_global(name) || find_func(name) || find_enum_const(name)) return -1;
    return si;
}

static int is_type_start(Token t) {
    switch (t.type) {
    case TOKEN_INT: case TOKEN_CHAR: case TOKEN_SHORT: case TOKEN_LONG: case TOKEN_UNSIGNED: case TOKEN_SIGNED:
    case TOKEN_FLOAT: case TOKEN_DOUBLE: case TOKEN_VOID: case TOKEN_STRUCT: case TOKEN_UNION: case TOKEN_ENUM:
    case TOKEN_CONST: case TOKEN_STATIC: case TOKEN_EXTERN: case TOKEN_REGISTER: case TOKEN_BOOL: case TOKEN_ALIGNAS:
    case TOKEN_TYPEOF: case TOKEN_TYPEOF_UNQUAL: case TOKEN_AUTO: case TOKEN_CONSTEXPR:
        return 1;
    case TOKEN_IDENT: return (find_typedef(t.text) && !find_var(t.text)) || implicit_struct(t.text) >= 0;
    default: return 0;
    }
}

static Type parse_ptrs(Type t) {
    while (current_token.type == TOKEN_ASTERISK) {
        t = ptr_to(t);
        next_token();
        while (current_token.type == TOKEN_CONST) { t = add_const(t); next_token(); }   /* T *const p */
    }
    return t;
}

static Type parse_declarator(Type base, char *name, int name_optional);

/* ( parameter-list ) -> signature index; current token is after '(' */
static int parse_params_in(int si);

static int parse_params(void) {   /* parameters have their own scope for tags and enumerators */
    int si = new_sig();
    scope_depth++;
    param_depth++;
    int saved_vla_ok = vla_ok;
    vla_ok = 0;
    parse_params_in(si);
    vla_ok = saved_vla_ok;
    param_depth--;
    hide_structs(scope_depth);
    while (nenum_consts > 0 && enum_consts[nenum_consts - 1].depth >= scope_depth) nenum_consts--;
    while (nenum_tags > 0 && enum_tags[nenum_tags - 1].depth >= scope_depth) nenum_tags--;
    while (nfunc_aliases > 0 && func_aliases[nfunc_aliases - 1].depth >= scope_depth) nfunc_aliases--;
    scope_depth--;
    return si;
}

static int parse_params_in(int si) {
    if (current_token.type == TOKEN_RPAREN) { next_token(); sigs[si].unprototyped = 1; return si; }
    if (current_token.type == TOKEN_VOID && peek_token().type == TOKEN_RPAREN) { next_token(); next_token(); return si; }
    if (current_token.type == TOKEN_IDENT && !is_type_start(current_token)) {   /* K&R: f(a, b) */
        Sig *g = &sigs[si];
        g->knr = 1;
        for (;;) {
            if (current_token.type != TOKEN_IDENT) error("parameter name expected");
            if (g->np >= MAX_PARAMS) error("too many parameters");
            g->pt[g->np] = int_type();
            snprintf(g->pn[g->np++], 32, "%s", current_token.text);
            next_token();
            if (current_token.type != TOKEN_COMMA) break;
            next_token();
        }
        expect(TOKEN_RPAREN);
        return si;
    }
    for (;;) {
        if (current_token.type == TOKEN_ELLIPSIS) {
            if (sigs[si].np == 0) error("'...' needs at least one named parameter");
            sigs[si].vararg = 1;
            next_token();
            break;
        }
        if (sigs[si].np >= MAX_PARAMS) error("too many parameters");
        char pname[32];
        decl_saw_static = 0;
        Type pt = parse_declarator(parse_type_spec(), pname, 1);
        if (c2_ext && is_array(pt) && arr_len(pt) > 0 && !decl_saw_static) {   /* .c2: int a[3] means exactly 3 */
            Sig *g = &sigs[si];
            if (!g->plen) g->plen = cc_calloc(MAX_PARAMS, sizeof *g->plen);
            g->plen[g->np] = arr_len(pt);
        }
        if (is_array(pt)) pt = ptr_to(elem_of(pt));       /* int a[] -> int *a */
        else if (is_func(pt)) pt = ptr_to(pt);            /* int f(int) -> int (*f)(int) */
        if (is_void(pt)) error("parameter of type void");
        Sig *g = &sigs[si];
        g->pt[g->np] = pt;
        snprintf(g->pn[g->np], 32, "%s", pname);
        if (current_token.type == TOKEN_ASSIGN) {      /* .c2: int f(int a, int b = 2) */
            if (!c2_ext) error("default parameter values need a .c2 file");
            const char *start = src;                   /* just after '=' */
            next_token();
            int depth = 0;
            for (; current_token.type != TOKEN_EOF; next_token()) {
                TokenType ty = current_token.type;
                if (ty == TOKEN_LPAREN || ty == TOKEN_LBRACKET || ty == TOKEN_LBRACE) depth++;
                else if (ty == TOKEN_RPAREN || ty == TOKEN_RBRACKET || ty == TOKEN_RBRACE) { if (depth-- == 0) break; }
                else if (ty == TOKEN_COMMA && depth == 0) break;
            }
            const char *end = src - 1;                 /* the ',' or ')' just read */
            while (end > start && isspace((unsigned char)end[-1])) end--;
            if (end <= start) error("default value expected for parameter '%s'", pname);
            if (!g->pdef) g->pdef = cc_calloc(MAX_PARAMS, sizeof *g->pdef);
            char *text = cc_malloc(end - start + 1);
            memcpy(text, start, end - start);
            text[end - start] = '\0';
            g->pdef[g->np] = text;
        } else if (g->pdef && g->np > 0 && g->pdef[g->np - 1]) {
            error("parameter '%s' needs a default value: it follows one that has one", pname);
        }
        g->np++;
        if (current_token.type != TOKEN_COMMA) break;
        next_token();
    }
    expect(TOKEN_RPAREN);
    return si;
}

/* [N] and (params) after a declarator name; applied right to left: a[2][3] = array 2 of array 3 */
static Val parse_assign(void);
static Type parse_decl_suffixes(Type t) {
    int kind[MAX_DERIV], n[MAX_DERIV], ns = 0;
    for (;;) {
        if (current_token.type == TOKEN_LBRACKET) {
            next_token();
            long long len = -1;
            while (current_token.type == TOKEN_STATIC || current_token.type == TOKEN_CONST) {   /* f(int a[static 3]) */
                if (current_token.type == TOKEN_STATIC) decl_saw_static = 1;
                next_token();
            }
            if (current_token.type == TOKEN_ASTERISK && peek_token().type == TOKEN_RBRACKET) next_token();   /* f(int a[*]) */
            else if (current_token.type != TOKEN_RBRACKET) {
                LexState ls = save_lex();
                ce_fail = 0;
                CV cv = ce_cond();
                if (!ce_fail && !cv.fp && current_token.type == TOKEN_RBRACKET) {
                    len = cv.i;
                    if (len <= 0) error("array size must be positive");
                } else if (param_depth > 0 && ns == 0) {   /* f(int n, int a[n]): decays, the length is unused */
                    restore_lex(ls);
                    for (int d = 0; current_token.type != TOKEN_EOF; next_token()) {
                        if (current_token.type == TOKEN_LBRACKET) d++;
                        else if (current_token.type == TOKEN_RBRACKET && d-- == 0) break;
                    }
                } else if (vla_ok && cur_func && ns == 0) {   /* int a[n]: variable-length array */
                    restore_lex(ls);
                    if (nvla >= 512) error("too many variable-length arrays");
                    Val n = rvalue(convert(parse_assign(), long_type()));
                    int r = new_temp(RK_I);
                    EMIT(MOV, R(r), n.op);
                    vla_len_reg[nvla] = r;
                    len = VLA_MARK(nvla++);
                } else {
                    error(vla_ok && cur_func ? "only the first array dimension may be variable"
                                             : "integer constant expression expected");
                }
            }
            expect(TOKEN_RBRACKET);
            if (ns > 0 && len == -1) error("only the first array dimension may be omitted");
            if (ns >= MAX_DERIV) error("type too complex");
            kind[ns] = D_ARR;
            n[ns++] = (int)len;
        } else if (current_token.type == TOKEN_LPAREN) {
            next_token();
            if (ns >= MAX_DERIV) error("type too complex");
            kind[ns] = D_FUNC;
            n[ns++] = parse_params();
        } else {
            break;
        }
    }
    for (int i = ns - 1; i >= 0; i--) {
        if (kind[i] == D_ARR) {
            if (is_func(t)) error("array of functions");
            t = array_of(t, n[i]);
        } else {
            if (is_func(t) || is_array(t)) error("function returning a %s", is_func(t) ? "function" : "array");
            sigs[n[i]].ret = t;
            t = func_of(t, n[i]);
        }
    }
    return t;
}

static int nested_declarator_follows(void) {   /* current token is '(' */
    Token p = peek_token();
    return p.type == TOKEN_ASTERISK || p.type == TOKEN_LPAREN || p.type == TOKEN_LBRACKET ||
           (p.type == TOKEN_IDENT && !is_type_start(p));
}

/* full C declarator: pointers, name (optional for abstract declarators), arrays, functions,
   parentheses:  int (*fp)(int),  int (*tab[4])(void),  int (*p)[3],  int m[2][3] ... */
static Type parse_declarator(Type base, char *name, int name_optional) {
    Type t = parse_ptrs(base);
    name[0] = '\0';
    if (current_token.type == TOKEN_LPAREN && nested_declarator_follows()) {
        next_token();
        LexState inner = save_lex();
        for (int depth = 0; !(current_token.type == TOKEN_RPAREN && depth == 0); next_token()) {
            if (current_token.type == TOKEN_EOF) error("unbalanced parentheses in declarator");
            if (current_token.type == TOKEN_LPAREN) depth++;
            if (current_token.type == TOKEN_RPAREN) depth--;
        }
        next_token();
        Type outer = parse_decl_suffixes(t);           /* what the inner declarator is applied to */
        LexState after = save_lex();
        restore_lex(inner);
        Type r = parse_declarator(outer, name, name_optional);
        expect(TOKEN_RPAREN);
        restore_lex(after);
        return r;
    }
    if (current_token.type == TOKEN_IDENT) {
        snprintf(name, 32, "%.31s", current_token.text);
        next_token();
    } else if (!name_optional) {
        error("name expected");
    }
    return parse_decl_suffixes(t);
}

/* abstract declarator after a type: casts, sizeof */
static Type parse_type_name(void) {
    char dummy[32];
    Type t = parse_declarator(parse_type_spec(), dummy, 1);
    if (dummy[0]) error("unexpected name '%s' in type", dummy);
    return t;
}

/* _Static_assert(constant, "message");  (the message is optional, as in C23) */
static void parse_static_assert(void) {
    next_token();
    expect(TOKEN_LPAREN);
    long long v = const_int_expr();
    char msg[200] = "";
    if (current_token.type == TOKEN_COMMA) {
        next_token();
        if (current_token.type != TOKEN_STR) error("string literal expected in _Static_assert");
        snprintf(msg, sizeof msg, "%.*s", current_token.str_len, current_token.str);
        next_token();
    }
    expect(TOKEN_RPAREN);
    expect(TOKEN_SEMI);
    if (!v) error("static assertion failed%s%s", msg[0] ? ": " : "", msg);
}

static Type parse_struct_spec(void) {   /* current token: struct or union */
    int is_union = current_token.type == TOKEN_UNION;
    next_token();
    char name[32] = "";
    if (current_token.type == TOKEN_IDENT) { snprintf(name, sizeof name, "%.31s", current_token.text); next_token(); }
    else if (current_token.type != TOKEN_LBRACE) error("%s name or '{' expected", is_union ? "union" : "struct");
    int si = name[0] ? find_struct(name) : -1;
    if (current_token.type == TOKEN_LBRACE ||
        (current_token.type == TOKEN_SEMI && name[0])) {   /* definition or `struct T;`: a tag in this scope */
        if (si >= 0 && structs[si].depth == scope_depth) {
            if (current_token.type == TOKEN_LBRACE && structs[si].complete)
                error("redefinition of %s %s", is_union ? "union" : "struct", name);
        } else {
            si = new_struct(name, is_union);
        }
    } else if (si < 0) {
        si = new_struct(name, is_union);                 /* first mention: incomplete */
    }
    if (structs[si].is_union != is_union) error("'%s' defined as the wrong kind of tag", name);
    Type ty = mk_type(TY_STRUCT);
    ty.sidx = si;
    if (current_token.type != TOKEN_LBRACE) return ty;
    next_token();
    int off = 0, size = 0, align = 1;
    long long boff = 0;                                  /* bit-fields: next free bit */
    int ms_unit = -1, ms_size = 0, ms_used = 0;          /* MSVC: the open bit-field unit */
    (void)ms_unit; (void)ms_size; (void)ms_used;
    while (current_token.type != TOKEN_RBRACE) {
        if (current_token.type == TOKEN_STATIC_ASSERT) { parse_static_assert(); continue; }
        Type base = parse_type_spec();
        int member_align = spec_align;
        if (current_token.type == TOKEN_SEMI && is_struct_val(base) && structs[base.sidx].name[0] == '<') {
            next_token();                                /* anonymous struct/union member (C11) */
            Struct *st = &structs[si];
            if (st->n >= 64) error("too many members");
            int al = align_of(base);
            if (!is_union) { off = (int)((boff + 7) / 8); off = (off + al - 1) / al * al; }
            ms_unit = -1;
            st->m[st->n].name[0] = '\0';
            st->m[st->n].ty = base;
            st->m[st->n++].off = is_union ? 0 : off;
            if (al > align) align = al;
            if (is_union) { if (size_of(base) > size) size = size_of(base); }
            else { off += size_of(base); boff = (long long)off * 8; }
            continue;
        }
        for (;;) {
            char mname[32] = "";
            Type mt = current_token.type == TOKEN_COLON ? base : parse_declarator(base, mname, 0);
            Struct *st = &structs[si];
            if (current_token.type == TOKEN_COLON) {   /* bit-field: type name : width */
                next_token();
                long long w = const_int_expr();
                if (!is_integer(mt)) error("bit-field '%s' has non-integer type '%s'", mname, type_name(mt));
                int sz = size_of(mt), bits = is_bool(mt) ? 1 : sz * 8;
                if (w < 0 || w > bits) error("width of bit-field '%s' is invalid", mname);
                if (w == 0 && mname[0]) error("zero-width bit-field '%s' must be unnamed", mname);
                int unit, pos;
                if (is_union) { unit = 0; pos = 0; }
#if defined(_WIN32)
                else {                                 /* MSVC: units of the declared type's size */
                    if (w == 0) { ms_unit = -1; goto next_member; }
                    if (ms_unit < 0 || ms_size != sz || ms_used + w > sz * 8) {
                        int o = (int)((boff + 7) / 8);
                        o = (o + sz - 1) / sz * sz;
                        ms_unit = o; ms_size = sz; ms_used = 0;
                        boff = (long long)(o + sz) * 8;
                    }
                    unit = ms_unit; pos = ms_used; ms_used += (int)w;
                }
#else
                else {                                 /* SysV: no straddling of a unit of the type's size */
                    long long ub = (long long)sz * 8;
                    if (w == 0) { boff = (boff + ub - 1) / ub * ub; goto next_member; }
                    if (boff / ub != (boff + w - 1) / ub) boff = (boff + ub - 1) / ub * ub;
                    unit = (int)(boff / ub) * sz;
                    pos = (int)(boff - (long long)unit * 8);
                    boff += w;
                }
#endif
                if (mname[0]) {
                    if (st->n >= 64) error("too many members");
                    for (int i = 0; i < st->n; i++)
                        if (strcmp(st->m[i].name, mname) == 0) error("duplicate member '%s'", mname);
                    Member *m = &st->m[st->n++];
                    strcpy(m->name, mname);
                    m->ty = mt;
                    m->ty.bfw = (unsigned char)w;
                    m->ty.bfp = (unsigned char)pos;
                    m->off = unit;
                    int al = align_of(mt);
                    if (al > align) align = al;
                }
                if (is_union && sz > size) size = sz;
                goto next_member;
            }
            ms_unit = -1;
            if (is_array(mt) && arr_len(mt) < 0) {     /* flexible array member: int data[]; last */
                if (is_union || current_token.type != TOKEN_SEMI || peek_token().type != TOKEN_RBRACE || structs[si].n == 0)
                    error("flexible array member '%s' must be the last member of a struct", mname);
                mt = set_arr_len(mt, 0);
            }
            if (is_void(mt) || is_func(mt)) error("member '%s' has invalid type '%s'", mname, type_name(mt));
            if (st->n >= 64) error("too many members");
            for (int i = 0; i < st->n; i++)
                if (strcmp(st->m[i].name, mname) == 0) error("duplicate member '%s'", mname);
            Member *m = &st->m[st->n++];
            strcpy(m->name, mname);
            int al = align_of(mt);
            if (member_align > al) al = member_align;
            if (al > align) align = al;
            m->ty = mt;
            if (is_union) {
                m->off = 0;
                if (size_of(mt) > size) size = size_of(mt);
            } else {
                off = (int)((boff + 7) / 8);
                off = (off + al - 1) / al * al;
                m->off = off;
                off += size_of(mt);
                boff = (long long)off * 8;
            }
        next_member:
            if (current_token.type != TOKEN_COMMA) break;
            next_token();
        }
        expect(TOKEN_SEMI);
    }
    expect(TOKEN_RBRACE);
    Struct *st = &structs[si];
    if (!is_union) size = (int)((boff + 7) / 8);
    st->align = align;
    st->size = (size + align - 1) / align * align;
    if (st->size == 0) st->size = 1;
    st->complete = 1;
    return ty;
}

static EnumTag *find_enum_tag(const char *name) {
    for (int i = nenum_tags - 1; i >= 0; i--)
        if (enum_tags[i].depth <= scope_depth && !strcmp(enum_tags[i].name, name)) return &enum_tags[i];
    return NULL;
}

/* returns the enum's type: int, or the fixed underlying type of enum E : type (C23), or long long
   when an enumerator does not fit in an int */
static Type parse_enum_spec(void) {   /* current token: enum */
    next_token();
    char tag[32] = "";
    Type base = int_type();
    int fixed = 0;
    if (current_token.type == TOKEN_IDENT) {
        snprintf(tag, sizeof tag, "%.31s", current_token.text);
        next_token();
        EnumTag *et = find_enum_tag(tag);
        if (et) { base = et->base; fixed = 1; }
    }
    if (current_token.type == TOKEN_COLON) {          /* enum E : type */
        next_token();
        if (!is_type_start(current_token)) error("type expected after 'enum %s:'", tag);
        int st = spec_static, ex = spec_extern, sa = spec_align;
        Type b = parse_type_name();
        spec_static = st; spec_extern = ex; spec_align = sa;
        if (!is_integer(b)) error("the underlying type of an enum must be an integer type");
        b.bcq = 0;
        if (fixed && (b.base != base.base || b.uns != base.uns)) error("enum '%s' redeclared with another underlying type", tag);
        base = b;
        fixed = 1;
        if (tag[0] && !find_enum_tag(tag)) {
            if (nenum_tags >= 256) error("too many enum types");
            strcpy(enum_tags[nenum_tags].name, tag);
            enum_tags[nenum_tags].depth = scope_depth;
            enum_tags[nenum_tags++].base = b;
        }
    }
    if (current_token.type != TOKEN_LBRACE) return base;
    next_token();
    long long v = 0;
    int first = nenum_consts, wide = 0;
    while (current_token.type != TOKEN_RBRACE) {
        if (current_token.type != TOKEN_IDENT) error("enumerator name expected");
        char name[32];
        snprintf(name, sizeof name, "%.31s", current_token.text);
        next_token();
        if (current_token.type == TOKEN_ASSIGN) { next_token(); v = const_int_expr(); }
        if (fixed && trunc_const(v, base) != v) error("enumerator '%s' value %lld does not fit the enum's type", name, v);
        if (!fixed && (v < INT_MIN || v > INT_MAX)) wide = 1;
        EnumConst *old = find_enum_const(name);
        if (old && old->depth == scope_depth) error("redefinition of enumerator '%s'", name);
        if (nenum_consts >= 1024) error("too many enumerators");
        strcpy(enum_consts[nenum_consts].name, name);
        enum_consts[nenum_consts].depth = scope_depth;
        enum_consts[nenum_consts++].val = v;
        v++;
        if (current_token.type != TOKEN_COMMA) break;
        next_token();
    }
    expect(TOKEN_RBRACE);
    if (wide) base = long_type();                     /* C23: values beyond int widen the enum */
    for (int i = first; i < nenum_consts; i++) enum_consts[i].type = base;
    return base;
}

/* t without its top-level qualifiers (for an array, its elements'): typeof_unqual, auto */
static Type unqual(Type t) {
    int i = t.nd - 1;
    while (i >= 0 && t.d[i].kind == D_ARR) t.d[i--].cq = 0;
    if (i >= 0) t.d[i].cq = 0;
    else t.bcq = 0;
    return t;
}

static Type dry_run_type(Val (*parse)(void), int decay);
static Val parse_expr(void);
static Val parse_assign(void);

static Type parse_type_spec(void) {
    int cnst = 0, uns = 0, sgn = 0, nlong = 0, nshort = 0, have_base = 0, seen = 0, is_auto = 0, is_constexpr = 0;
    int is_typedef = 0;
    Type t = int_type();
    spec_static = spec_extern = spec_align = 0;
    for (;;) {
        Token k = current_token;
        if (k.type == TOKEN_CONST) cnst = 1;
        else if (k.type == TOKEN_CONSTEXPR) cnst = is_constexpr = 1;
        else if (k.type == TOKEN_TYPEDEF) is_typedef = 1;
        else if (k.type == TOKEN_AUTO) is_auto = 1;
        else if (k.type == TOKEN_TYPEOF || k.type == TOKEN_TYPEOF_UNQUAL) {   /* C23 typeof(expr or type) */
            if (have_base) error("two or more data types in declaration");
            next_token();
            expect(TOKEN_LPAREN);
            int st = spec_static, ex = spec_extern, sa = spec_align;
            if (is_type_start(current_token)) t = parse_type_name();
            else {
                t = dry_run_type(parse_expr, 0);       /* not evaluated; arrays stay arrays */
                for (int depth = 0; current_token.type != TOKEN_EOF; next_token()) {   /* skip the expression */
                    if (current_token.type == TOKEN_LPAREN) depth++;
                    else if (current_token.type == TOKEN_RPAREN && depth-- == 0) break;
                }
            }
            spec_static = st; spec_extern = ex; spec_align = sa;
            expect(TOKEN_RPAREN);
            if (k.type == TOKEN_TYPEOF_UNQUAL) t = unqual(t);
            have_base = seen = 1;
            continue;
        }
        else if (k.type == TOKEN_BOOL) {
            if (have_base) error("two or more data types in declaration");
            have_base = 1;
            t = mk_type(TY_CHAR);
            t.uns = 2;
        } else if (k.type == TOKEN_ALIGNAS) {          /* _Alignas(type) or _Alignas(constant) */
            next_token();
            expect(TOKEN_LPAREN);
            int al;
            if (is_type_start(current_token)) {
                int st = spec_static, ex = spec_extern, sa = spec_align;
                al = align_of(parse_type_name());
                spec_static = st; spec_extern = ex; spec_align = sa;
            } else al = (int)const_int_expr();
            expect(TOKEN_RPAREN);
            if (al & (al - 1)) error("alignment %d is not a power of two", al);
            if (al > spec_align) spec_align = al;
            seen = 1;
            continue;
        }
        else if (k.type == TOKEN_STATIC) spec_static = 1;
        else if (k.type == TOKEN_EXTERN) spec_extern = 1;
        else if (k.type == TOKEN_REGISTER) { }               /* storage class with no effect: register x; is an int */
        else if (k.type == TOKEN_UNSIGNED) uns = 1;
        else if (k.type == TOKEN_SIGNED) sgn = 1;
        else if (k.type == TOKEN_LONG) nlong++;
        else if (k.type == TOKEN_SHORT) nshort++;
        else if (k.type == TOKEN_INT || k.type == TOKEN_CHAR || k.type == TOKEN_FLOAT ||
                 k.type == TOKEN_DOUBLE || k.type == TOKEN_VOID) {
            if (have_base) error("two or more data types in declaration");
            have_base = 1;
            t = mk_type(k.type == TOKEN_INT ? TY_INT : k.type == TOKEN_CHAR ? TY_CHAR : k.type == TOKEN_FLOAT ? TY_FLOAT
                        : k.type == TOKEN_DOUBLE ? TY_DOUBLE : TY_VOID);
        } else if (k.type == TOKEN_STRUCT || k.type == TOKEN_UNION) {
            if (have_base) error("two or more data types in declaration");
            int st = spec_static, ex = spec_extern, sa = spec_align;
            t = parse_struct_spec();
            spec_static = st;
            spec_extern = ex;
            spec_align = sa;
            have_base = seen = 1;
            continue;
        } else if (k.type == TOKEN_ENUM) {
            if (have_base) error("two or more data types in declaration");
            t = parse_enum_spec();
            have_base = seen = 1;
            continue;
        } else if (k.type == TOKEN_IDENT && !have_base && !uns && !sgn && !nlong && !nshort &&
                   find_typedef(k.text) && !find_var(k.text)) {
            t = find_typedef(k.text)->ty;
            have_base = 1;
        } else if (k.type == TOKEN_IDENT && !have_base && !uns && !sgn && !nlong && !nshort &&
                   implicit_struct(k.text) >= 0) {   /* .c2: Point p; */
            t = mk_type(TY_STRUCT);
            t.sidx = implicit_struct(k.text);
            have_base = 1;
        } else {
            break;
        }
        seen = 1;
        next_token();
    }
    if (!seen) error("type expected");
    if (is_auto && !have_base && !uns && !sgn && !nlong && !nshort) {   /* C23: auto x = expr; */
        if (current_token.type != TOKEN_IDENT || peek_token().type != TOKEN_ASSIGN)
            error("'auto' needs an initializer to infer the type from (auto x = ...)");
        LexState ls = save_lex();
        next_token();
        next_token();
        int st = spec_static, ex = spec_extern, sa = spec_align;
        t = unqual(dry_run_type(parse_assign, 1));   /* arrays and functions decay, qualifiers go */
        spec_static = st; spec_extern = ex; spec_align = sa;
        restore_lex(ls);
        if (is_void(t)) error("'auto' variable initialized with a void expression");
        have_base = 1;
    }
    if (nshort) {
        if (have_base && t.base != TY_INT) error("invalid use of 'short'");
        t = mk_type(TY_SHORT);
    }
    if (nlong) {
        if (have_base && t.base == TY_DOUBLE && t.nd == 0) t = mk_type(TY_DOUBLE);   /* long double = double here */
        else if (have_base && (t.base != TY_INT || t.nd)) error("invalid use of 'long'");
        else t = long_type();
    }
    if (uns || sgn) {
        if (!is_integer(t) || is_bool(t)) error("'%s' with a non-integer type", uns ? "unsigned" : "signed");
        t.uns = uns;
    }
    if (cnst) t = add_const(t);
    spec_constexpr = is_constexpr;   /* set last: nested type names (typeof) must not clear it */
    spec_typedef = is_typedef;
    return t;
}

/* C23 constexpr: an integer one is a named constant (usable in array sizes, case labels, #embed
   limits...), like an enumerator; others are plain const objects.  Returns 1 if it was handled. */
static int constexpr_decl(Type t, const char *name) {
    if (!spec_constexpr) return 0;
    if (current_token.type != TOKEN_ASSIGN) error("constexpr '%s' needs an initializer", name);
    if (!is_integer(t) || t.nd) return 0;
    next_token();
    long long v = const_int_expr();
    EnumConst *old = find_enum_const(name);
    if (old && old->depth == scope_depth) error("redefinition of '%s'", name);
    if (nenum_consts >= 1024) error("too many constants");
    if (trunc_const(v, t) != v) error("constexpr '%s': %lld does not fit its type", name, v);
    strcpy(enum_consts[nenum_consts].name, name);
    enum_consts[nenum_consts].depth = scope_depth;
    enum_consts[nenum_consts].val = v;
    enum_consts[nenum_consts++].type = unqual(t);
    return 1;
}

static void parse_typedef_names(Type base);
static void parse_typedef(void) {   /* current token: typedef */
    next_token();
    parse_typedef_names(parse_type_spec());
}

/* the declarators of a typedef: `typedef` may also come among the specifiers (const char typedef *s;) */
static void parse_typedef_names(Type base) {
    for (;;) {
        char name[32];
        Type t = parse_declarator(base, name, 0);
        Typedef *old = find_typedef(name);
        if (old) {
            if (!same_type(old->ty, t)) error("conflicting typedef '%s'", name);
        } else {
            if (ntypedefs >= 256) error("too many typedefs");
            strcpy(typedefs[ntypedefs].name, name);
            typedefs[ntypedefs++].ty = t;
        }
        if (current_token.type != TOKEN_COMMA) break;
        next_token();
    }
    expect(TOKEN_SEMI);
}

/* ============================================================ parser: expressions */
static int literal_counter;   /* file-scope compound literals: __c2_litN */
static Val parse_expr(void);
static Val parse_assign(void);
static Val parse_cond(void);
static Val parse_unary(void);
static void parse_statement(void);

typedef struct Member Member;
static Member *find_member(Type st, const char *name, int *off);
static int count_initializer_at(Type elem, int at_init);
static void init_value(int base, int disp, Type t);
static Global *add_global(const char *name, Type t);
static void ensure_global_storage(Global *g);
static Func *declare_function(Type ft, const char *name);

static Val var_val(Var *v) {
    if (v->sra) { Val r; memset(&r, 0, sizeof r); r.k = V_SRA; r.base = (int)(v - vars); r.ty = v->ty; return r; }
    if (v->global >= 0) {                              /* static local */
        int a = new_temp(RK_I);
        EMIT(MOV, R(a), REF(globals[v->global].name));
        return lv_mem(a, 0, v->ty);
    }
    if (v->is_mem) return lv_mem(v->reg, 0, v->ty);
    Val r;
    memset(&r, 0, sizeof r);
    r.k = V_REG;
    r.op = R(v->reg);
    r.ty = v->ty;
    return r;
}

/* type of an expression without keeping its code */
static Type dry_run_type(Val (*parse)(void), int decay) {
    LexState ls = save_lex();
    EmitMark em = emit_mark();
    no_uninit_warn++;
    Val v = parse();
    no_uninit_warn--;
    Type t = decay ? rvalue(v).ty : v.ty;
    emit_rollback(em);
    restore_lex(ls);
    return t;
}

/* one call: g = signature, callee = REF(name) or a register holding a function address,
   c_abi = narrow int results come back as C's 32-bit int (imports, calls through pointers) */
static Val gen_call_sig(Sig *g, const char *fname, Opnd callee, const char *pname, int c_abi, int is_import, Val *args, int n) {
    if (!g->unprototyped && (n < g->np || (!g->vararg && n > g->np)))
        error("function '%s' expects %d argument%s, got %d", fname, g->np, g->np == 1 ? "" : "s", n);
    for (int i = 0; i < n; i++) {
        if (i < g->np && !g->unprototyped) {
            if (is_struct_val(g->pt[i])) {
                Val v = convert(args[i], g->pt[i]);
                MemT bt = abi_arg_type(g->pt[i]);
                if (bt == MT_I64) {                    /* by reference: a pointer to a copy */
                    int slot = new_slot("_arg", size_of(g->pt[i]));
                    copy_mem(slot, 0, v.base, v.disp, size_of(g->pt[i]));
                    args[i] = rv_reg(slot, ptr_to(g->pt[i]));
                } else {                               /* block: MIR copies it as the ABI says */
                    args[i] = rv_op(MEM(bt, size_of(g->pt[i]), addr_reg(v.base, v.disp)), g->pt[i]);
                }
            } else {
                args[i] = convert(args[i], unconst(g->pt[i]));
            }
        } else {                                       /* variadic / unprototyped: default promotions */
            Val v = rvalue(args[i]);
            if (is_struct_val(v.ty)) {                 /* our own convention: a pointer to a copy */
                if (is_import) error("struct passed to external variadic function '%s'", fname);
                int slot = new_slot("_varg", size_of(v.ty));
                copy_mem(slot, 0, v.base, v.disp, size_of(v.ty));
                args[i] = rv_reg(slot, ptr_to(v.ty));
                continue;
            }
            if (is_float(v.ty)) v = convert(v, mk_type(TY_DOUBLE));
            else if (is_integer(v.ty)) v = convert(v, promote(v.ty));
            args[i] = rv_reg(to_reg(v), v.ty);
        }
    }
    Opnd ops[MAX_PARAMS + 40];
    int k = 0;
    ops[k++] = REF(pname);                             /* call <proto>, <func>, [<result>,] <args...> */
    ops[k++] = callee;
    Val res = rv_op(IMM(0), g->ret);
    MemT rq[2];
    int nrr = 0, rr[2] = {0, 0}, rslot = -1;
    if (sig_returns_value(g)) {
        int t = new_temp(reg_kind(g->ret));
        ops[k++] = R(t);
        res = rv_reg(t, g->ret);
    } else if (is_struct_val(g->ret)) {
        rslot = new_slot("_res", size_of(g->ret));
        res = lv_mem(rslot, 0, g->ret);
        nrr = abi_ret_regs(g->ret, rq);
        for (int i = 0; i < nrr; i++) { rr[i] = new_temp(mt_rk(rq[i])); ops[k++] = R(rr[i]); }
        if (nrr == 0) {                                /* hidden result buffer; rax = its address */
            ops[k++] = R(new_temp(RK_I));
            ops[k++] = MEM(MT_RBLK, size_of(g->ret), rslot);
        }
    }
    if (n + k > MAX_PARAMS + 40) error("too many arguments");
    for (int i = 0; i < n; i++) ops[k++] = args[i].op;
    emit_ops(0, I_CALL, ops, k);   /* (MIR inlines direct calls of small functions by itself) */
    for (int i = 0; i < nrr; i++) {                    /* struct returned in registers */
        Opnd st[2] = {MEM(rq[i], 8 * i, rslot), R(rr[i])};
        emit_ops(0, mov_insn(mt_rk(rq[i])), st, 2);
    }
    if (c_abi && is_integer(g->ret) && int_width(g->ret) < 64)   /* C's 32-bit int result */
        res = normalize(res, g->ret);
    if (is_int32(g->ret)) res.dirty = 1;
    return res;
}

static Val gen_call(Func *f, Val *args, int n) {
    ensure_func_items(f);
    char pname[40];
    snprintf(pname, sizeof pname, "proto_%s", f->name);
    Sig g = sig_of_func(f);
    g.unprototyped = f->unprototyped && !f->defined;
    return gen_call_sig(&g, f->name, REF(f->name), pname, f->is_import, f->is_import, args, n);
}

static int parse_args(Val *args) {   /* current token: '(' */
    next_token();
    int n = 0;
    if (current_token.type != TOKEN_RPAREN) {
        for (;;) {
            if (n >= MAX_PARAMS + 32) error("too many arguments");
            args[n++] = rvalue(parse_assign());
            if (current_token.type != TOKEN_COMMA) break;
            next_token();
        }
    }
    expect(TOKEN_RPAREN);
    return n;
}

/* signature index of a named function, for taking its address */
static int func_sig_index(Func *f) {
    if (f->sig < 0) {
        f->sig = new_sig();
        Sig g = sig_of_func(f);
        g.unprototyped = f->unprototyped;
        sigs[f->sig] = g;
    }
    return f->sig;
}

static Val call_builtin(const char *name, Val *args, int n) {
    Func *f = find_func(name);
    if (!f) error("internal: builtin '%s' missing", name);
    return gen_call(f, args, n);
}

/* what a .c2 call needs to know about its callee: a named function or a function pointer's type */
typedef struct {
    const char *name;
    int np, vararg, unprototyped;
    Type *pt;
    char (*pn)[32];      /* names used to match .name = value */
    char **pdef;         /* default values (source text), or NULL */
    int *plen;           /* exact array lengths, or NULL */
} Callee;

static Callee callee_of_func(Func *f) {
    Callee c = {f->name, f->np, f->vararg, f->unprototyped, f->pt, f->un ? f->un : f->pn, f->pdef, f->plen};
    return c;
}

static Callee callee_of_sig(Sig *g, const char *name) {
    Callee c = {name, g->np, g->vararg, g->unprototyped, g->pt, g->pn, g->pdef, g->plen};
    return c;
}

/* is parameter i named name?  (define_function may have renamed it to name_argN for MIR) */
static int param_named(Callee *f, int i, const char *name) {
    const char *p = f->pn[i];
    size_t n = strlen(name);
    return !strcmp(p, name) || (!strncmp(p, name, n) && !strncmp(p + n, "_arg", 4));
}

/* argument i of a .c2 call; an `int a[3]` parameter wants an array of exactly 3 elements */
static Val c2_arg(Callee *f, int i) {
    Val raw = parse_assign();
    if (f->plen && i < f->np && f->plen[i] > 0) {
        int want = f->plen[i];
        if (!is_array(raw.ty) || arr_len(raw.ty) < 0)
            error("argument %d of '%s' must be an array of %d elements, not '%s'", i + 1, f->name, want, type_name(raw.ty));
        if (arr_len(raw.ty) != want)
            error("argument %d of '%s' must be an array of %d elements, not %d", i + 1, f->name, want, arr_len(raw.ty));
    }
    return rvalue(raw);
}

/* .c2 call arguments: positional, then named (.b = 2); missing ones take their default value,
   evaluated at the call (its source text is parsed here), where the other parameters are visible
   by name with the values this call gives them (`int d = w * 2`).  first: the receiver of x.f(...) */
static int parse_args_c2(Callee *f, Val *args, Val *first) {   /* current token: '(' */
    Val byp[MAX_PARAMS];
    int set[MAX_PARAMS] = {0}, pos = 0, extra = 0, named = 0;
    Val extras[32];
    if (first) { byp[0] = *first; set[0] = 1; pos = 1; }
    next_token();
    while (current_token.type != TOKEN_RPAREN) {
        if (current_token.type == TOKEN_DOT) {         /* .name = value */
            next_token();
            if (current_token.type != TOKEN_IDENT) error("parameter name expected after '.'");
            char pname[64];
            snprintf(pname, sizeof pname, "%s", current_token.text);
            next_token();
            expect(TOKEN_ASSIGN);
            int i = 0;
            while (i < f->np && !param_named(f, i, pname)) i++;
            if (i == f->np) error("'%s' has no parameter '%s'", f->name, pname);
            if (set[i]) error("parameter '%s' of '%s' given twice", pname, f->name);
            byp[i] = c2_arg(f, i);
            set[i] = 1;
            named = 1;
        } else {
            if (named) error("positional argument after a named one in the call to '%s'", f->name);
            Val v = c2_arg(f, pos);
            if (pos < f->np) { byp[pos] = v; set[pos] = 1; }
            else if (f->vararg || f->unprototyped) { if (extra >= 32) error("too many arguments"); extras[extra++] = v; }
            else error("'%s' expects %d argument%s, got more", f->name, f->np, f->np == 1 ? "" : "s");
            pos++;
        }
        if (current_token.type != TOKEN_COMMA) break;
        next_token();
    }
    expect(TOKEN_RPAREN);
    for (int i = 0; i < f->np; i++) {
        if (set[i]) continue;
        if (!f->pdef || !f->pdef[i])
            error("missing argument for parameter %d ('%s') of '%s'", i + 1, f->pn[i], f->name);
        enter_scope();                                 /* the given parameters, by name */
        for (int k = 0; k < f->np; k++) {
            if (!set[k] || !f->pn[k][0] || nvars >= 2048) continue;
            Var *v = &vars[nvars++];
            memset(v, 0, sizeof *v);
            snprintf(v->name, sizeof v->name, "%s", f->pn[k]);
            v->ty = byp[k].ty;
            v->depth = scope_depth;
            v->global = -1;
            if (is_struct_val(v->ty)) { v->is_mem = 1; v->reg = to_reg(address_of(byp[k])); }
            else { v->reg = new_temp(reg_kind(v->ty)); EMIT_OP(mov_insn(reg_kind(v->ty)), R(v->reg), byp[k].op); }
        }
        LexState ls = save_lex();                      /* parse the default value's text here */
        src = f->pdef[i];
        next_token();
        byp[i] = rvalue(parse_assign());
        if (current_token.type != TOKEN_EOF) error("bad default value for parameter '%s' of '%s'", f->pn[i], f->name);
        restore_lex(ls);
        leave_scope();
        set[i] = 1;
    }
    int n = 0;
    for (int i = 0; i < f->np; i++) args[n++] = byp[i];
    for (int i = 0; i < extra; i++) args[n++] = extras[i];
    return n;
}

static Val parse_call(Func *f) {
    Val args[MAX_PARAMS + 32];
    Callee ce = callee_of_func(f);
    int n = c2_ext && !f->unprototyped ? parse_args_c2(&ce, args, NULL) : parse_args(args);
    return gen_call(f, args, n);
}

/* address of a va_list object: va_list is an array (SysV) or a pointer to the struct (as a
   parameter), or char* (Windows, Apple arm64), whose own address is wanted */
static int va_list_addr(Val v) {
    if (is_array(v.ty) || (is_pointer(v.ty) && is_struct_val(elem_of(v.ty)))) return to_reg(rvalue(v));
    return to_reg(address_of(v));
}

/* va_start(ap, last) / va_arg(ap, T) / va_end(ap) / va_copy(dst, src), see <stdarg.h> */
static Val parse_va_builtin(const char *name) {
    next_token();
    Val res = rv_op(IMM(0), mk_type(TY_VOID));
    if (!strcmp(name, "__builtin_va_start")) {
        if (!cur_func || !cur_func->vararg) error("va_start used in a function without '...'");
        int a = va_list_addr(parse_assign());
        if (current_token.type == TOKEN_COMMA) { next_token(); parse_assign(); }   /* the last named parameter */
        EMIT(VA_START, R(a));
    } else if (!strcmp(name, "__builtin_va_arg")) {
        int a = va_list_addr(parse_assign());
        expect(TOKEN_COMMA);
        Type t = parse_type_name();
        if (is_struct_val(t)) {                        /* passed as a pointer to the caller's copy */
            int r = new_temp(RK_I);
            EMIT(VA_ARG, R(r), R(a), MEM_ANY(MT_I64));
            int p = new_temp(RK_I);
            EMIT(MOV, R(p), MEM(MT_I64, 0, r));
            int slot = new_slot("_vstruct", size_of(t));
            copy_mem(slot, 0, p, 0, size_of(t));
            expect(TOKEN_RPAREN);
            return lv_mem(slot, 0, t);
        }
        if (is_void(t) || is_array(t) || is_func(t)) error("invalid type '%s' for va_arg", type_name(t));
        Type slot = is_float(t) ? mk_type(TY_DOUBLE) : is_integer(t) ? promote(t) : t;   /* promoted */
        int r = new_temp(RK_I);
        EMIT(VA_ARG, R(r), R(a), MEM_ANY(mem_type(slot)));
        res = convert(rvalue(lv_mem(r, 0, slot)), t);
    } else if (!strcmp(name, "__builtin_va_end")) {
        int a = va_list_addr(parse_assign());
        EMIT(VA_END, R(a));
    } else if (!strcmp(name, "__builtin_va_copy")) {
        Val d = parse_assign();
        expect(TOKEN_COMMA);
        Val s = parse_assign();
        Type vt = d.ty;
        int size = is_array(vt) ? size_of(vt) : is_pointer(vt) && is_struct_val(elem_of(vt)) ? size_of(elem_of(vt)) : 8;
        int da = va_list_addr(d), sa = va_list_addr(s);
        copy_mem(da, 0, sa, 0, size);
    } else {
        error("unknown builtin '%s'", name);
    }
    expect(TOKEN_RPAREN);
    return res;
}

/* _Generic(controlling-expr, type: expr, ..., default: expr): the controlling expression and the
   associations that aren't chosen are parsed without keeping their code */
static int same_type_q(Type a, Type b) {   /* same type with the same qualifiers (const char * != char *) */
    if (!same_type(a, b) || a.bcq != b.bcq) return 0;
    for (int i = 0; i < a.nd; i++) if (a.d[i].cq != b.d[i].cq) return 0;
    return 1;
}
static Type skip_assign_expr(int decay) {   /* parse an expression, keep only its type */
    EmitMark em = emit_mark();
    Val v = parse_assign();
    Type t = decay ? rvalue(v).ty : v.ty;
    emit_rollback(em);
    return t;
}
static Val parse_generic(void) {
    next_token();
    expect(TOKEN_LPAREN);
    Type ct = unconst(skip_assign_expr(1));
    expect(TOKEN_COMMA);
    LexState start = save_lex();
    int chosen = -1, dflt = -1;
    for (int i = 0;; i++) {                            /* pass 1: which association? */
        if (current_token.type == TOKEN_DEFAULT) { next_token(); dflt = i; }
        else if (same_type_q(unconst(parse_type_name()), ct) && chosen < 0) chosen = i;
        expect(TOKEN_COLON);
        skip_assign_expr(0);
        if (current_token.type != TOKEN_COMMA) break;
        next_token();
    }
    if (chosen < 0) chosen = dflt;
    if (chosen < 0) error("_Generic: no association for type '%s'", type_name(ct));
    restore_lex(start);
    Val r;
    memset(&r, 0, sizeof r);
    for (int i = 0;; i++) {                            /* pass 2: generate the chosen one */
        if (current_token.type == TOKEN_DEFAULT) next_token(); else parse_type_name();
        expect(TOKEN_COLON);
        if (i == chosen) r = parse_assign(); else skip_assign_expr(0);
        if (current_token.type != TOKEN_COMMA) break;
        next_token();
    }
    expect(TOKEN_RPAREN);
    return r;
}

static Val parse_primary(void) {
    Token t = current_token;
    switch (t.type) {
    case TOKEN_GENERIC: return parse_generic();
    case TOKEN_NUM: {
        next_token();
        Type ty = t.num_flags & NUM_LONG ? long_type() : int_type();
        ty.uns = (t.num_flags & NUM_UNSIGNED) != 0;
        if (t.num_flags & NUM_BOOL) { ty = mk_type(TY_CHAR); ty.uns = 2; }
        if (t.num_flags & NUM_UCHAR) { ty = mk_type(TY_CHAR); ty.uns = 1; }
        return rv_op(IMM(trunc_const(t.val, ty)), ty);
    }
    case TOKEN_FLOAT_NUM:
        next_token();
        if (t.num_flags & NUM_FLOAT32) return rv_op(FIMM(t.float_val), mk_type(TY_FLOAT));
        return rv_op(DIMM(t.float_val), mk_type(TY_DOUBLE));
    case TOKEN_NULL: next_token(); return rv_op(IMM(0), ptr_to(mk_type(TY_VOID)));
    case TOKEN_STR: {
        const char *item = be_string(t.str, t.str_len + (t.wide ? WCHAR_SIZE - 1 : 0));   /* (+ the terminator's rest) */
        int r = new_temp(RK_I);
        EMIT(MOV, R(r), REF(item));
        next_token();
        if (t.wide) return lv_mem(r, 0, array_of(wchar_type(), t.str_len / WCHAR_SIZE + 1));   /* wchar_t[N] */
        return lv_mem(r, 0, array_of(mk_type(TY_CHAR), t.str_len + 1));   /* char[N]: decays to char* */
    }
    case TOKEN_LPAREN: {
        next_token();
        Val v = parse_expr();
        expect(TOKEN_RPAREN);
        return v;
    }
    case TOKEN_IDENT: {
        next_token();
        Var *v = find_var(t.text);
        if (v) {
            if (v->uninit && !no_uninit_warn) {        /* .c: read before any assignment? */
                TokenType nt = current_token.type;
                if (nt == TOKEN_ASSIGN || addr_of_pending) v->uninit = 0;
                else {
                    diag("%s:%d: warning: '%s' is used uninitialized\n", cur_file, cur_line, v->name);
                    v->uninit = 0;
                }
            }
            addr_of_pending = 0;
            return var_val(v);
        }
        if (current_token.type == TOKEN_LPAREN && strncmp(t.text, "__builtin_va_", 13) == 0)
            return parse_va_builtin(t.text);
        if (current_token.type == TOKEN_LPAREN && strcmp(t.text, "offsetof") == 0 && !find_func(t.text)) {
            next_token();                              /* offsetof(type, member) */
            Type st = parse_type_name();
            expect(TOKEN_COMMA);
            int off = 0;
            for (;;) {                                 /* member designator: a.b[2].c */
                if (current_token.type != TOKEN_IDENT) error("member name expected");
                int moff;
                Member *m = find_member(st, current_token.text, &moff);
                off += moff;
                st = m->ty;
                next_token();
                while (current_token.type == TOKEN_LBRACKET) {
                    next_token();
                    if (!is_array(st)) error("offsetof: subscript of a non-array");
                    st = elem_of(st);
                    off += (int)const_int_expr() * size_of(st);
                    expect(TOKEN_RBRACKET);
                }
                if (current_token.type != TOKEN_DOT) break;
                next_token();
            }
            expect(TOKEN_RPAREN);
            Type r = long_type();
            r.uns = 1;
            return rv_op(IMM(off), r);
        }
        if (current_token.type == TOKEN_LPAREN && !find_global(t.text)) {
            Func *f = find_func(t.text);
            if (!f) {                                  /* C89 implicit declaration: int name() */
                diag("%s:%d: warning: implicit declaration of function '%s'\n", cur_file, cur_line, t.text);
                int si = new_sig();
                sigs[si].unprototyped = 1;
                sigs[si].ret = int_type();
                f = declare_function(func_of(int_type(), si), t.text);
            }
            return parse_call(f);
        }
        EnumConst *e = find_enum_const(t.text);
        if (e) return rv_op(IMM(e->val), e->type);
        if (strcmp(t.text, "__func__") == 0 && cur_func) {   /* static const char __func__[] = "name" */
            const char *item = be_string(cur_func->name, (int)strlen(cur_func->name));
            int r = new_temp(RK_I);
            EMIT(MOV, R(r), REF(item));
            return lv_mem(r, 0, array_of(add_const(mk_type(TY_CHAR)), (int)strlen(cur_func->name) + 1));
        }
        Global *g = find_global(t.text);
        if (g) {   /* a MIR data item name is its address */
            ensure_global_storage(g);
            int a = new_temp(RK_I);
            EMIT(MOV, R(a), REF(g->name));
            return lv_mem(a, 0, g->ty);
        }
        Func *f = find_func(t.text);
        if (f) {   /* function designator: its address, typed as a function (decays to a pointer) */
            ensure_func_items(f);
            int a = new_temp(RK_I);
            EMIT(MOV, R(a), REF(f->name));
            return rv_op(R(a), func_of(f->ret, func_sig_index(f)));
        }
        error("undeclared identifier '%s'", t.text);
        break;
    }
    default: error("expression expected");
    }
    return rv_op(IMM(0), int_type());
}

/* member lookup, searching anonymous struct/union members too; *off gets the accumulated offset */
static Member *find_member_rec(int sidx, const char *name, int *off) {
    Struct *s = &structs[sidx];
    for (int i = 0; i < s->n; i++) {
        if (s->m[i].name[0] && strcmp(s->m[i].name, name) == 0) { *off += s->m[i].off; return &s->m[i]; }
        if (!s->m[i].name[0]) {
            int sub = *off + s->m[i].off;
            Member *m = find_member_rec(s->m[i].ty.sidx, name, &sub);
            if (m) { *off = sub; return m; }
        }
    }
    return NULL;
}

static Member *find_member(Type st, const char *name, int *off) {
    if (!is_struct_val(st)) error("member access on '%s', which is not a struct", type_name(st));
    Struct *s = &structs[st.sidx];
    if (!s->complete) error("%s %s is incomplete", s->is_union ? "union" : "struct", s->name);
    *off = 0;
    Member *m = find_member_rec(st.sidx, name, off);
    if (!m) error("%s %s has no member '%s'", s->is_union ? "union" : "struct", s->name, name);
    return m;
}

/* ++x / --x / x++ / x-- */
static Val inc_dec(Val v, TokenType op, int post) {
    if (v.k == V_RVAL || is_array(v.ty)) error("++/-- needs an lvalue");
    if (!is_arith(v.ty) && !is_pointer(v.ty)) error("++/-- on '%s'", type_name(v.ty));
    Val cur = rvalue(v);
    if (post) {                                        /* keep the old value: v's register will change */
        int t = new_temp(reg_kind(cur.ty));
        EMIT_OP(mov_insn(reg_kind(cur.ty)), R(t), cur.op);
        int d = cur.dirty;
        cur = rv_reg(t, cur.ty);
        cur.dirty = d;
    }
    Val nv = store(v, arith(op == TOKEN_INC ? TOKEN_PLUS : TOKEN_MINUS, cur, rv_op(IMM(1), int_type())));
    return post ? cur : nv;
}

/* does x.name / x->name name a member? (for .c2 method calls) */
static int has_member(Val v, int arrow, const char *name) {
    Type st = v.ty;
    if (arrow || (is_pointer(st) && !is_array(st))) {
        if (!is_pointer(st) || is_array(st)) return 0;
        st = elem_of(st);
    }
    if (!is_struct_val(st) || !structs[st.sidx].complete) return 0;
    int off = 0;
    return find_member_rec(st.sidx, name, &off) != NULL;
}

/* .c2 uniform call syntax: x.f(args) calls f(x, args).  When f's first parameter is a pointer and
   x is not (a struct, say), &x is passed, so methods can change the object; x->f(...) passes x. */
static Val method_call(Val v, int arrow, const char *fname) {
    Func *f = find_func(fname);
    if (!f) error("no member or function '%s' for %s.%s(...)", fname, type_name(v.ty), fname);
    if (f->unprototyped || f->np == 0) error("'%s' takes no parameters: it cannot be called as x.%s()", fname, fname);
    Type p0 = f->pt[0];
    Val self;
    if (!arrow && is_pointer(p0) && !is_pointer(v.ty) && !is_array(v.ty) && !is_func(v.ty)) self = address_of(v);
    else self = rvalue(v);
    if (arrow && !is_pointer(self.ty)) error("'->' on '%s', which is not a pointer", type_name(self.ty));
    Val args[MAX_PARAMS + 32];
    Callee ce = callee_of_func(f);
    int n = parse_args_c2(&ce, args, &self);
    return gen_call(f, args, n);
}

static Val parse_postfix(void) {
    Val v = parse_primary();
    for (;;) {
        if (current_token.type == TOKEN_LBRACKET) {
            next_token();
            Val base = rvalue(v);
            Val idx = rvalue(parse_expr());
            expect(TOKEN_RBRACKET);
            if (is_integer(base.ty) && is_pointer(idx.ty)) { Val tmp = base; base = idx; idx = tmp; }   /* i[p] */
            if (!is_pointer(base.ty)) error("subscript of '%s'", type_name(base.ty));
            if (!is_integer(idx.ty)) error("array index is not an integer");
            idx = convert(idx, long_type());
            Type et = elem_of(base.ty);
            int esz = is_void(et) ? 1 : size_of(et);
            int b = to_reg(base);
            if (idx.op.k == OK_INT) {
                v = lv_mem(b, (int)(idx.op.i * esz), et);
            } else if (!is_aggregate(et) && (esz == 1 || esz == 2 || esz == 4 || esz == 8)) {
                v = lv_mem(b, 0, et);                  /* scalar element: indexed memory operand */
                v.ix = to_reg(idx) + 1;
                v.sc = esz;
            } else {
                int off = to_reg(idx), a = new_temp(RK_I);
                if (esz != 1) { off = new_temp(RK_I); EMIT(MUL, R(off), idx.op, IMM(esz)); }
                EMIT(ADD, R(a), R(b), R(off));
                v = lv_mem(a, 0, et);
            }
        } else if (current_token.type == TOKEN_DOT || current_token.type == TOKEN_ARROW) {
            int arrow = current_token.type == TOKEN_ARROW;
            next_token();
            if (current_token.type != TOKEN_IDENT) error("member name expected");
            char mname[64];
            strcpy(mname, current_token.text);
            next_token();
            if (c2_ext && current_token.type == TOKEN_LPAREN && !has_member(v, arrow, mname)) {
                v = method_call(v, arrow, mname);      /* .c2: x.f(a) is f(x, a) or f(&x, a) */
                continue;
            }
            if (c2_ext && !arrow && v.k != V_SRA && is_pointer(v.ty) && is_struct_val(elem_of(v.ty)))
                arrow = 1;                             /* .c2: p.x is p->x */
            Type st;
            int base, disp;
            if (arrow) {
                Val p = rvalue(v);
                if (!is_pointer(p.ty)) error("'->' on '%s', which is not a pointer", type_name(p.ty));
                st = elem_of(p.ty);
                base = to_reg(p);
                disp = 0;
            } else if (v.k == V_SRA) {                /* member of a struct kept in registers */
                Var *sv = &vars[v.base];
                Struct *sd = &structs[v.ty.sidx];
                int mi = -1;
                for (int i = 0; i < sd->n; i++) if (strcmp(sd->m[i].name, mname) == 0) mi = i;
                if (mi < 0) error("%s has no member '%s'", type_name(v.ty), mname);
                Type mt = sd->m[mi].ty;
                if (is_const_obj(v.ty)) mt = add_const(mt);
                memset(&v, 0, sizeof v);
                v.k = V_REG;
                v.op = R(sv->sra_reg[mi]);
                v.ty = mt;
                continue;
            } else {
                if (v.k != V_MEM || !is_struct_val(v.ty)) error("'.' on '%s', which is not a struct", type_name(v.ty));
                st = v.ty;
                base = v.base;
                disp = v.disp;
            }
            int moff;
            Member *m = find_member(st, mname, &moff);
            Type mt = m->ty;
            if (is_const_obj(st)) mt = add_const(mt);   /* members of a const struct are const */
            v = lv_mem(base, disp + moff, mt);
        } else if (current_token.type == TOKEN_LPAREN) {  /* call through a function pointer */
            Val fv = rvalue(v);
            if (!is_func_ptr(fv.ty)) error("called object of type '%s' is not a function", type_name(fv.ty));
            int si = func_sig(elem_of(fv.ty));
            Val args[MAX_PARAMS + 32];
            Callee ce = callee_of_sig(&sigs[si], "(function pointer)");
            int n = c2_ext && !sigs[si].unprototyped ? parse_args_c2(&ce, args, NULL) : parse_args(args);
            fv = rvalue(fv);
            Opnd callee = R(to_reg(fv));
            v = gen_call_sig(&sigs[si], "(function pointer)", callee, sig_proto_name(si), 1, 0, args, n);
        } else if (current_token.type == TOKEN_INC || current_token.type == TOKEN_DEC) {
            TokenType op = current_token.type;
            next_token();
            v = inc_dec(v, op, 1);
        } else {
            return v;
        }
    }
}

static Val parse_unary(void) {
    switch (current_token.type) {
    case TOKEN_INC:
    case TOKEN_DEC: {
        TokenType op = current_token.type;
        next_token();
        return inc_dec(parse_unary(), op, 0);
    }
    case TOKEN_AMPERSAND: {
        next_token();
        addr_of_pending = 1;                           /* &x sets x, as far as the warning goes */
        Val v = parse_unary();
        addr_of_pending = 0;
        return address_of(v);
    }
    case TOKEN_ASTERISK: {
        next_token();
        Val p = rvalue(parse_unary());
        if (!is_pointer(p.ty)) error("dereference of '%s', which is not a pointer", type_name(p.ty));
        if (is_void(elem_of(p.ty))) error("dereference of void*");
        if (is_func(elem_of(p.ty))) return rv_op(p.op, elem_of(p.ty));   /* *fp is the function again */
        return lv_mem(to_reg(p), 0, elem_of(p.ty));
    }
    case TOKEN_PLUS: {
        next_token();
        Val v = rvalue(parse_unary());
        if (!is_arith(v.ty)) error("unary + on '%s'", type_name(v.ty));
        return convert(v, promote(v.ty));
    }
    case TOKEN_MINUS: {
        next_token();
        Val v = rvalue(parse_unary());
        if (!is_arith(v.ty)) error("unary - on '%s'", type_name(v.ty));
        Type t = promote(v.ty);
        v = convert(v, t);
        if (v.op.k == OK_INT) return rv_op(IMM(trunc_const((long long)(0ull - (unsigned long long)v.op.i), t)), t);
        if (v.op.k == OK_FLT) return rv_op(FIMM(-v.op.d), t);
        if (v.op.k == OK_DBL) return rv_op(DIMM(-v.op.d), t);
        RegKind rk = reg_kind(t);
        int r = new_temp(rk);
        EMIT_OP(rk == RK_F ? I_FNEG : rk == RK_D ? I_DNEG : is_int32(t) ? I_NEGS : I_NEG, R(r), v.op);
        return normalize(rv_reg(r, t), t);
    }
    case TOKEN_TILDE: {
        next_token();
        Val v = rvalue(parse_unary());
        if (!is_integer(v.ty)) error("~ on '%s'", type_name(v.ty));
        Type t = promote(v.ty);
        v = convert(v, t);
        if (v.op.k == OK_INT) return rv_op(IMM(trunc_const(~v.op.i, t)), t);
        int r = new_temp(RK_I);
        if (is_int32(t)) EMIT(XORS, R(r), v.op, IMM(-1));
        else EMIT(XOR, R(r), v.op, IMM(-1));
        return normalize(rv_reg(r, t), t);
    }
    case TOKEN_NOT: {
        next_token();
        Val v = rvalue(parse_unary());
        if (is_struct_val(v.ty) || is_void(v.ty)) error("! on '%s'", type_name(v.ty));
        int t = new_temp(RK_I);
        if (is_float(v.ty)) EMIT(FEQ, R(t), v.op, FIMM(0.0));
        else if (is_double(v.ty)) EMIT(DEQ, R(t), v.op, DIMM(0.0));
        else if (is_int32(v.ty)) EMIT(EQS, R(t), v.op, IMM(0));
        else EMIT(EQ, R(t), v.op, IMM(0));
        return rv_reg(t, int_type());
    }
    case TOKEN_ALIGNOF: {
        next_token();
        expect(TOKEN_LPAREN);
        Type t = parse_type_name();
        expect(TOKEN_RPAREN);
        Type r = long_type();
        r.uns = 1;
        return rv_op(IMM(align_of(t)), r);
    }
    case TOKEN_SIZEOF: {
        next_token();
        Type t;
        if (current_token.type == TOKEN_LPAREN && is_type_start(peek_token())) {
            next_token();
            t = parse_type_name();
            expect(TOKEN_RPAREN);
        } else {
            EmitMark em = emit_mark();                 /* sizeof doesn't evaluate its operand */
            no_uninit_warn++;
            t = parse_unary().ty;                      /* not decayed: arrays keep their size */
            no_uninit_warn--;
            emit_rollback(em);
        }
        Type r = long_type();
        r.uns = 1;
        if (is_vla(t)) {                               /* computed at run time */
            int b = new_temp(RK_I);
            EMIT(MUL, R(b), R(vla_len_reg[-2 - arr_len(t)]), IMM(size_of(elem_of(t))));
            return rv_reg(b, r);
        }
        return rv_op(IMM(size_of(t)), r);
    }
    case TOKEN_LPAREN:
        if (is_type_start(peek_token())) {             /* cast */
            next_token();
            Type t = parse_type_name();
            expect(TOKEN_RPAREN);
            if (current_token.type == TOKEN_LBRACE) {   /* compound literal (T){...} */
                if (is_array(t) && arr_len(t) < 0) t = set_arr_len(t, count_initializer_at(elem_of(t), 1));
                int size = size_of(t), base;
                if (!cur_func || strncmp(cur_func->name, "__c2_ginit", 10) == 0) {
                    char name[40];                     /* file scope: static storage */
                    snprintf(name, sizeof name, "__c2_lit%d", literal_counter++);
                    Global *g = add_global(name, t);
                    be_bss(g->name, size);
                    base = new_temp(RK_I);
                    EMIT(MOV, R(base), REF(g->name));
                } else {
                    base = new_slot("_lit", size);
                    zero_mem(base, 0, size);
                }
                init_value(base, 0, t);
                return lv_mem(base, 0, t);
            }
            return convert_ex(parse_unary(), t, 1);
        }
        return parse_postfix();
    default: return parse_postfix();
    }
}

static Val parse_binary(int level);

/* binary operators, by C precedence (higher level binds tighter) */
static Val parse_binary(int level) {
    if (level > 8) return parse_unary();
    Val v = parse_binary(level + 1);
    for (;;) {
        TokenType op = current_token.type;
        int ok;
        switch (level) {
        case 1: ok = op == TOKEN_PIPE; break;
        case 2: ok = op == TOKEN_CARET; break;
        case 3: ok = op == TOKEN_AMPERSAND; break;
        case 4: ok = op == TOKEN_EQ || op == TOKEN_NE; break;
        case 5: ok = op == TOKEN_LT || op == TOKEN_LE || op == TOKEN_GT || op == TOKEN_GE; break;
        case 6: ok = op == TOKEN_SHL || op == TOKEN_SHR; break;
        case 7: ok = op == TOKEN_PLUS || op == TOKEN_MINUS; break;
        default: ok = op == TOKEN_ASTERISK || op == TOKEN_SLASH || op == TOKEN_PERCENT; break;
        }
        if (!ok) break;
        next_token();
        Val rhs = parse_binary(level + 1);
        if (level == 4 || level == 5) v = compare(op, v, rhs);
        else if (level == 6) v = shift(op, v, rhs);
        else v = arith(op, v, rhs);
    }
    return v;
}

/* a && b  /  a || b with short-circuit: result is 0 or 1 */
static Val parse_logical(int is_or) {
    Val v = is_or ? parse_logical(0) : parse_binary(1);
    TokenType tok = is_or ? TOKEN_OROR : TOKEN_ANDAND;
    while (current_token.type == tok) {
        next_token();
        int res = new_temp(RK_I), done = new_label();
        int c = cond_reg(v);
        EMIT(MOV, R(res), IMM(is_or ? 1 : 0));
        if (is_or) EMIT(BT, LBL(done), R(c));
        else EMIT(BF, LBL(done), R(c));
        int c2 = cond_reg(is_or ? parse_logical(0) : parse_binary(1));
        EMIT(NE, R(res), R(c2), IMM(0));
        place_label(done);
        v = rv_reg(res, int_type());
    }
    return v;
}

/* cond ? a : b */
static Val parse_cond(void) {
    Val c = parse_logical(1);
    if (current_token.type != TOKEN_QUESTION) return c;
    next_token();
    int cr = cond_reg(c);
    /* the result type depends on both branches: look at them first without keeping the code */
    LexState ls = save_lex();
    EmitMark em = emit_mark();
    Type ta = rvalue(parse_expr()).ty;
    expect(TOKEN_COLON);
    Type tb = rvalue(parse_cond()).ty;
    emit_rollback(em);
    restore_lex(ls);

    Type rt;
    if (is_arith(ta) && is_arith(tb)) rt = common_type(ta, tb);
    else if (is_struct_val(ta) && is_struct_val(tb) && ta.sidx == tb.sidx) rt = ta;
    else if (is_void(ta) && is_void(tb)) rt = ta;
    else if (is_pointer(ta) && is_pointer(tb)) {
        rt = is_void(elem_of(tb)) ? tb : ta;
        if (is_const_obj(elem_of(ta)) || is_const_obj(elem_of(tb))) {   /* pointee keeps const */
            if (rt.nd >= 2) rt.d[rt.nd - 2].cq = 1;
            else rt.bcq = 1;
        }
    } else if (is_pointer(ta) && is_integer(tb)) rt = ta;
    else if (is_integer(ta) && is_pointer(tb)) rt = tb;
    else error("incompatible operand types '%s' and '%s' in ?:", type_name(ta), type_name(tb));

    int l_else = new_label(), l_end = new_label();
    int r = -1;
    if (is_struct_val(rt)) r = new_slot("_cond", size_of(rt));
    else if (!is_void(rt)) r = new_temp(reg_kind(rt));
    EMIT(BF, LBL(l_else), R(cr));
    for (int branch = 0; branch < 2; branch++) {
        Val v = branch == 0 ? parse_expr() : parse_cond();
        if (is_struct_val(rt)) {
            v = convert(v, rt);
            copy_mem(r, 0, v.base, v.disp, size_of(rt));
        } else if (!is_void(rt)) {
            v = convert(v, rt);
            EMIT_OP(mov_insn(reg_kind(rt)), R(r), v.op);
        } else {
            rvalue(v);
        }
        if (branch == 0) {
            EMIT(JMP, LBL(l_end));
            place_label(l_else);
            expect(TOKEN_COLON);
        }
    }
    place_label(l_end);
    if (is_struct_val(rt)) return lv_mem(r, 0, rt);
    if (is_void(rt)) return rv_op(IMM(0), rt);
    Val res = rv_reg(r, rt);
    res.dirty = is_int32(rt);
    return res;
}

static Val parse_assign(void) {
    Val lhs = parse_cond();
    TokenType op = current_token.type;
    if (op == TOKEN_ASSIGN) {
        next_token();
        Val rhs = parse_assign();                      /* right-associative: a = b = c */
        return store(lhs, rhs);
    }
    if (op >= TOKEN_PLUS_ASSIGN && op <= TOKEN_SHR_ASSIGN) {
        static const TokenType bin[] = {TOKEN_PLUS, TOKEN_MINUS, TOKEN_ASTERISK, TOKEN_SLASH, TOKEN_PERCENT,
                                        TOKEN_AMPERSAND, TOKEN_PIPE, TOKEN_CARET, TOKEN_SHL, TOKEN_SHR};
        TokenType b = bin[op - TOKEN_PLUS_ASSIGN];
        next_token();
        if (lhs.k == V_RVAL) error("compound assignment to something that is not an lvalue");
        if (is_const_obj(lhs.ty)) error("assignment to a const object of type '%s'", type_name(lhs.ty));
        Val rhs = parse_assign();
        Val r = (b == TOKEN_SHL || b == TOKEN_SHR) ? shift(b, lhs, rhs) : arith(b, lhs, rhs);   /* lhs address computed once */
        return store(lhs, r);
    }
    return lhs;
}

static Val parse_expr(void) {
    Val v = parse_assign();
    while (current_token.type == TOKEN_COMMA) {
        next_token();
        rvalue(v);
        v = parse_assign();
    }
    return v;
}

/* ============================================================ initializers */
static int scalar_leaves(Type t) {
    if (is_array(t)) return arr_len(t) * scalar_leaves(elem_of(t));
    if (is_struct_val(t)) {
        int n = 0;
        Struct *s = &structs[t.sidx];
        for (int i = 0; i < s->n && (!s->is_union || i < 1); i++) n += scalar_leaves(s->m[i].ty);
        return n ? n : 1;
    }
    return 1;
}

static Val zero_of(Type t) {
    if (is_float(t)) return rv_op(FIMM(0.0), t);
    if (is_double(t)) return rv_op(DIMM(0.0), t);
    return rv_op(IMM(0), t);
}

/* a string literal initializes arrays of char ("...") or of a wchar_t-sized integer (L"...") */
static int str_inits(Type et) {
    if (current_token.type != TOKEN_STR || !is_scalar_obj(et)) return 0;
    if (!current_token.wide) return et.base == TY_CHAR;
    return is_integer(et) && size_of(et) == WCHAR_SIZE;
}
static int str_units(void) { return current_token.str_len / (current_token.wide ? WCHAR_SIZE : 1); }

/* element count of `T x[] = <init>`; at_init: the current token is the initializer itself
   (otherwise it is the '=' before it).  Designators ([5] = ...) are taken into account. */
static int count_initializer_at(Type elem, int at_init) {
    LexState s = save_lex();
    if (!at_init) next_token();
    int n = 0;
    if (str_inits(elem)) {
        n = str_units() + 1;
    } else if (current_token.type == TOKEN_LBRACE) {
        next_token();
        int items = 0, pos = 0, maxpos = 0, designated = 0;
        int first_braced = current_token.type == TOKEN_LBRACE ||
                           (is_array(elem) && str_inits(elem_of(elem)));
        while (current_token.type != TOKEN_RBRACE && current_token.type != TOKEN_EOF) {
            if (current_token.type == TOKEN_LBRACKET) {       /* [index] = */
                next_token();
                pos = (int)const_int_expr();
                expect(TOKEN_RBRACKET);
                designated = 1;
            }
            for (int depth = 0; current_token.type != TOKEN_EOF; next_token()) {   /* skip the item */
                TokenType t = current_token.type;
                if (depth == 0 && (t == TOKEN_COMMA || t == TOKEN_RBRACE)) break;
                if (t == TOKEN_LBRACE || t == TOKEN_LPAREN || t == TOKEN_LBRACKET) depth++;
                if (t == TOKEN_RBRACE || t == TOKEN_RPAREN || t == TOKEN_RBRACKET) depth--;
            }
            items++;
            pos++;
            if (pos > maxpos) maxpos = pos;
            if (current_token.type == TOKEN_COMMA) next_token();
        }
        int leaves = scalar_leaves(elem);
        if (designated) n = maxpos;
        else n = (is_aggregate(elem) && !first_braced) ? (items + leaves - 1) / leaves : items;
    } else {
        if (current_token.type == TOKEN_STR)
            error(current_token.wide ? "L\"...\" initializes wchar_t arrays only" : "a string literal initializes char arrays only");
        error("array initializer must be a list");
    }
    restore_lex(s);
    if (n <= 0) error("zero-size array");
    return n;
}

static int count_initializer(Type elem) { return count_initializer_at(elem, 0); }

/* ---------------- initializers (C11 6.7.9)
 * A braced list is walked with a cursor: a stack of frames (aggregate, offset, current member).
 * Designators (.x, [n], .a.b[2]) reset the cursor; brace elision descends into sub-aggregates;
 * after each initializer the cursor advances, climbing out of finished sub-aggregates. */
typedef struct { Type t; int disp, idx; } IFrame;

static int is_union_t(Type t) { return is_struct_val(t) && structs[t.sidx].is_union; }
static int frame_count(Type t) { return is_array(t) ? arr_len(t) : structs[t.sidx].n; }
static Type frame_member(Type t, int i, int *off) {
    if (is_array(t)) { Type e = elem_of(t); *off = i * size_of(e); return e; }
    *off = structs[t.sidx].m[i].off;
    return structs[t.sidx].m[i].ty;
}

/* index path to member `name`, through anonymous struct/union members */
static int member_path(int sidx, const char *name, int *path, int depth) {
    Struct *s = &structs[sidx];
    if (depth >= 16) return 0;
    for (int i = 0; i < s->n; i++) {
        if (s->m[i].name[0] && strcmp(s->m[i].name, name) == 0) { path[depth] = i; return depth + 1; }
        if (!s->m[i].name[0]) {
            path[depth] = i;
            int n = member_path(s->m[i].ty.sidx, name, path, depth + 1);
            if (n) return n;
        }
    }
    return 0;
}

/* the next initializer is one expression for all of `t` (no brace elision into it) */
static int takes_whole(Type t) {
    if (is_array(t))
        return str_inits(elem_of(t));
    if (is_struct_val(t)) {
        Type et = dry_run_type(parse_assign, 1);
        return is_struct_val(et) && et.sidx == t.sidx;
    }
    return 1;
}

static void cursor_advance(IFrame *st, int *sp) {
    IFrame *f = &st[*sp - 1];
    f->idx = is_union_t(f->t) ? frame_count(f->t) : f->idx + 1;   /* a union takes one initializer */
    while (*sp > 1 && st[*sp - 1].idx >= frame_count(st[*sp - 1].t)) {
        (*sp)--;
        f = &st[*sp - 1];
        f->idx = is_union_t(f->t) ? frame_count(f->t) : f->idx + 1;
    }
}

static void init_value(int base, int disp, Type t);

/* current token: after '{'; consumes through '}' */
static void init_braced(int base, int disp, Type t) {
    IFrame st[64];
    int sp = 1;
    st[0].t = t;
    st[0].disp = disp;
    st[0].idx = 0;
    while (current_token.type != TOKEN_RBRACE) {
        if (current_token.type == TOKEN_DOT || current_token.type == TOKEN_LBRACKET) {
            sp = 1;                                    /* designators start from the braced object */
            for (;;) {
                IFrame *f = &st[sp - 1];
                if (current_token.type == TOKEN_LBRACKET) {
                    if (!is_array(f->t)) error("array designator for '%s', which is not an array", type_name(f->t));
                    next_token();
                    long long i = const_int_expr();
                    if (i < 0 || i >= arr_len(f->t)) error("array designator index %lld out of range", i);
                    expect(TOKEN_RBRACKET);
                    f->idx = (int)i;
                } else if (current_token.type == TOKEN_DOT) {
                    if (!is_struct_val(f->t)) error("member designator for '%s', which is not a struct", type_name(f->t));
                    next_token();
                    if (current_token.type != TOKEN_IDENT) error("member name expected after '.'");
                    int path[16], n = member_path(f->t.sidx, current_token.text, path, 0);
                    if (!n) error("%s %s has no member '%s'", structs[f->t.sidx].is_union ? "union" : "struct",
                                  structs[f->t.sidx].name, current_token.text);
                    for (int k = 0; k < n - 1; k++) {      /* through anonymous members */
                        int off;
                        f->idx = path[k];
                        Type mt = frame_member(f->t, path[k], &off);
                        if (sp >= 64) error("initializer nested too deeply");
                        st[sp].t = mt; st[sp].disp = f->disp + off; st[sp].idx = 0;
                        f = &st[sp++];
                    }
                    f->idx = path[n - 1];
                    next_token();
                } else {
                    break;
                }
                if (current_token.type == TOKEN_DOT || current_token.type == TOKEN_LBRACKET) {
                    int off;
                    Type mt = frame_member(f->t, f->idx, &off);
                    if (!is_aggregate(mt)) error("designator into '%s', which is not an aggregate", type_name(mt));
                    if (sp >= 64) error("initializer nested too deeply");
                    st[sp].t = mt; st[sp].disp = f->disp + off; st[sp].idx = 0;
                    sp++;
                }
            }
            expect(TOKEN_ASSIGN);
        } else if (sp == 1 && st[0].idx >= frame_count(st[0].t)) {
            error("too many initializers for '%s'", type_name(t));
        }
        for (;;) {                                     /* brace elision */
            IFrame *f = &st[sp - 1];
            int off;
            Type mt = frame_member(f->t, f->idx, &off);
            if (!is_aggregate(mt) || current_token.type == TOKEN_LBRACE || takes_whole(mt)) break;
            if (frame_count(mt) == 0) error("cannot initialize an empty aggregate");
            if (sp >= 64) error("initializer nested too deeply");
            st[sp].t = mt; st[sp].disp = f->disp + off; st[sp].idx = 0;
            sp++;
        }
        IFrame *f = &st[sp - 1];
        int off;
        Type mt = frame_member(f->t, f->idx, &off);
        init_value(base, f->disp + off, mt);
        cursor_advance(st, &sp);
        if (current_token.type != TOKEN_COMMA) break;
        next_token();
    }
    expect(TOKEN_RBRACE);
}

/* one initializer for exactly the (already zeroed) object t at disp(base) */
static void init_value(int base, int disp, Type t) {
    if (current_token.type == TOKEN_LBRACE) {
        next_token();
        if (is_aggregate(t)) { init_braced(base, disp, t); return; }
        if (current_token.type == TOKEN_RBRACE) { next_token(); store_ex(lv_mem(base, disp, t), zero_of(t), 1); return; }
        store_ex(lv_mem(base, disp, t), parse_assign(), 1);   /* int x = {5}; */
        if (current_token.type == TOKEN_COMMA) next_token();
        expect(TOKEN_RBRACE);
        return;
    }
    if (is_array(t)) {
        Type et = elem_of(t);
        if (str_inits(et)) {
            int units = str_units();
            if (units > arr_len(t)) error("initializer string is too long for %s[%d]", current_token.wide ? "wchar_t" : "char", arr_len(t));
            for (int i = 0; i < units; i++) {
                if (!current_token.wide) { EMIT(MOV, MEM(MT_I8, disp + i, base), IMM((signed char)current_token.str[i])); continue; }
                long long v;
                if (WCHAR_SIZE == 2) { unsigned short u; memcpy(&u, current_token.str + 2 * i, 2); v = u; }
                else { int u; memcpy(&u, current_token.str + 4 * i, 4); v = u; }
                EMIT(MOV, MEM(mem_type(et), disp + i * WCHAR_SIZE, base), IMM(v));
            }
            next_token();
            return;
        }
        if (current_token.type == TOKEN_STR)
            error(current_token.wide ? "L\"...\" initializes wchar_t arrays only" : "a string literal initializes char arrays only");
        error("array initializer must be a list");
    }
    store_ex(lv_mem(base, disp, t), parse_assign(), 1);   /* scalar, or struct copied from an expression */
}

static void init_object(int base, int disp, Type t, int top) { (void)top; init_value(base, disp, t); }


/* ============================================================ parser: statements */
static int break_stack[128], continue_stack[128], loop_depth;   /* label ids; continue = -1 for switch */

typedef struct { int n, cap, def; long long *val; int *lab; Type ty; } SwitchCtx;
static SwitchCtx *sw_stack[64];
static int sw_depth;

static Func *declare_function(Type ft, const char *name);
static void emit_global_init(Global *g);

typedef struct { char name[32]; int label, defined, line; } GotoLabel;
static GotoLabel glabels[512];
static int nglabels;

static int goto_label(const char *name, int define) {
    GotoLabel *g = NULL;
    for (int i = 0; i < nglabels; i++)
        if (strcmp(glabels[i].name, name) == 0) g = &glabels[i];
    if (!g) {
        if (nglabels >= 512) error("too many labels");
        g = &glabels[nglabels++];
        snprintf(g->name, sizeof g->name, "%.*s", (int)sizeof g->name - 1, name);
        g->label = new_label();
        g->defined = 0;
        g->line = cur_line;
    }
    if (define) {
        if (g->defined) error("duplicate label '%s'", name);
        g->defined = 1;
    }
    return g->label;
}

/* names used as a whole value in the current function (passed, returned, copied from): keeping
   such a struct in registers would spill it at every use, so it stays in memory */
static char whole_read[512][32];
static int nwhole_read;
static void prescan_whole_reads(void) {   /* current token: the function's '{' */
    LexState s = save_lex();
    nwhole_read = 0;
    TokenType prev = TOKEN_LBRACE;
    next_token();
    for (int depth = 1; current_token.type != TOKEN_EOF && depth > 0; ) {
        Token t = current_token;
        if (t.type == TOKEN_LBRACE) depth++;
        else if (t.type == TOKEN_RBRACE) depth--;
        next_token();
        if (t.type == TOKEN_IDENT && prev != TOKEN_DOT && prev != TOKEN_ARROW && prev != TOKEN_IDENT &&
            prev != TOKEN_RBRACE && prev != TOKEN_ASTERISK && prev != TOKEN_STRUCT && prev != TOKEN_UNION &&
            current_token.type != TOKEN_DOT && current_token.type != TOKEN_ASSIGN &&
            current_token.type != TOKEN_LPAREN && !(prev == TOKEN_COMMA && current_token.type == TOKEN_SEMI) &&
            nwhole_read < 512)                         /* `T a, b;` declares b */
            snprintf(whole_read[nwhole_read++], 32, "%.31s", t.text);
        prev = t.type;
    }
    restore_lex(s);
}
static int is_whole_read(const char *name) {
    for (int i = 0; i < nwhole_read; i++) if (!strcmp(whole_read[i], name)) return 1;
    return 0;
}

/* a struct local whose members can live in registers: up to 8 scalar members, no arrays,
   nested structs, bit-fields or anonymous members (and its address is never taken) */
static int sra_ok(Type t) {
    Struct *sd = &structs[t.sidx];
    if (sd->is_union || !sd->complete || sd->n == 0 || sd->n > 8) return 0;
    for (int i = 0; i < sd->n; i++) {
        Type mt = sd->m[i].ty;
        if (!sd->m[i].name[0] || mt.bfw || is_aggregate(mt) || is_func(mt)) return 0;
    }
    return 1;
}
static void sra_load(Var *v, int base, int disp) {      /* memory -> member registers */
    Struct *sd = &structs[v->ty.sidx];
    for (int i = 0; i < v->sra; i++) {
        Type mt = sd->m[i].ty;
        Opnd o[2] = {R(v->sra_reg[i]), MEM(mem_type(mt), disp + sd->m[i].off, base)};
        emit_ops(0, mov_insn(reg_kind(mt)), o, 2);
    }
}
static Val sra_spill(Val v) {                           /* member registers -> the spill slot, as a V_MEM value */
    Var *sv = &vars[v.base];
    Struct *sd = &structs[sv->ty.sidx];
    for (int i = 0; i < sv->sra; i++) {
        Type mt = sd->m[i].ty;
        Opnd o[2] = {MEM(mem_type(mt), sd->m[i].off, sv->reg), R(sv->sra_reg[i])};
        emit_ops(0, mov_insn(reg_kind(mt)), o, 2);
    }
    return lv_mem(sv->reg, 0, v.ty);
}

static Var *declare_var(const char *name, Type t) {
    for (int i = nvars - 1; i >= 0 && vars[i].depth == scope_depth; i--)
        if (strcmp(vars[i].name, name) == 0) error("redeclaration of '%s'", name);
    if (is_void(t)) error("variable '%s' declared void", name);
    if (is_struct_val(t) && !structs[t.sidx].complete) error("variable '%s' has incomplete type", name);
    if (nvars >= 2048) error("too many variables");
    Var *v = &vars[nvars++]; memset(v, 0, sizeof *v);
    strcpy(v->name, name);
    v->ty = t;
    v->depth = scope_depth;
    v->global = -1;
    v->is_mem = is_aggregate(t) || is_addr_taken(name);
    v->sra = 0;
    if (v->is_mem) v->reg = new_slot(name, size_of(t));   /* the register holds the slot's address */
    else v->reg = new_reg(name, reg_kind(t), 0);
    if (is_struct_val(t) && !is_addr_taken(name) && !is_whole_read(name) && sra_ok(t)) {   /* members in registers; the slot is a spill area */
        Struct *sd = &structs[t.sidx];
        v->sra = sd->n;
        for (int i = 0; i < sd->n; i++) {
            char rn[48];
            snprintf(rn, sizeof rn, "%.20s_%.20s", name, sd->m[i].name);
            v->sra_reg[i] = new_reg(rn, reg_kind(sd->m[i].ty), 0);
        }
    }
    return v;
}

/* ---------------- static locals: a global with a private name; non-constant initializers run
   from an init function generated after the current function (they can't nest) */
typedef struct { int g; LexState init; } PendingInit;
static PendingInit pending_inits[512];
static int npending_inits, static_counter;
static int ginit_with_statics;
static Var static_vars[512];      /* static locals of the current function */
static int nstatic_vars;

static void skip_initializer(void) {
    for (int depth = 0; current_token.type != TOKEN_EOF; next_token()) {
        TokenType t = current_token.type;
        if (depth == 0 && (t == TOKEN_COMMA || t == TOKEN_SEMI)) return;
        if (t == TOKEN_LBRACE || t == TOKEN_LPAREN || t == TOKEN_LBRACKET) depth++;
        if (t == TOKEN_RBRACE || t == TOKEN_RPAREN || t == TOKEN_RBRACKET) depth--;
    }
}

/* `struct S s;` before struct S is complete: storage is emitted once the size is known */
static void ensure_global_storage(Global *g) {
    if (!g->deferred) return;
    if (!structs[g->ty.sidx].complete) error("variable '%s' has incomplete type", g->name);
    g->deferred = 0;
    be_bss(g->name, size_of(g->ty));
}

static Global *add_global(const char *name, Type t) {
    if (nglobals >= 1024) error("too many globals");
    Global *g = &globals[nglobals++];
    snprintf(g->name, sizeof g->name, "%s", name);
    g->ty = t;
    g->has_init = 0;
    return g;
}

/* storage + initializer of a global (or static local); current token is after the declarator */
static void define_global_storage(Global *g, int defer_dynamic) {
    Type t = g->ty;
    if (current_token.type != TOKEN_ASSIGN) { be_bss(g->name, size_of(t)); return; }   /* zero */
    next_token();
    g->has_init = 1;
    CV cv;
    Token nx = peek_token();
    if (is_pointer(t) && current_token.type == TOKEN_STR && (nx.type == TOKEN_SEMI || nx.type == TOKEN_COMMA)) {
        char item[32];
        snprintf(item, sizeof item, "%s", be_string(current_token.str, current_token.str_len + (current_token.wide ? WCHAR_SIZE - 1 : 0)));
        next_token();
        be_ref_data(g->name, item);
    } else if (!is_aggregate(t) && try_const_init(&cv)) {
        if (is_fp(t)) be_scalar_data(g->name, t, 0, cv.fp ? cv.d : (double)cv.i);
        else be_scalar_data(g->name, t, trunc_const(cv.fp ? (long long)cv.d : cv.i, t), 0);
    } else {
        be_bss(g->name, size_of(t));
        if (defer_dynamic) {
            if (npending_inits >= 512) error("too many static initializers");
            pending_inits[npending_inits].g = (int)(g - globals);
            pending_inits[npending_inits++].init = save_lex();
            skip_initializer();
        } else {
            emit_global_init(g);
        }
    }
}

static Func *declare_function(Type ft, const char *name);
static void nested_function(Type t, const char *name) {   /* current token: '{' of its body */
    if (nfunc_aliases >= 64 || npending_nested >= 64) error("too many nested functions");
    for (int i = nfunc_aliases - 1; i >= 0 && func_aliases[i].depth == scope_depth; i--)
        if (strcmp(func_aliases[i].name, name) == 0) error("redefinition of nested function '%s'", name);
    NestedFunc *n = &pending_nested[npending_nested++];
    snprintf(n->name, sizeof n->name, "%s", name);
    snprintf(n->target, sizeof n->target, "__n%d_%.24s", nested_counter++, name);
    snprintf(n->file, sizeof n->file, "%s", cur_file);
    n->body = src - 1;                                 /* the '{' */
    n->line = cur_line;
    Func *f = declare_function(t, n->target);
    f->is_import = 0;                                  /* defined after the enclosing function */
    FuncAlias *a = &func_aliases[nfunc_aliases++];
    snprintf(a->name, sizeof a->name, "%s", name);
    snprintf(a->target, sizeof a->target, "%s", n->target);
    a->depth = scope_depth;
    int depth = 0;                                     /* skip the body for now */
    for (; current_token.type != TOKEN_EOF; next_token()) {
        if (current_token.type == TOKEN_LBRACE) depth++;
        else if (current_token.type == TOKEN_RBRACE && --depth == 0) break;
    }
    expect(TOKEN_RBRACE);
}

static void define_function(Func *f);
static void flush_nested(void) {   /* compile the nested functions found so far (and theirs) */
    while (npending_nested > 0) {
        NestedFunc n = pending_nested[0];
        memmove(pending_nested, pending_nested + 1, --npending_nested * sizeof *pending_nested);
        LexState ls = save_lex();
        char file[sizeof cur_file];
        snprintf(file, sizeof file, "%s", cur_file);
        src = n.body;
        cur_line = n.line;
        snprintf(cur_file, sizeof cur_file, "%s", n.file);
        next_token();
        FuncAlias *a = &func_aliases[nfunc_aliases++];   /* its own name, for recursion */
        snprintf(a->name, sizeof a->name, "%s", n.name);
        snprintf(a->target, sizeof a->target, "%s", n.target);
        a->depth = 0;
        define_function(find_func(n.target));
        nfunc_aliases--;
        restore_lex(ls);
        snprintf(cur_file, sizeof cur_file, "%s", file);
    }
}

static void parse_local_decl(void) {
    if (current_token.type == TOKEN_TYPEDEF) { parse_typedef(); return; }
    Type base = parse_type_spec();
    if (spec_typedef) { parse_typedef_names(base); return; }
    int is_static = spec_static, spec_extern_local = spec_extern, cx = spec_constexpr;
    if (current_token.type == TOKEN_SEMI) { next_token(); return; }   /* struct X {...}; */
    for (;;) {
        char name[32];
        vla_ok = !is_static && !spec_extern_local;
        Type t = parse_declarator(base, name, 0);
        vla_ok = 0;
        spec_constexpr = cx;
        if (constexpr_decl(t, name)) {
        } else if (is_func(t) && current_token.type == TOKEN_LBRACE) {
            if (!c2_ext) error("nested function '%s': nested functions need a .c2 file", name);
            nested_function(t, name);                  /* .c2: a function defined inside a function */
            return;
        } else if (is_func(t)) {                           /* local prototype */
            declare_function(t, name);
        } else if (is_vla(t)) {
            if (current_token.type == TOKEN_ASSIGN) error("variable-length array '%s' may not be initialized", name);
            for (int i = nvars - 1; i >= 0 && vars[i].depth == scope_depth; i--)
                if (strcmp(vars[i].name, name) == 0) error("redeclaration of '%s'", name);
            if (nvars >= 2048) error("too many variables");
            if (nvla_scopes >= 256) error("too many variable-length arrays");
            int len = vla_len_reg[-2 - arr_len(t)], bytes = new_temp(RK_I), save = new_temp(RK_I);
            EMIT(MUL, R(bytes), R(len), IMM(size_of(elem_of(t))));
            if (nvla_scopes == 0 || vla_scopes[nvla_scopes - 1].depth != scope_depth) {
                EMIT(BSTART, R(save));                 /* once per block: bend restores the stack */
                vla_scopes[nvla_scopes].save_reg = save;
                vla_scopes[nvla_scopes++].depth = scope_depth;
            }
            Var *v = &vars[nvars++]; memset(v, 0, sizeof *v);
            strcpy(v->name, name);
            v->ty = t;
            v->depth = scope_depth;
            v->global = -1;
            v->is_mem = 1;
            v->reg = new_reg(name, RK_I, 0);
            EMIT(ALLOCA, R(v->reg), R(bytes));
            Val a[3] = {rv_reg(v->reg, ptr_to(mk_type(TY_VOID))), rv_op(IMM(0), int_type()), rv_reg(bytes, long_type())};
            if (c2_ext) call_builtin("__c2_memset", a, 3);   /* .c2: zeroed like every other variable */
        } else {
            int has_init = current_token.type == TOKEN_ASSIGN;
            if (is_array(t) && arr_len(t) < 0) {
                if (!has_init) error("array '%s' needs a size or an initializer", name);
                t = set_arr_len(t, count_initializer(elem_of(t)));
            }
            if (spec_extern_local) {                   /* extern int x; refers to the global */
                Global *g = find_global(name);
                if (!g) { g = add_global(name, t); be_bss(name, size_of(t)); }
                else if (!same_type(g->ty, t)) error("conflicting types for '%s'", name);
                if (nvars >= 2048) error("too many variables");
                Var *v = &vars[nvars++]; memset(v, 0, sizeof *v);
                strcpy(v->name, name);
                v->ty = g->ty;
                v->depth = scope_depth;
                v->is_mem = 1;
                v->reg = -1;
                v->global = (int)(g - globals);
            } else if (is_static) {
                for (int i = nvars - 1; i >= 0 && vars[i].depth == scope_depth; i--)
                    if (strcmp(vars[i].name, name) == 0) error("redeclaration of '%s'", name);
                if (is_struct_val(t) && !structs[t.sidx].complete) error("variable '%s' has incomplete type", name);
                char gname[40];
                snprintf(gname, sizeof gname, "__c2_s%d_%.24s", static_counter++, name);
                Global *g = add_global(gname, t);
                if (nvars >= 2048) error("too many variables");
                Var *v = &vars[nvars++]; memset(v, 0, sizeof *v);
                strcpy(v->name, name);
                v->ty = t;
                v->depth = scope_depth;
                v->is_mem = 1;
                v->reg = -1;
                v->global = (int)(g - globals);
                if (nstatic_vars < 512) static_vars[nstatic_vars++] = *v;   /* visible to deferred initializers */
                define_global_storage(g, 1);               /* initialised once, before main */
            } else {
                Var *v = declare_var(name, t);
                Val lv = var_val(v);
                if (v->sra) {                              /* struct in registers */
                    Struct *sd = &structs[t.sidx];
                    if (has_init) {                        /* initialise the spill slot, then load it */
                        next_token();
                        Val mem = lv_mem(v->reg, 0, t);
                        if (current_token.type == TOKEN_LBRACE) { zero_mem(v->reg, 0, size_of(t)); init_object(v->reg, 0, t, 1); }
                        else store_ex(mem, parse_assign(), 1);
                        sra_load(v, v->reg, 0);
                    } else if (c2_ext) {                   /* .c2: zeroed; .c: undefined, as in C */
                        for (int i = 0; i < v->sra; i++) {
                            Type mt = sd->m[i].ty;
                            Opnd o[2] = {R(v->sra_reg[i]), zero_of(mt).op};
                            emit_ops(0, mov_insn(reg_kind(mt)), o, 2);
                        }
                    }
                } else if (has_init) {
                    next_token();
                    if (is_aggregate(t)) {
                        if (!(is_struct_val(t) && current_token.type != TOKEN_LBRACE)) zero_mem(lv.base, lv.disp, size_of(t));
                        init_object(lv.base, lv.disp, t, 1);
                    } else if (current_token.type == TOKEN_LBRACE) {
                        next_token();
                        store_ex(lv, parse_assign(), 1);
                        if (current_token.type == TOKEN_COMMA) next_token();
                        expect(TOKEN_RBRACE);
                    } else {
                        store_ex(lv, parse_assign(), 1);
                    }
                } else if (!c2_ext && is_aggregate(t)) {  /* .c: arrays and structs are not cleared */
                } else if (!c2_ext) {                      /* .c: a scalar gets 0 anyway (a free mov, and no stale
                                                              bits in narrow types); reading it first warns */
                    v->uninit = 1;
                    store_ex(lv, zero_of(t), 1);
                } else if (is_aggregate(t)) {
                    zero_mem(lv.base, lv.disp, size_of(t));    /* .c2: everything starts zeroed */
                } else {
                    store_ex(lv, zero_of(t), 1);
                }
            }
        }
        if (current_token.type != TOKEN_COMMA) break;
        next_token();
    }
    expect(TOKEN_SEMI);
}
/* jump to `label` when the condition is true (when_true) or false; constant conditions fold */
static void emit_cond_jump(Val v, int when_true, int label) {
    v = rvalue(v);
    if (v.op.k == OK_INT && is_integer(v.ty)) {
        if ((v.op.i != 0) == when_true) EMIT(JMP, LBL(label));
        return;
    }
    int c = cond_reg(v);
    if (when_true) EMIT(BT, LBL(label), R(c));
    else EMIT(BF, LBL(label), R(c));
}

/* conditions of if/while/for/do as jumping code: `a && b || c` branches straight to its target
   instead of computing 0/1 first.  Used when the condition's top level is only && / || chains. */
static int cj_scan(TokenType stop) {   /* does `stop` come before the end of the current operand? */
    LexState s = save_lex();
    int depth = 0, found = 0;
    for (;; next_token()) {
        TokenType t = current_token.type;
        if (t == TOKEN_EOF) break;
        if (t == TOKEN_LPAREN || t == TOKEN_LBRACKET || t == TOKEN_LBRACE) depth++;
        else if (t == TOKEN_RPAREN || t == TOKEN_RBRACKET || t == TOKEN_RBRACE) { if (depth-- == 0) break; }
        else if (depth == 0) {
            if (t == stop) { found = 1; break; }
            if (t == TOKEN_SEMI || t == TOKEN_OROR || (stop == TOKEN_ANDAND && t == TOKEN_ANDAND)) break;
        }
    }
    restore_lex(s);
    return found;
}
static int cj_is_logical(void) {        /* only && / || at the top level (no ?: , = there) */
    LexState s = save_lex();
    int depth = 0, logical = 0;
    for (;; next_token()) {
        TokenType t = current_token.type;
        if (t == TOKEN_EOF || t == TOKEN_SEMI) break;
        if (t == TOKEN_LPAREN || t == TOKEN_LBRACKET || t == TOKEN_LBRACE) depth++;
        else if (t == TOKEN_RPAREN || t == TOKEN_RBRACKET || t == TOKEN_RBRACE) { if (depth-- == 0) break; }
        else if (depth == 0) {
            if (t == TOKEN_ANDAND || t == TOKEN_OROR) logical = 1;
            else if (t == TOKEN_QUESTION || t == TOKEN_COMMA || (t >= TOKEN_ASSIGN && t <= TOKEN_SHR_ASSIGN)) { logical = 0; break; }
        }
    }
    restore_lex(s);
    return logical;
}
static int cj_group_is_operand(void) {  /* '(' logical ')' that is a whole && / || operand */
    if (current_token.type != TOKEN_LPAREN) return 0;
    LexState s = save_lex();
    next_token();
    int ok = cj_is_logical();
    if (ok) {                                       /* after the matching ')' */
        for (int depth = 0; current_token.type != TOKEN_EOF; next_token()) {
            TokenType t = current_token.type;
            if (t == TOKEN_LPAREN || t == TOKEN_LBRACKET || t == TOKEN_LBRACE) depth++;
            else if (t == TOKEN_RPAREN || t == TOKEN_RBRACKET || t == TOKEN_RBRACE) { if (depth-- == 0) break; }
        }
        next_token();
        TokenType t = current_token.type;
        ok = t == TOKEN_ANDAND || t == TOKEN_OROR || t == TOKEN_RPAREN || t == TOKEN_SEMI;
    }
    restore_lex(s);
    return ok;
}
static void cj_or(int jt, int label);
static void cj_operand(int jt, int label) {
    if (current_token.type == TOKEN_NOT && peek_token().type == TOKEN_LPAREN) {
        LexState s = save_lex();
        next_token();
        if (cj_group_is_operand()) { cj_operand(!jt, label); return; }
        restore_lex(s);
    }
    if (cj_group_is_operand()) {
        next_token();
        cj_or(jt, label);
        expect(TOKEN_RPAREN);
        return;
    }
    emit_cond_jump(parse_binary(1), jt, label);
}
static void cj_and(int jt, int label) {
    int skip = -1;
    for (;;) {
        if (!cj_scan(TOKEN_ANDAND)) { cj_operand(jt, label); break; }
        if (!jt) cj_operand(0, label);                 /* any false operand: the chain is false */
        else { if (skip < 0) skip = new_label(); cj_operand(0, skip); }
        expect(TOKEN_ANDAND);
    }
    if (skip >= 0) place_label(skip);
}
static void cj_or(int jt, int label) {
    int done = -1;
    for (;;) {
        if (!cj_scan(TOKEN_OROR)) { cj_and(jt, label); break; }
        if (jt) cj_and(1, label);                      /* any true chain: the condition is true */
        else { if (done < 0) done = new_label(); cj_and(1, done); }
        expect(TOKEN_OROR);
    }
    if (done >= 0) place_label(done);
}
/* parse a condition and jump to `label` when it is true (when_true) or false */
static void cond_jump(int when_true, int label) {
    if (cj_is_logical()) cj_or(when_true, label);
    else emit_cond_jump(parse_expr(), when_true, label);
}

static void push_loop(int brk, int cont) {
    if (loop_depth >= 128) error("nesting too deep");
    break_stack[loop_depth] = brk;
    continue_stack[loop_depth++] = cont;
}

/* __on_enter / __on_leave: when the script defines them (or the host provides them with
   C2_add_symbol), every other function calls __on_enter(func, file, line) on entry and
   __on_leave(func, file, line) before each return: profilers, call stacks, tracing.  Without them
   nothing is emitted.  The hooks themselves (any __on_* function) are not instrumented. */
typedef struct C2 C2;
static C2 *compiling;                     /* the instance compiling now (host symbols) */
static int hook_line;                     /* line for the next hook call (0: the current line) */
static int host_has_symbol(C2 *cc, const char *name);
static int is_defined_in_file(const char *name);
static Func *hook_func(const char *name) {
    if (!cur_func || !strncmp(cur_func->name, "__on_", 5) || !strncmp(cur_func->name, "__c2_", 5)) return NULL;
    if (!is_defined_in_file(name) && !(compiling && host_has_symbol(compiling, name))) return NULL;
    Func *f = find_func(name);
    if (!f) {                                          /* void name(const char *func, const char *file, int line) */
        int si = new_sig();
        sigs[si].ret = mk_type(TY_VOID);
        sigs[si].np = 3;
        sigs[si].pt[0] = sigs[si].pt[1] = ptr_to(add_const(mk_type(TY_CHAR)));
        sigs[si].pt[2] = int_type();
        f = declare_function(func_of(mk_type(TY_VOID), si), name);
        f->is_import = !is_defined_in_file(name);
    }
    return f;
}

static void emit_hook(const char *name) {
    Func *f = hook_func(name);
    if (!f) return;
    Val a[3];
    const char *strs[2] = {cur_func->name, cur_file};
    for (int i = 0; i < 2; i++) {
        int r = new_temp(RK_I);
        EMIT(MOV, R(r), REF(be_string(strs[i], (int)strlen(strs[i]))));
        a[i] = rv_reg(r, ptr_to(add_const(mk_type(TY_CHAR))));
    }
    a[2] = rv_op(IMM(hook_line ? hook_line : cur_line), int_type());
    hook_line = 0;
    gen_call(f, a, 3);
}

/* self tail calls: `return f(args);` inside f becomes "assign the parameters, jump back to the
   start", so tail recursion of any depth runs in constant stack.  When that is not possible the
   call stays a plain recursive call, with a warning saying why. */
static int tail_label = -1;                /* start of the current function's body */
static int tail_param_reg[MAX_PARAMS];      /* its parameters' registers */
static int parse_args(Val *args);

static int try_tail_call(void) {   /* current token: the first after `return`; 1 if it emitted the jump */
    if (current_token.type != TOKEN_IDENT || !cur_func || tail_label < 0) return 0;
    if (find_var(current_token.text) || find_func(current_token.text) != cur_func) return 0;
    if (peek_token().type != TOKEN_LPAREN) return 0;
    LexState ls = save_lex();
    EmitMark em = emit_mark();
    Func *f = cur_func;
    next_token();
    Val args[MAX_PARAMS + 32];
    Callee ce = callee_of_func(f);
    no_uninit_warn++;                                  /* (parsed again below if this is not a tail call) */
    int n = c2_ext ? parse_args_c2(&ce, args, NULL) : parse_args(args);
    no_uninit_warn--;
    if (current_token.type != TOKEN_SEMI) { emit_rollback(em); restore_lex(ls); return 0; }   /* f(x) + 1 ... */
    const char *why = NULL;
    if (hook_func("__on_enter") || hook_func("__on_leave")) why = "";   /* hooks want every call: no warning */
    else if (f->vararg) why = "it has '...'";
    else if (n != f->np) why = "the argument count differs";
    else if (naddr_taken > 0) why = "the address of a local or parameter is taken";
    else if (nvla_scopes > 0) why = "a variable-length array is in scope";
    else if (is_struct_val(f->ret)) why = "it returns a struct";
    for (int i = 0; !why && i < f->np; i++)
        if (is_struct_val(f->pt[i])) why = "it has struct parameters";
    if (why) {
        if (why[0]) diag("%s:%d: warning: tail call of '%s' stays a recursive call: %s\n", cur_file, cur_line, f->name, why);
        emit_rollback(em);
        restore_lex(ls);
        return 0;
    }
    int tmp[MAX_PARAMS];
    for (int i = 0; i < n; i++) {                      /* all values first: f(b, a) swaps */
        Val v = rvalue(convert(args[i], f->pt[i]));
        RegKind rk = reg_kind(f->pt[i]);
        tmp[i] = new_temp(rk);
        EMIT_OP(mov_insn(rk), R(tmp[i]), v.op);
    }
    for (int i = 0; i < n; i++) EMIT_OP(mov_insn(reg_kind(f->pt[i])), R(tail_param_reg[i]), R(tmp[i]));
    EMIT(JMP, LBL(tail_label));
    next_token();                                      /* ; */
    return 1;
}

static void emit_return(Val *v) {
    Type rt = cur_func->ret;
    MemT q[2];
    int nq = abi_ret_regs(rt, q);
    if (!v) {
        if (nq) {                                      /* struct in registers: return zeros */
            Opnd z[2];                                 /* (MIR mishandles `ret 0, 0`: use registers) */
            for (int i = 0; i < nq; i++) {
                int t = new_temp(mt_rk(q[i]));
                Opnd mv[2] = {R(t), q[i] == MT_F ? FIMM(0) : q[i] == MT_D ? DIMM(0) : IMM(0)};
                emit_ops(0, mov_insn(mt_rk(q[i])), mv, 2);
                z[i] = R(t);
            }
            emit_hook("__on_leave");
            emit_ops(0, I_RET, z, nq);
        } else if (is_struct_val(rt)) {
            Opnd a = R(cur_sret_reg);
            emit_hook("__on_leave");
            emit_ops(0, I_RET, &a, 1);
        } else if (is_void(rt)) { emit_hook("__on_leave"); emit_ops(0, I_RET, NULL, 0); }
        else { emit_hook("__on_leave"); EMIT(RET, zero_of(rt).op); }
        return;
    }
    if (is_void(rt)) error("return with a value in a void function");
    Type plain = unconst(rt);
    Val r = convert(*v, plain);
    if (nq) {                                          /* struct in registers */
        Opnd ro[2];
        for (int i = 0; i < nq; i++) {
            int t = new_temp(mt_rk(q[i]));
            Opnd ld[2] = {R(t), MEM(q[i], r.disp + 8 * i, r.base)};
            emit_ops(0, mov_insn(mt_rk(q[i])), ld, 2);
            ro[i] = R(t);
        }
        emit_hook("__on_leave");
        emit_ops(0, I_RET, ro, nq);
    } else if (is_struct_val(rt)) {                   /* hidden result buffer */
        copy_mem(cur_sret_reg, 0, r.base, r.disp, size_of(rt));
        Opnd a = R(cur_sret_reg);
        emit_hook("__on_leave");
        emit_ops(0, I_RET, &a, 1);
    } else {
        if (r.op.k != OK_REG && r.op.k != OK_INT && r.op.k != OK_FLT && r.op.k != OK_DBL) {
            RegKind rk = reg_kind(plain);              /* keep the value in a register across the hook */
            int t = new_temp(rk);
            EMIT_OP(mov_insn(rk), R(t), r.op);
            r.op = R(t);
        }
        emit_hook("__on_leave");
        EMIT(RET, r.op);
    }
}

static void parse_statement(void) {
    switch (current_token.type) {
    case TOKEN_LBRACE:
        next_token();
        enter_scope();
        while (current_token.type != TOKEN_RBRACE && current_token.type != TOKEN_EOF) parse_statement();
        expect(TOKEN_RBRACE);
        leave_scope();
        return;
    case TOKEN_SEMI:
        next_token();
        return;
    case TOKEN_BREAK:
        next_token();
        expect(TOKEN_SEMI);
        if (loop_depth == 0) error("break outside loop/switch");
        EMIT(JMP, LBL(break_stack[loop_depth - 1]));
        return;
    case TOKEN_CONTINUE: {
        next_token();
        expect(TOKEN_SEMI);
        int d = loop_depth - 1;
        while (d >= 0 && continue_stack[d] < 0) d--;
        if (d < 0) error("continue outside loop");
        EMIT(JMP, LBL(continue_stack[d]));
        return;
    }
    case TOKEN_GOTO: {
        next_token();
        if (current_token.type != TOKEN_IDENT) error("label name expected after goto");
        int l = goto_label(current_token.text, 0);
        next_token();
        expect(TOKEN_SEMI);
        EMIT(JMP, LBL(l));
        return;
    }
    case TOKEN_IF: {
        next_token();
        expect(TOKEN_LPAREN);
        int l_else = new_label();
        cond_jump(0, l_else);
        expect(TOKEN_RPAREN);
        parse_statement();
        if (current_token.type == TOKEN_ELSE) {
            next_token();
            int l_end = new_label();
            EMIT(JMP, LBL(l_end));
            place_label(l_else);
            parse_statement();
            place_label(l_end);
        } else {
            place_label(l_else);
        }
        return;
    }
    case TOKEN_WHILE: {
        /* rotated: test once before the loop, then at the bottom (one branch per iteration) */
        next_token();
        int l_top = new_label(), l_cont = new_label(), l_brk = new_label();
        expect(TOKEN_LPAREN);
        LexState cond = save_lex();
        cond_jump(0, l_brk);
        expect(TOKEN_RPAREN);
        place_label(l_top);
        push_loop(l_brk, l_cont);
        parse_statement();
        loop_depth--;
        place_label(l_cont);
        LexState after = save_lex();
        restore_lex(cond);
        cond_jump(1, l_top);        /* the condition again, compiled at the bottom */
        restore_lex(after);
        place_label(l_brk);
        return;
    }
    case TOKEN_DO: {
        next_token();
        int l_top = new_label(), l_cont = new_label(), l_brk = new_label();
        place_label(l_top);
        push_loop(l_brk, l_cont);
        parse_statement();
        loop_depth--;
        place_label(l_cont);
        expect(TOKEN_WHILE);
        expect(TOKEN_LPAREN);
        cond_jump(1, l_top);
        expect(TOKEN_RPAREN);
        expect(TOKEN_SEMI);
        place_label(l_brk);
        return;
    }
    case TOKEN_FOR: {
        next_token();
        expect(TOKEN_LPAREN);
        enter_scope();                                 /* for (int i ...) is scoped to the loop */
        if (is_type_start(current_token)) parse_local_decl();
        else {
            if (current_token.type != TOKEN_SEMI) rvalue(parse_expr());
            expect(TOKEN_SEMI);
        }
        int l_top = new_label(), l_cont = new_label(), l_brk = new_label();
        LexState cond = save_lex();                    /* rotated like while */
        int has_cond = current_token.type != TOKEN_SEMI;
        if (has_cond) cond_jump(0, l_brk);
        expect(TOKEN_SEMI);
        /* the increment is generated after the body: skip it now, come back for it later */
        LexState inc = save_lex();
        for (int depth = 0; !(current_token.type == TOKEN_RPAREN && depth == 0); next_token()) {
            if (current_token.type == TOKEN_EOF) error("unterminated for");
            if (current_token.type == TOKEN_LPAREN) depth++;
            if (current_token.type == TOKEN_RPAREN) depth--;
        }
        expect(TOKEN_RPAREN);
        place_label(l_top);
        push_loop(l_brk, l_cont);
        parse_statement();
        loop_depth--;
        place_label(l_cont);
        LexState after = save_lex();
        restore_lex(inc);
        if (current_token.type != TOKEN_RPAREN) rvalue(parse_expr());
        if (has_cond) {
            restore_lex(cond);
            cond_jump(1, l_top);
        } else {
            EMIT(JMP, LBL(l_top));
        }
        restore_lex(after);
        place_label(l_brk);
        leave_scope();
        return;
    }
    case TOKEN_SWITCH: {
        /* body is any statement; case/default labels may sit anywhere inside it (Duff's device).
           Dispatch code goes after the body: a chain of beq to the case labels. */
        next_token();
        expect(TOKEN_LPAREN);
        Val sv = rvalue(parse_expr());
        if (!is_integer(sv.ty)) error("switch on '%s', which is not an integer", type_name(sv.ty));
        Type st = promote(sv.ty);
        sv = clean32(convert(sv, st));                 /* beq compares 64 bits */
        expect(TOKEN_RPAREN);
        int sw = new_temp(RK_I);                       /* evaluate once */
        EMIT(MOV, R(sw), sv.op);
        int l_disp = new_label(), l_brk = new_label();
        if (sw_depth >= 64) error("switch nesting too deep");
        SwitchCtx *c = calloc(1, sizeof *c);
        c->def = -1;
        c->ty = st;
        sw_stack[sw_depth++] = c;
        EMIT(JMP, LBL(l_disp));
        push_loop(l_brk, -1);
        parse_statement();
        loop_depth--;
        sw_depth--;
        EMIT(JMP, LBL(l_brk));
        place_label(l_disp);
        int l_def = c->def >= 0 ? c->def : l_brk;
        long long lo = 0, hi = 0;
        for (int i = 0; i < c->n; i++) {
            if (i == 0 || c->val[i] < lo) lo = c->val[i];
            if (i == 0 || c->val[i] > hi) hi = c->val[i];
        }
        unsigned long long span = (unsigned long long)hi - (unsigned long long)lo + 1;
        if (c->n >= 4 && span <= 4096 && span <= 3ull * c->n) {   /* dense: a jump table */
            int t = new_temp(RK_I);
            EMIT(SUB, R(t), R(sw), IMM(lo));
            EMIT(UBGT, LBL(l_def), R(t), IMM((long long)span - 1));
            Opnd *ops = malloc((span + 1) * sizeof *ops);
            ops[0] = R(t);
            for (unsigned long long k = 0; k < span; k++) ops[k + 1] = LBL(l_def);
            for (int i = 0; i < c->n; i++) ops[c->val[i] - lo + 1] = LBL(c->lab[i]);
            emit_ops(0, I_SWITCH, ops, (int)span + 1);
            free(ops);
        } else {
            for (int i = 0; i < c->n; i++) EMIT(BEQ, LBL(c->lab[i]), R(sw), IMM(c->val[i]));
            EMIT(JMP, LBL(l_def));
        }
        place_label(l_brk);
        free(c->val);
        free(c->lab);
        free(c);
        return;
    }
    case TOKEN_CASE:
    case TOKEN_DEFAULT: {
        int is_default = current_token.type == TOKEN_DEFAULT;
        if (sw_depth == 0) error("'%s' outside a switch", is_default ? "default" : "case");
        SwitchCtx *c = sw_stack[sw_depth - 1];
        next_token();
        int l = new_label();
        if (is_default) {
            if (c->def >= 0) error("duplicate default label");
            c->def = l;
        } else {
            long long v = trunc_const(const_int_expr(), c->ty);
            for (int i = 0; i < c->n; i++)
                if (c->val[i] == v) error("duplicate case value %lld", v);
            if (c->n == c->cap) {
                c->cap = c->cap ? c->cap * 2 : 16;
                c->val = realloc(c->val, c->cap * sizeof *c->val);
                c->lab = realloc(c->lab, c->cap * sizeof *c->lab);
            }
            c->val[c->n] = v;
            c->lab[c->n++] = l;
        }
        expect(TOKEN_COLON);
        place_label(l);
        if (current_token.type != TOKEN_RBRACE) parse_statement();
        return;
    }
    case TOKEN_RETURN:
        hook_line = cur_line;                          /* __on_leave reports the return's line */
        next_token();
        if (try_tail_call()) { hook_line = 0; return; }
        if (current_token.type == TOKEN_SEMI) {
            next_token();
            if (!is_void(cur_func->ret)) error("return without a value in a function returning '%s'", type_name(cur_func->ret));
            emit_return(NULL);
        } else {
            Val v = parse_expr();
            expect(TOKEN_SEMI);
            emit_return(&v);
        }
        return;
    case TOKEN_IDENT:
        if (peek_token().type == TOKEN_COLON && !is_type_start(current_token)) {   /* label: */
            place_label(goto_label(current_token.text, 1));
            next_token();
            next_token();
            if (current_token.type != TOKEN_RBRACE) parse_statement();
            return;
        }
        /* fall through */
    default:
        if (current_token.type == TOKEN_STATIC_ASSERT) { parse_static_assert(); return; }
        if (is_type_start(current_token) || current_token.type == TOKEN_TYPEDEF) { parse_local_decl(); return; }
        rvalue(parse_expr());
        expect(TOKEN_SEMI);
    }
}

/* ============================================================ parser: top level */
/* Which functions does this file define?  A prototype of anything else becomes an import. */
static char defined_names[1024][32];
static int ndefined;

static int is_defined_in_file(const char *name) {
    for (int i = 0; i < ndefined; i++)
        if (strcmp(defined_names[i], name) == 0) return 1;
    return 0;
}
/* ... and which global variables (a non-extern declaration at file scope)?  `extern T x;` of
   anything else is a host/library variable: an import. */
static char defined_vars[1024][32];
static int ndefined_vars;
static int is_var_defined_in_file(const char *name) {
    for (int i = 0; i < ndefined_vars; i++)
        if (strcmp(defined_vars[i], name) == 0) return 1;
    return 0;
}

static void prescan_definitions(void) {   /* call with the first token loaded; lexer state is restored */
    LexState saved = save_lex();
    int depth = 0, pdepth = 0, skip_decl = 0, in_init = 0;   /* skip_decl: extern/typedef statement */
    while (current_token.type != TOKEN_EOF) {
        TokenType tt = current_token.type;
        if (depth == 0 && pdepth == 0) {
            if (tt == TOKEN_SEMI) { skip_decl = in_init = 0; next_token(); continue; }
            if (tt == TOKEN_EXTERN || tt == TOKEN_TYPEDEF) skip_decl = 1;
            else if (tt == TOKEN_ASSIGN) in_init = 1;
            else if (tt == TOKEN_COMMA) in_init = 0;
        }
        if (tt == TOKEN_LPAREN) pdepth++;
        else if (tt == TOKEN_RPAREN) pdepth--;
        if (current_token.type == TOKEN_LBRACE) depth++;
        else if (current_token.type == TOKEN_RBRACE) { if (--depth == 0 && !in_init) skip_decl = 0; }
        else if (depth == 0 && pdepth == 0 && current_token.type == TOKEN_IDENT && !skip_decl && !in_init) {
            Token nx = peek_token();
            if ((nx.type == TOKEN_ASSIGN || nx.type == TOKEN_SEMI || nx.type == TOKEN_COMMA || nx.type == TOKEN_LBRACKET)
                && !is_type_start(current_token) && ndefined_vars < 1024)
                snprintf(defined_vars[ndefined_vars++], 32, "%.31s", current_token.text);
        }
        if (depth == 0 && pdepth == 0 && current_token.type == TOKEN_IDENT) {
            char name[32];
            snprintf(name, sizeof name, "%.31s", current_token.text);
            next_token();
            if (current_token.type != TOKEN_LPAREN) continue;
            for (int pd = 0; current_token.type != TOKEN_EOF; ) {   /* skip the parameter list */
                if (current_token.type == TOKEN_LPAREN) pd++;
                else if (current_token.type == TOKEN_RPAREN && --pd == 0) break;
                next_token();
            }
            next_token();
            if (current_token.type == TOKEN_LBRACE && ndefined < 1024) strcpy(defined_names[ndefined++], name);
            continue;
        }
        next_token();
    }
    restore_lex(saved);
}

/* declaration of a function (prototype, definition header, or local prototype) */
static Func *declare_function(Type ft, const char *name) {
    Sig *g = &sigs[func_sig(ft)];
    Type ret = g->ret;
    if (is_func(ret) || is_array(ret)) error("function '%s' returns a %s", name, is_func(ret) ? "function" : "array");
    Func *f = find_func(name);
    if (find_global(name)) error("'%s' redeclared as a function", name);
    if (!f) {
        if (nfuncs >= 1024) error("too many functions");
        f = &funcs[nfuncs++];
        memset(f, 0, sizeof *f);
        snprintf(f->name, sizeof f->name, "%s", name);
        f->ret = ret;
        f->sig = -1;
        f->is_import = !is_defined_in_file(name);
        f->unprototyped = 1;
        f->np = 0;
    } else if (!same_type(f->ret, ret)) {
        error("conflicting return type for '%s'", name);
    }
    if (!g->unprototyped) {
        if (!f->unprototyped) {
            if (f->np != g->np || f->vararg != g->vararg) error("conflicting declaration of '%s'", name);
            for (int i = 0; i < f->np; i++)
                if (!same_type(f->pt[i], g->pt[i])) error("conflicting types for parameter %d of '%s'", i + 1, name);
        }
        f->unprototyped = 0;
        f->np = g->np;
        f->vararg = g->vararg;
        memcpy(f->pt, g->pt, sizeof f->pt);
        memcpy(f->pn, g->pn, sizeof f->pn);   /* the latest declaration's names (the definition's) win */
        if (g->pdef) {                         /* defaults: from whichever declaration gave them */
            f->pdef = g->pdef;
            f->un = cc_malloc(sizeof *f->un * MAX_PARAMS);
            memcpy(f->un, g->pn, sizeof *f->un * MAX_PARAMS);
        }
        if (g->plen) f->plen = g->plen;
    }
    return f;
}

static void flush_pending_inits(void);

/* function body; current token is '{' */
static void define_function(Func *f) {
    if (f->defined) error("redefinition of '%s'", f->name);
    if (strcmp(f->name, "main") == 0) be_simple_item("export", "main");
    f->unprototyped = 0;

    /* a parameter's MIR name must not collide with a module item (see reg_exists) */
    char user_pn[MAX_PARAMS][32];
    memcpy(user_pn, f->pn, sizeof user_pn);
    for (int i = 0; i < f->np; i++)
        if (!f->pn[i][0] || name_is_item(f->pn[i]) || strcmp(f->pn[i], "_ret") == 0)
            snprintf(f->pn[i], 32, "%.20s_arg%d", f->pn[i][0] ? user_pn[i] : "p", i & 31);

    prescan_addr_taken(src);
    prescan_whole_reads();
    f->defined = 1;                                    /* recursion: no forward needed */
    f->is_import = 0;
    be_func_begin(f);
    cur_func = f;
    nvars = 0;
    nglabels = 0;
    scope_depth = 1;
    loop_depth = 0;
    sw_depth = 0;
    MemT rq[2];
    cur_sret_reg = is_struct_val(f->ret) && abi_ret_regs(f->ret, rq) == 0 ? new_reg("_ret", RK_I, 1) : -1;
    for (int i = 0; i < f->np; i++) {
        int preg = new_reg(f->pn[i], reg_kind(f->pt[i]), 1);
        tail_param_reg[i] = preg;
        if (is_integer(f->pt[i]) && int_width(f->pt[i]) < 32) {
            /* callers from C (qsort, callbacks) leave the upper bits of narrow ints undefined;
               int/unsigned are fine: they are treated as possibly dirty anyway */
            Val nv = normalize(rv_reg(preg, f->pt[i]), f->pt[i]);
            EMIT(MOV, R(preg), nv.op);
        }
        if (!user_pn[i][0]) continue;                  /* unnamed parameter */
        Var *v;
        if (is_struct_val(f->pt[i])) {                 /* by value: the reg points at our own copy */
            v = &vars[nvars++]; memset(v, 0, sizeof *v);
            v->is_mem = 1;
            v->reg = preg;
        } else if (is_addr_taken(user_pn[i])) {        /* spill an address-taken parameter to the stack */
            v = declare_var(user_pn[i], f->pt[i]);
            Opnd spill[2] = {MEM(mem_type(f->pt[i]), 0, v->reg), R(preg)};
            emit_ops(1, mov_insn(reg_kind(f->pt[i])), spill, 2);
            continue;
        } else {
            v = &vars[nvars++]; memset(v, 0, sizeof *v);
            v->is_mem = 0;
            v->reg = preg;
        }
        strcpy(v->name, user_pn[i]);
        v->ty = f->pt[i];
        v->depth = scope_depth;
        v->global = -1;
    }
    hook_line = 0;
    emit_hook("__on_enter");
    tail_label = new_label();                          /* self tail calls jump here */
    place_label(tail_label);
    expect(TOKEN_LBRACE);
    while (current_token.type != TOKEN_RBRACE && current_token.type != TOKEN_EOF) parse_statement();
    expect(TOKEN_RBRACE);
    for (int i = 0; i < nglabels; i++)
        if (!glabels[i].defined) { cur_line = glabels[i].line; error("label '%s' used but not defined", glabels[i].name); }
    emit_return(NULL);                                 /* falling off the end */
    tail_label = -1;
    be_func_end();
    hide_structs(1);                                   /* struct tags declared in the body */
    while (nfunc_aliases > 0 && func_aliases[nfunc_aliases - 1].depth >= 1) nfunc_aliases--;
    scope_depth = 0;
    cur_func = NULL;
    flush_pending_inits();
}

/* global with a non-constant or aggregate initializer: a small init function the driver runs
   before main.  Current token: first token of the initializer. */
static int ginit_counter;

static void emit_global_init(Global *g) {
    if (nfuncs >= 1024) error("too many functions");
    Func *f = &funcs[nfuncs++];
    memset(f, 0, sizeof *f);
    snprintf(f->name, sizeof f->name, "__c2_ginit%d", ginit_counter++);
    f->ret = mk_type(TY_VOID);
    f->defined = 1;
    f->sig = -1;
    naddr_taken = 0;
    be_func_begin(f);
    cur_func = f;
    nvars = 0;
    if (ginit_with_statics)
        for (int i = 0; i < nstatic_vars; i++) { vars[nvars] = static_vars[i]; vars[nvars++].depth = 1; }
    scope_depth = 1;
    loop_depth = 0;
    cur_sret_reg = -1;
    int a = new_temp(RK_I);
    EMIT(MOV, R(a), REF(g->name));
    if (is_aggregate(g->ty)) init_object(a, 0, g->ty, 1);     /* storage is bss: already zero */
    else if (current_token.type == TOKEN_LBRACE) {
        next_token();
        store_ex(lv_mem(a, 0, g->ty), parse_assign(), 1);
        if (current_token.type == TOKEN_COMMA) next_token();
        expect(TOKEN_RBRACE);
    } else {
        store_ex(lv_mem(a, 0, g->ty), parse_assign(), 1);
    }
    emit_ops(0, I_RET, NULL, 0);
    be_func_end();
    scope_depth = 0;
    cur_func = NULL;
}

static void flush_pending_inits(void) {
    LexState here = save_lex();
    for (int i = 0; i < npending_inits; i++) {
        restore_lex(pending_inits[i].init);
        ginit_with_statics = 1;                        /* `static int *p = &f;` sees the static f */
        emit_global_init(&globals[pending_inits[i].g]);
        ginit_with_statics = 0;
    }
    npending_inits = 0;
    nstatic_vars = 0;
    restore_lex(here);
}

/* K&R definition: `int f(a, b) int a; char *b; { ... }`: the declarations between the declarator and
   the body give the parameters' types (int when left out).  Callers pass promoted values, so a float
   parameter is received as a double.  Without a body it is just an unprototyped declaration. */
static void knr_param_decls(Sig *g) {
    if (current_token.type != TOKEN_LBRACE && !is_type_start(current_token)) {   /* int f(a, b); */
        g->knr = 0;
        g->unprototyped = 1;
        g->np = 0;
        return;
    }
    while (current_token.type != TOKEN_LBRACE) {
        Type base = parse_type_spec();
        for (;;) {
            char pname[32];
            Type pt = parse_declarator(base, pname, 0);
            int i = 0;
            while (i < g->np && strcmp(g->pn[i], pname)) i++;
            if (i == g->np) error("'%s' is not a parameter", pname);
            if (is_array(pt)) pt = ptr_to(elem_of(pt));
            else if (is_func(pt)) pt = ptr_to(pt);
            else if (is_float(pt)) pt = mk_type(TY_DOUBLE);
            if (is_void(pt)) error("parameter of type void");
            g->pt[i] = pt;
            if (current_token.type != TOKEN_COMMA) break;
            next_token();
        }
        expect(TOKEN_SEMI);
    }
    g->knr = 0;
}

static void parse_toplevel(void) {
    if (current_token.type == TOKEN_TYPEDEF) { parse_typedef(); return; }
    if (current_token.type == TOKEN_STATIC_ASSERT) { parse_static_assert(); return; }
    if (current_token.type == TOKEN_SEMI) { next_token(); return; }
    Type base;
    if (current_token.type == TOKEN_IDENT && !is_type_start(current_token) && peek_token().type == TOKEN_LPAREN) {
        base = int_type();                             /* implicit int: main() { ... } */
        spec_static = 0;
    } else {
        base = parse_type_spec();                      /* `static` at file scope: one module, nothing to do */
        if (spec_typedef) { parse_typedef_names(base); return; }
    }
    if (current_token.type == TOKEN_SEMI) { next_token(); return; }   /* struct/enum definition */
    int cx = spec_constexpr;
    for (;;) {
        char name[32];
        Type t = parse_declarator(base, name, 0);
        spec_constexpr = cx;
        if (constexpr_decl(t, name)) {
        } else if (is_func(t)) {
            if (sigs[func_sig(t)].knr) knr_param_decls(&sigs[func_sig(t)]);
            Func *f = declare_function(t, name);
            if (current_token.type == TOKEN_LBRACE) { define_function(f); flush_nested(); return; }
        } else {
            if (is_void(t)) error("variable '%s' declared void", name);
            if (find_func(name)) error("'%s' redeclared as a variable", name);
            int has_init = current_token.type == TOKEN_ASSIGN;
            if (is_array(t) && arr_len(t) < 0) {
                if (!has_init) error("array '%s' needs a size or an initializer", name);
                t = set_arr_len(t, count_initializer(elem_of(t)));
            }
            Global *g = find_global(name);
            if (is_struct_val(t) && !structs[t.sidx].complete && !(spec_extern && !is_var_defined_in_file(name))) {
                if (has_init) error("variable '%s' has incomplete type", name);
                if (!g) { g = add_global(name, t); g->deferred = 1; }   /* tentative: wait for the struct */
                if (current_token.type != TOKEN_COMMA) break;
                next_token();
                continue;
            }
            if (!g && spec_extern && !has_init && !is_var_defined_in_file(name)) {
                g = add_global(name, t);               /* defined by the host or a library: import */
                g->has_init = 1;
                be_simple_item("import", name);
            } else if (g) {                            /* tentative definitions: int x; int x = 1; extern int x; */
                if (!same_type(g->ty, t)) error("conflicting types for '%s'", name);
                if (has_init) {
                    if (g->has_init) error("redefinition of '%s'", name);
                    g->has_init = 1;
                    next_token();
                    emit_global_init(g);               /* storage exists: store the value before main */
                }
            } else {
                define_global_storage(add_global(name, t), 0);
            }
        }
        if (current_token.type != TOKEN_COMMA) break;
        next_token();
    }
    expect(TOKEN_SEMI);
}

/* ============================================================ C output (c2 -C): no JIT at all
 * The MIR module is translated to portable C, to be compiled by the platform's own compiler --
 * for targets that forbid generating code at run time (iOS, consoles).  Every MIR register is a
 * C local; memory operands are casts; branches are gotos.  Structs passed by value keep the ABI
 * classification C² made (see abi_regs), so the output is for targets with the ABI of the
 * machine that ran C².  Script globals and functions keep their names (external linkage);
 * c2_init() runs the global initializers; a script main() becomes the program's main(). */
#ifndef NO_MIR
static Buf c_hdr, c_out;
static void *builtin_symbol(const char *name);
/* C source of the runtime pieces generated programs need (the wrappers of the runtime below) */
static const char *c_runtime =
"/* c2_rt.c: runtime for programs generated by c2 -C (c2 -Crt > c2_rt.c) */\n"
"#include <stdint.h>\n"
"#include <string.h>\n"
"#include <stdio.h>\n"
"#include <stdlib.h>\n"
"#include <math.h>\n"
"#include <ctype.h>\n"
"#include <time.h>\n"
"#include <errno.h>\n"
"#include <signal.h>\n"
"#include <locale.h>\n"
"#include <wchar.h>\n"
"#ifdef _WIN32\n"
"__declspec(dllimport) void *__stdcall LoadLibraryA(const char *);\n"
"__declspec(dllimport) void *__stdcall GetProcAddress(void *, const char *);\n"
"__declspec(dllimport) int __stdcall FreeLibrary(void *);\n"
"#else\n"
"#include <dlfcn.h>\n"
"#endif\n"
"static FILE *c2_w___c2_stdin(void) { return stdin; }\n"
"static FILE *c2_w___c2_stdout(void) { return stdout; }\n"
"static FILE *c2_w___c2_stderr(void) { return stderr; }\n"
"static void *c2_w___c2_memset(void *p, int c, size_t n) { return memset(p, c, n); }\n"
"static int64_t c2_w___c2_clocks_per_sec(void) { return (int64_t)CLOCKS_PER_SEC; }\n"
"static int64_t c2_w_clock(void) { return (int64_t)clock(); }\n"
"static int64_t c2_w_time(int64_t *t) { int64_t r = (int64_t)time(NULL); if (t) *t = r; return r; }\n"
"static int64_t c2_w_atol(const char *s) { return (int64_t)strtoll(s, NULL, 10); }\n"
"static int64_t c2_w_strtol(const char *s, char **e, int b) { return (int64_t)strtoll(s, e, b); }\n"
"static uint64_t c2_w_strtoul(const char *s, char **e, int b) { return (uint64_t)strtoull(s, e, b); }\n"
"static int64_t c2_w_labs(int64_t x) { return x < 0 ? -x : x; }\n"
"static double c2_w___c2_huge_val(void) { return HUGE_VAL; }\n"
"static float c2_w___c2_huge_valf(void) { return (float)INFINITY; }\n"
"static float c2_w___c2_nanf(void) { return (float)NAN; }\n"
"static void c2_w___c2_assert_fail(const char *e, const char *f, int l) {\n"
"    fflush(stdout); fprintf(stderr, \"%s:%d: assertion failed: %s\\n\", f, l, e); abort();\n"
"}\n"
"static int c2_w_putc(int c, FILE *f) { return fputc(c, f); }\n"
"static int c2_w_getc(FILE *f) { return fgetc(f); }\n"
"#ifdef _WIN32\n"
"static int c2_w_fseek(FILE *f, int64_t o, int w) { return _fseeki64(f, o, w); }\n"
"static int64_t c2_w_ftell(FILE *f) { return _ftelli64(f); }\n"
"static char *c2_w_strdup(const char *s) { return _strdup(s); }\n"
"static void *c2_w_dlopen(const char *p, int fl) { (void)fl; return LoadLibraryA(p); }\n"
"static void *c2_w_dlsym(void *h, const char *n) { return GetProcAddress(h, n); }\n"
"static int c2_w_dlclose(void *h) { return FreeLibrary(h) ? 0 : -1; }\n"
"static char *c2_w_dlerror(void) { return NULL; }\n"
"#else\n"
"static int c2_w_fseek(FILE *f, int64_t o, int w) { return fseek(f, (long)o, w); }\n"
"static int64_t c2_w_ftell(FILE *f) { return (int64_t)ftell(f); }\n"
"static char *c2_w_strdup(const char *s) { size_t n = strlen(s) + 1; char *p = malloc(n); if (p) memcpy(p, s, n); return p; }\n"
"static void *c2_w_dlopen(const char *p, int fl) { return dlopen(p, fl); }\n"
"static void *c2_w_dlsym(void *h, const char *n) { return dlsym(h, n); }\n"
"static int c2_w_dlclose(void *h) { return dlclose(h); }\n"
"static char *c2_w_dlerror(void) { return dlerror(); }\n"
"#endif\n"
"static double c2_w_strtold(const char *s, char **e) { return strtod(s, e); }\n"
"typedef struct { long long quot, rem; } c2_ldiv_t;\n"
"static c2_ldiv_t c2_w_ldiv(long long a, long long b) { lldiv_t d = lldiv(a, b); c2_ldiv_t r = {d.quot, d.rem}; return r; }\n"
"static double c2_w_scalbln(double x, int64_t n) { return scalbn(x, n > 2147483647 ? 2147483647 : n < -2147483647 ? -2147483647 : (int)n); }\n"
"static int64_t c2_w_lrint(double x) { return (int64_t)llrint(x); }\n"
"static int64_t c2_w_lround(double x) { return (int64_t)llround(x); }\n"
"static double c2_w_nexttoward(double x, double y) { return nextafter(x, y); }\n"
"static int c2_w___c2_signbit(double x) { return signbit(x) != 0; }\n"
"static int c2_w___c2_mb_cur_max(void) { return (int)MB_CUR_MAX; }\n"
"struct c2_timespec { int64_t tv_sec, tv_nsec; };\n"
"static int c2_w_timespec_get(struct c2_timespec *ts, int base) {\n"
"    struct timespec t; int r = timespec_get(&t, base);\n"
"    if (r) { ts->tv_sec = (int64_t)t.tv_sec; ts->tv_nsec = (int64_t)t.tv_nsec; }\n"
"    return r;\n"
"}\n"
"static int *c2_w___c2_errno(void) { return &errno; }\n"
"static int64_t c2_w_imaxabs(int64_t x) { return x < 0 ? -x : x; }\n"
"static int64_t c2_w_strtoimax(const char *s, char **e, int b) { return (int64_t)strtoll(s, e, b); }\n"
"static uint64_t c2_w_strtoumax(const char *s, char **e, int b) { return (uint64_t)strtoull(s, e, b); }\n"
"\n"
;
static const char *c_runtime_table =
"void *c2_rt(const char *name) {   /* name -> address, like c2's built-in C library */\n"
"    const struct { const char *n; void *a; } tab[] = {\n"
"        {\"printf\", (void *)printf},\n"
"        {\"fprintf\", (void *)fprintf},\n"
"        {\"sprintf\", (void *)sprintf},\n"
"        {\"snprintf\", (void *)snprintf},\n"
"        {\"vprintf\", (void *)vprintf},\n"
"        {\"vfprintf\", (void *)vfprintf},\n"
"        {\"vsprintf\", (void *)vsprintf},\n"
"        {\"vsnprintf\", (void *)vsnprintf},\n"
"        {\"perror\", (void *)perror},\n"
"        {\"scanf\", (void *)scanf},\n"
"        {\"sscanf\", (void *)sscanf},\n"
"        {\"puts\", (void *)puts},\n"
"        {\"putchar\", (void *)putchar},\n"
"        {\"getchar\", (void *)getchar},\n"
"        {\"fopen\", (void *)fopen},\n"
"        {\"fclose\", (void *)fclose},\n"
"        {\"fflush\", (void *)fflush},\n"
"        {\"fread\", (void *)fread},\n"
"        {\"fwrite\", (void *)fwrite},\n"
"        {\"fgets\", (void *)fgets},\n"
"        {\"fputs\", (void *)fputs},\n"
"        {\"fputc\", (void *)fputc},\n"
"        {\"fgetc\", (void *)fgetc},\n"
"        {\"feof\", (void *)feof},\n"
"        {\"remove\", (void *)remove},\n"
"        {\"rename\", (void *)rename},\n"
"        {\"malloc\", (void *)malloc},\n"
"        {\"calloc\", (void *)calloc},\n"
"        {\"realloc\", (void *)realloc},\n"
"        {\"free\", (void *)free},\n"
"        {\"exit\", (void *)exit},\n"
"        {\"abort\", (void *)abort},\n"
"        {\"atoi\", (void *)atoi},\n"
"        {\"atof\", (void *)atof},\n"
"        {\"strtod\", (void *)strtod},\n"
"        {\"abs\", (void *)abs},\n"
"        {\"rand\", (void *)rand},\n"
"        {\"srand\", (void *)srand},\n"
"        {\"getenv\", (void *)getenv},\n"
"        {\"qsort\", (void *)qsort},\n"
"        {\"bsearch\", (void *)bsearch},\n"
"        {\"atexit\", (void *)atexit},\n"
"        {\"strlen\", (void *)strlen},\n"
"        {\"strcmp\", (void *)strcmp},\n"
"        {\"strncmp\", (void *)strncmp},\n"
"        {\"strcpy\", (void *)strcpy},\n"
"        {\"strncpy\", (void *)strncpy},\n"
"        {\"strcat\", (void *)strcat},\n"
"        {\"strncat\", (void *)strncat},\n"
"        {\"strchr\", (void *)strchr},\n"
"        {\"strrchr\", (void *)strrchr},\n"
"        {\"strstr\", (void *)strstr},\n"
"        {\"memset\", (void *)memset},\n"
"        {\"memcpy\", (void *)memcpy},\n"
"        {\"memmove\", (void *)memmove},\n"
"        {\"memcmp\", (void *)memcmp},\n"
"        {\"sqrt\", (void *)sqrt},\n"
"        {\"sin\", (void *)sin},\n"
"        {\"cos\", (void *)cos},\n"
"        {\"tan\", (void *)tan},\n"
"        {\"asin\", (void *)asin},\n"
"        {\"acos\", (void *)acos},\n"
"        {\"atan\", (void *)atan},\n"
"        {\"atan2\", (void *)atan2},\n"
"        {\"exp\", (void *)exp},\n"
"        {\"log\", (void *)log},\n"
"        {\"log10\", (void *)log10},\n"
"        {\"log2\", (void *)log2},\n"
"        {\"pow\", (void *)pow},\n"
"        {\"fabs\", (void *)fabs},\n"
"        {\"floor\", (void *)floor},\n"
"        {\"ceil\", (void *)ceil},\n"
"        {\"round\", (void *)round},\n"
"        {\"trunc\", (void *)trunc},\n"
"        {\"fmod\", (void *)fmod},\n"
"        {\"hypot\", (void *)hypot},\n"
"        {\"fmin\", (void *)fmin},\n"
"        {\"fmax\", (void *)fmax},\n"
"        {\"sqrtf\", (void *)sqrtf},\n"
"        {\"sinf\", (void *)sinf},\n"
"        {\"cosf\", (void *)cosf},\n"
"        {\"tanf\", (void *)tanf},\n"
"        {\"atan2f\", (void *)atan2f},\n"
"        {\"expf\", (void *)expf},\n"
"        {\"logf\", (void *)logf},\n"
"        {\"powf\", (void *)powf},\n"
"        {\"fabsf\", (void *)fabsf},\n"
"        {\"floorf\", (void *)floorf},\n"
"        {\"ceilf\", (void *)ceilf},\n"
"        {\"roundf\", (void *)roundf},\n"
"        {\"fmodf\", (void *)fmodf},\n"
"        {\"isalpha\", (void *)isalpha},\n"
"        {\"isdigit\", (void *)isdigit},\n"
"        {\"isalnum\", (void *)isalnum},\n"
"        {\"isspace\", (void *)isspace},\n"
"        {\"isupper\", (void *)isupper},\n"
"        {\"islower\", (void *)islower},\n"
"        {\"isxdigit\", (void *)isxdigit},\n"
"        {\"ispunct\", (void *)ispunct},\n"
"        {\"toupper\", (void *)toupper},\n"
"        {\"tolower\", (void *)tolower},\n"
"        {\"tmpfile\", (void *)tmpfile},\n"
"        {\"freopen\", (void *)freopen},\n"
"        {\"setbuf\", (void *)setbuf},\n"
"        {\"setvbuf\", (void *)setvbuf},\n"
"        {\"fscanf\", (void *)fscanf},\n"
"        {\"vfscanf\", (void *)vfscanf},\n"
"        {\"vscanf\", (void *)vscanf},\n"
"        {\"vsscanf\", (void *)vsscanf},\n"
"        {\"ungetc\", (void *)ungetc},\n"
"        {\"fgetpos\", (void *)fgetpos},\n"
"        {\"fsetpos\", (void *)fsetpos},\n"
"        {\"rewind\", (void *)rewind},\n"
"        {\"clearerr\", (void *)clearerr},\n"
"        {\"ferror\", (void *)ferror},\n"
"        {\"atoll\", (void *)atoll},\n"
"        {\"strtof\", (void *)strtof},\n"
"        {\"strtoll\", (void *)strtoll},\n"
"        {\"strtoull\", (void *)strtoull},\n"
"        {\"system\", (void *)system},\n"
"        {\"llabs\", (void *)llabs},\n"
"        {\"div\", (void *)div},\n"
"        {\"lldiv\", (void *)lldiv},\n"
"        {\"_Exit\", (void *)_Exit},\n"
"        {\"memchr\", (void *)memchr},\n"
"        {\"strspn\", (void *)strspn},\n"
"        {\"strcspn\", (void *)strcspn},\n"
"        {\"strpbrk\", (void *)strpbrk},\n"
"        {\"strtok\", (void *)strtok},\n"
"        {\"strerror\", (void *)strerror},\n"
"        {\"strcoll\", (void *)strcoll},\n"
"        {\"strxfrm\", (void *)strxfrm},\n"
"        {\"sinh\", (void *)sinh},\n"
"        {\"cosh\", (void *)cosh},\n"
"        {\"tanh\", (void *)tanh},\n"
"        {\"asinh\", (void *)asinh},\n"
"        {\"acosh\", (void *)acosh},\n"
"        {\"atanh\", (void *)atanh},\n"
"        {\"exp2\", (void *)exp2},\n"
"        {\"expm1\", (void *)expm1},\n"
"        {\"log1p\", (void *)log1p},\n"
"        {\"logb\", (void *)logb},\n"
"        {\"ilogb\", (void *)ilogb},\n"
"        {\"frexp\", (void *)frexp},\n"
"        {\"ldexp\", (void *)ldexp},\n"
"        {\"modf\", (void *)modf},\n"
"        {\"scalbn\", (void *)scalbn},\n"
"        {\"cbrt\", (void *)cbrt},\n"
"        {\"erf\", (void *)erf},\n"
"        {\"erfc\", (void *)erfc},\n"
"        {\"lgamma\", (void *)lgamma},\n"
"        {\"tgamma\", (void *)tgamma},\n"
"        {\"nearbyint\", (void *)nearbyint},\n"
"        {\"rint\", (void *)rint},\n"
"        {\"llrint\", (void *)llrint},\n"
"        {\"llround\", (void *)llround},\n"
"        {\"remainder\", (void *)remainder},\n"
"        {\"remquo\", (void *)remquo},\n"
"        {\"copysign\", (void *)copysign},\n"
"        {\"nan\", (void *)nan},\n"
"        {\"nextafter\", (void *)nextafter},\n"
"        {\"fdim\", (void *)fdim},\n"
"        {\"fma\", (void *)fma},\n"
"        {\"asinf\", (void *)asinf},\n"
"        {\"acosf\", (void *)acosf},\n"
"        {\"atanf\", (void *)atanf},\n"
"        {\"exp2f\", (void *)exp2f},\n"
"        {\"log2f\", (void *)log2f},\n"
"        {\"log10f\", (void *)log10f},\n"
"        {\"truncf\", (void *)truncf},\n"
"        {\"hypotf\", (void *)hypotf},\n"
"        {\"fminf\", (void *)fminf},\n"
"        {\"fmaxf\", (void *)fmaxf},\n"
"        {\"cbrtf\", (void *)cbrtf},\n"
"        {\"copysignf\", (void *)copysignf},\n"
"        {\"isblank\", (void *)isblank},\n"
"        {\"iscntrl\", (void *)iscntrl},\n"
"        {\"isgraph\", (void *)isgraph},\n"
"        {\"isprint\", (void *)isprint},\n"
"        {\"difftime\", (void *)difftime},\n"
"        {\"mktime\", (void *)mktime},\n"
"        {\"asctime\", (void *)asctime},\n"
"        {\"ctime\", (void *)ctime},\n"
"        {\"gmtime\", (void *)gmtime},\n"
"        {\"localtime\", (void *)localtime},\n"
"        {\"strftime\", (void *)strftime},\n"
"        {\"signal\", (void *)signal},\n"
"        {\"raise\", (void *)raise},\n"
"        {\"setlocale\", (void *)setlocale},\n"
"        {\"__c2_assert_fail\", (void *)c2_w___c2_assert_fail},\n"
"        {\"__c2_clocks_per_sec\", (void *)c2_w___c2_clocks_per_sec},\n"
"        {\"__c2_errno\", (void *)c2_w___c2_errno},\n"
"        {\"__c2_huge_val\", (void *)c2_w___c2_huge_val},\n"
"        {\"__c2_huge_valf\", (void *)c2_w___c2_huge_valf},\n"
"        {\"__c2_memset\", (void *)c2_w___c2_memset},\n"
"        {\"__c2_nanf\", (void *)c2_w___c2_nanf},\n"
"        {\"__c2_signbit\", (void *)c2_w___c2_signbit},\n"
"        {\"__c2_stderr\", (void *)c2_w___c2_stderr},\n"
"        {\"__c2_stdin\", (void *)c2_w___c2_stdin},\n"
"        {\"__c2_stdout\", (void *)c2_w___c2_stdout},\n"
"        {\"atol\", (void *)c2_w_atol},\n"
"        {\"clock\", (void *)c2_w_clock},\n"
"        {\"dlclose\", (void *)c2_w_dlclose},\n"
"        {\"dlerror\", (void *)c2_w_dlerror},\n"
"        {\"dlopen\", (void *)c2_w_dlopen},\n"
"        {\"dlsym\", (void *)c2_w_dlsym},\n"
"        {\"fseek\", (void *)c2_w_fseek},\n"
"        {\"ftell\", (void *)c2_w_ftell},\n"
"        {\"getc\", (void *)c2_w_getc},\n"
"        {\"imaxabs\", (void *)c2_w_imaxabs},\n"
"        {\"labs\", (void *)c2_w_labs},\n"
"        {\"ldiv\", (void *)c2_w_ldiv},\n"
"        {\"lrint\", (void *)c2_w_lrint},\n"
"        {\"lround\", (void *)c2_w_lround},\n"
"        {\"nexttoward\", (void *)c2_w_nexttoward},\n"
"        {\"putc\", (void *)c2_w_putc},\n"
"        {\"scalbln\", (void *)c2_w_scalbln},\n"
"        {\"strdup\", (void *)c2_w_strdup},\n"
"        {\"strtoimax\", (void *)c2_w_strtoimax},\n"
"        {\"strtol\", (void *)c2_w_strtol},\n"
"        {\"strtold\", (void *)c2_w_strtold},\n"
"        {\"strtoul\", (void *)c2_w_strtoul},\n"
"        {\"strtoumax\", (void *)c2_w_strtoumax},\n"
"        {\"time\", (void *)c2_w_time},\n"
"        {\"timespec_get\", (void *)c2_w_timespec_get},\n"
"        {\"mblen\", (void *)mblen},\n"
"        {\"mbtowc\", (void *)mbtowc},\n"
"        {\"wctomb\", (void *)wctomb},\n"
"        {\"mbstowcs\", (void *)mbstowcs},\n"
"        {\"wcstombs\", (void *)wcstombs},\n"
"        {\"wcslen\", (void *)wcslen},\n"
"        {\"wcscmp\", (void *)wcscmp},\n"
"        {\"wcsncmp\", (void *)wcsncmp},\n"
"        {\"wcscpy\", (void *)wcscpy},\n"
"        {\"wcsncpy\", (void *)wcsncpy},\n"
"        {\"wcscat\", (void *)wcscat},\n"
"        {\"wcsncat\", (void *)wcsncat},\n"
"        {\"wcschr\", (void *)wcschr},\n"
"        {\"wcsrchr\", (void *)wcsrchr},\n"
"        {\"wcsstr\", (void *)wcsstr},\n"
"        {\"wmemcpy\", (void *)wmemcpy},\n"
"        {\"wmemmove\", (void *)wmemmove},\n"
"        {\"wmemset\", (void *)wmemset},\n"
"        {\"wmemcmp\", (void *)wmemcmp},\n"
"        {\"wmemchr\", (void *)wmemchr},\n"
"        {\"wcstod\", (void *)wcstod},\n"
"        {\"wcstoll\", (void *)wcstoll},\n"
"        {\"wcstoull\", (void *)wcstoull},\n"
"        {\"__c2_mb_cur_max\", (void *)c2_w___c2_mb_cur_max},\n"
"    };\n"
"    for (size_t i = 0; i < sizeof tab / sizeof tab[0]; i++) if (!strcmp(tab[i].n, name)) return tab[i].a;\n"
"    fprintf(stderr, \"c2 runtime: unknown function '%s'\\n\", name);\n"
"    abort();\n"
"}\n";
static const char *c_wrapped[] = {"__c2_assert_fail", "__c2_mb_cur_max", "__c2_clocks_per_sec", "__c2_errno", "__c2_huge_val", "__c2_huge_valf", "__c2_memset", "__c2_nanf", "__c2_signbit", "__c2_stderr", "__c2_stdin", "__c2_stdout", "atol", "clock", "dlclose", "dlerror", "dlopen", "dlsym", "fseek", "ftell", "getc", "imaxabs", "labs", "ldiv", "lrint", "lround", "nexttoward", "putc", "scalbln", "strdup", "strtoimax", "strtol", "strtold", "strtoul", "strtoumax", "time", "timespec_get"};
static int c_is_wrapped(const char *n) {
    for (size_t i = 0; i < sizeof c_wrapped / sizeof c_wrapped[0]; i++) if (!strcmp(c_wrapped[i], n)) return 1;
    return 0;
}

static const char *c_mtype(MIR_type_t t) {
    switch (t) {
    case MIR_T_I8: return "int8_t";
    case MIR_T_U8: return "uint8_t";
    case MIR_T_I16: return "int16_t";
    case MIR_T_U16: return "uint16_t";
    case MIR_T_I32: return "int32_t";
    case MIR_T_U32: return "uint32_t";
    case MIR_T_F: return "float";
    case MIR_T_D: return "double";
    default: return "int64_t";
    }
}
static const char *c_rtype(MIR_type_t t) { return t == MIR_T_F ? "float" : t == MIR_T_D ? "double" : "int64_t"; }
static int c_tsize(MIR_type_t t) {
    switch (t) {
    case MIR_T_I8: case MIR_T_U8: return 1;
    case MIR_T_I16: case MIR_T_U16: return 2;
    case MIR_T_I32: case MIR_T_U32: case MIR_T_F: return 4;
    default: return 8;
    }
}

/* a struct with the classification of a MIR block type (MIR_T_BLK + k) of `size` bytes */
static char c_blk_done[512][24];
static int c_nblk;
static const char *c_blk(MIR_type_t t, size_t size) {
    static char name[24];
    int k = (int)(t - MIR_T_BLK);
    snprintf(name, sizeof name, "__blk%d_%u", k, (unsigned)size);
    for (int i = 0; i < c_nblk; i++) if (!strcmp(c_blk_done[i], name)) return name;
    if (c_nblk < 512) strcpy(c_blk_done[c_nblk++], name);
    buf_emit(&c_hdr, "typedef struct { ");
    size_t n = (size + 7) / 8;
    if (k == 0 || n > 2) buf_emit(&c_hdr, "int64_t q[%u]; ", (unsigned)n);   /* memory class */
    else
        for (size_t i = 0; i < n; i++) {
            int fp = k == 2 || (k == 3 && i == 1) || (k == 4 && i == 0);
            size_t rest = size - 8 * i;
            if (fp) buf_emit(&c_hdr, "%s q%u; ", rest <= 4 ? "float" : "double", (unsigned)i);
            else buf_emit(&c_hdr, "%s q%u; ", rest == 1 ? "int8_t" : rest == 2 ? "int16_t" : rest <= 4 ? "int32_t" : "int64_t", (unsigned)i);
        }
    buf_emit(&c_hdr, "} %s;\n", name);
    return name;
}

static const char *c_item_name(MIR_item_t it) {
    if (it->item_type == MIR_func_item) return it->u.func->name;
    if (it->item_type == MIR_proto_item) return it->u.proto->name;
    if (it->item_type == MIR_import_item) return it->u.import_id;
    if (it->item_type == MIR_export_item) return it->u.export_id;
    if (it->item_type == MIR_forward_item) return it->u.forward_id;
    return MIR_item_name(ctx, it);
}
static int c_is_main_renamed;
static void c_item_ref(Buf *b, MIR_item_t it) {
    const char *n = c_item_name(it);
    if (it->item_type == MIR_import_item) { buf_emit(b, "(int64_t)(intptr_t)c2_imp_%s", n); return; }
    if (c_is_main_renamed && !strcmp(n, "main")) n = "__c2_main";
    buf_emit(b, "(int64_t)(intptr_t)&%s", n);
}

/* result type and parameter list of a function or proto */
static void c_signature(Buf *b, const char *name, const char *rname, uint32_t nres, MIR_type_t *res,
                        VARR(MIR_var_t) *args, int vararg, int with_names, int as_ptr_type) {
    size_t nargs = args ? VARR_LENGTH(MIR_var_t, args) : 0;
    MIR_var_t *v = args ? VARR_ADDR(MIR_var_t, args) : NULL;
    int rblk = nargs > 0 && v[0].type == MIR_T_RBLK;
    if (nres == 2) buf_emit(b, "%s ", rname);
    else if (nres == 1) buf_emit(b, "%s ", c_mtype(res[0]) [0] == 'u' || res[0] == MIR_T_I32 ? c_mtype(res[0]) : c_rtype(res[0]));
    else buf_emit(b, rblk ? "void *" : "void ");
    if (as_ptr_type) buf_emit(b, "(*%s)(", name); else buf_emit(b, "%s(", name);
    if (nargs == 0) buf_emit(b, vararg ? ")" : "void)");
    for (size_t i = 0; i < nargs; i++) {
        const char *ty = MIR_blk_type_p(v[i].type) ? c_blk(v[i].type, v[i].size)
                         : v[i].type == MIR_T_RBLK ? "void *" : c_mtype(v[i].type);
        if (MIR_blk_type_p(v[i].type)) ty = c_blk(v[i].type, v[i].size);
        buf_emit(b, "%s%s", i ? ", " : "", ty);
        if (with_names) buf_emit(b, " p_%s", v[i].name);
        if (i + 1 == nargs) buf_emit(b, vararg ? ", ...)" : ")");
    }
}
static void c_ret_struct(const char *rname, uint32_t nres, MIR_type_t *res) {
    if (nres == 2) buf_emit(&c_hdr, "typedef struct { %s r0; %s r1; } %s;\n", c_rtype(res[0]), c_rtype(res[1]), rname);
}

static void c_reg(Buf *b, MIR_func_t fn, MIR_reg_t r) { buf_emit(b, "r_%s", MIR_reg_name(ctx, r, fn)); }

static void c_val(Buf *b, MIR_func_t fn, MIR_op_t *op) {   /* an operand as a C expression */
    union { float f; uint32_t u; } fu;
    union { double d; uint64_t u; } du;
    switch (op->mode) {
    case MIR_OP_REG: c_reg(b, fn, op->u.reg); break;
    case MIR_OP_INT:
        if (op->u.i == INT64_MIN) buf_emit(b, "INT64_MIN");
        else buf_emit(b, "INT64_C(%lld)", (long long)op->u.i);
        break;
    case MIR_OP_UINT: buf_emit(b, "(int64_t)UINT64_C(%llu)", (unsigned long long)op->u.u); break;
    case MIR_OP_FLOAT: fu.f = op->u.f; buf_emit(b, "c2_f32(0x%08xu)", (unsigned)fu.u); break;
    case MIR_OP_DOUBLE: du.d = op->u.d; buf_emit(b, "c2_f64(UINT64_C(0x%016llx))", (unsigned long long)du.u); break;
    case MIR_OP_REF: c_item_ref(b, op->u.ref); break;
    case MIR_OP_MEM:
        buf_emit(b, "(*(%s *)(intptr_t)(", c_mtype(op->u.mem.type));
        if (op->u.mem.base) c_reg(b, fn, op->u.mem.base); else buf_emit(b, "0");
        if (op->u.mem.index) { buf_emit(b, " + "); c_reg(b, fn, op->u.mem.index); buf_emit(b, " * %d", op->u.mem.scale); }
        if (op->u.mem.disp) buf_emit(b, " + %lld", (long long)op->u.mem.disp);
        buf_emit(b, "))");
        break;
    default: error("C output: unsupported operand");
    }
}
static void c_label(Buf *b, MIR_op_t *op) { buf_emit(b, "L%d", (int)(intptr_t)op->u.label->data); }

static const char *c_binop(MIR_insn_code_t c, const char **cast) {   /* integer and fp binary ops */
    *cast = "";
    switch (c) {
    case MIR_ADD: case MIR_FADD: case MIR_DADD: case MIR_ADDS: return "+";
    case MIR_SUB: case MIR_FSUB: case MIR_DSUB: case MIR_SUBS: return "-";
    case MIR_MUL: case MIR_FMUL: case MIR_DMUL: case MIR_MULS: return "*";
    case MIR_DIV: case MIR_FDIV: case MIR_DDIV: case MIR_DIVS: case MIR_UDIV: case MIR_UDIVS: return "/";
    case MIR_MOD: case MIR_MODS: case MIR_UMOD: case MIR_UMODS: return "%";
    case MIR_AND: case MIR_ANDS: return "&";
    case MIR_OR: case MIR_ORS: return "|";
    case MIR_XOR: case MIR_XORS: return "^";
    case MIR_EQ: case MIR_EQS: case MIR_FEQ: case MIR_DEQ: case MIR_BEQ: case MIR_BEQS: case MIR_FBEQ: case MIR_DBEQ: return "==";
    case MIR_NE: case MIR_NES: case MIR_FNE: case MIR_DNE: case MIR_BNE: case MIR_BNES: case MIR_FBNE: case MIR_DBNE: return "!=";
    case MIR_LT: case MIR_LTS: case MIR_ULT: case MIR_ULTS: case MIR_FLT: case MIR_DLT:
    case MIR_BLT: case MIR_BLTS: case MIR_UBLT: case MIR_UBLTS: case MIR_FBLT: case MIR_DBLT: return "<";
    case MIR_LE: case MIR_LES: case MIR_ULE: case MIR_ULES: case MIR_FLE: case MIR_DLE:
    case MIR_BLE: case MIR_BLES: case MIR_UBLE: case MIR_UBLES: case MIR_FBLE: case MIR_DBLE: return "<=";
    case MIR_GT: case MIR_GTS: case MIR_UGT: case MIR_UGTS: case MIR_FGT: case MIR_DGT:
    case MIR_BGT: case MIR_BGTS: case MIR_UBGT: case MIR_UBGTS: case MIR_FBGT: case MIR_DBGT: return ">";
    case MIR_GE: case MIR_GES: case MIR_UGE: case MIR_UGES: case MIR_FGE: case MIR_DGE:
    case MIR_BGE: case MIR_BGES: case MIR_UBGE: case MIR_UBGES: case MIR_FBGE: case MIR_DBGE: return ">=";
    default: return NULL;
    }
}
static const char *c_opcast(MIR_insn_code_t c) {   /* how integer operands are viewed */
    switch (c) {
    case MIR_ADD: case MIR_SUB: case MIR_MUL: case MIR_AND: case MIR_OR: case MIR_XOR:
    case MIR_UDIV: case MIR_UMOD: case MIR_ULT: case MIR_ULE: case MIR_UGT: case MIR_UGE:
    case MIR_UBLT: case MIR_UBLE: case MIR_UBGT: case MIR_UBGE: return "(uint64_t)";
    case MIR_ADDS: case MIR_SUBS: case MIR_MULS: case MIR_ANDS: case MIR_ORS: case MIR_XORS:
    case MIR_UDIVS: case MIR_UMODS: case MIR_ULTS: case MIR_ULES: case MIR_UGTS: case MIR_UGES:
    case MIR_UBLTS: case MIR_UBLES: case MIR_UBGTS: case MIR_UBGES: return "(uint32_t)";
    case MIR_DIVS: case MIR_MODS: case MIR_EQS: case MIR_NES: case MIR_LTS: case MIR_LES: case MIR_GTS: case MIR_GES:
    case MIR_BEQS: case MIR_BNES: case MIR_BLTS: case MIR_BLES: case MIR_BGTS: case MIR_BGES: return "(int32_t)";
    default: return "";
    }
}

static MIR_item_t c_resolve(MIR_item_t it) {   /* forward/export items -> the definition */
    if (it->item_type != MIR_forward_item && it->item_type != MIR_export_item) return it;
    const char *n = c_item_name(it);
    for (MIR_item_t j = DLIST_HEAD(MIR_item_t, mir_module->items); j; j = DLIST_NEXT(MIR_item_t, j))
        if (j->item_type == MIR_func_item && !strcmp(j->u.func->name, n)) return j;
    return it;
}

static void c_function(MIR_item_t it) {
    MIR_func_t fn = it->u.func;
    Buf *b = &c_out;
    const char *fname = c_is_main_renamed && !strcmp(fn->name, "main") ? "__c2_main" : fn->name;
    char rname[80];
    snprintf(rname, sizeof rname, "__ret_%s", fname);
    MIR_var_t *v = VARR_ADDR(MIR_var_t, fn->vars);
    VARR(MIR_var_t) *args;
    VARR_CREATE(MIR_var_t, args, MIR_get_alloc(ctx), 8);
    for (uint32_t i = 0; i < fn->nargs; i++) VARR_PUSH(MIR_var_t, args, v[i]);
    c_signature(b, fname, rname, fn->nres, fn->res_types, args, fn->vararg_p, 1, 0);
    VARR_DESTROY(MIR_var_t, args);
    buf_emit(b, " {\n");
    /* registers: every one gets a C local (parameters copy their argument) */
    size_t nregs_used = 0;
    static unsigned char seen[MAX_REGS + 4096];
    memset(seen, 0, sizeof seen);
    int nlab = 0;
    for (MIR_insn_t in = DLIST_HEAD(MIR_insn_t, fn->insns); in; in = DLIST_NEXT(MIR_insn_t, in)) {
        if (in->code == MIR_LABEL) { in->data = (void *)(intptr_t)++nlab; continue; }
        for (unsigned i = 0; i < in->nops; i++) {
            MIR_op_t *o = &in->ops[i];
            MIR_reg_t rs[2] = {0, 0};
            if (o->mode == MIR_OP_REG) rs[0] = o->u.reg;
            else if (o->mode == MIR_OP_MEM) { rs[0] = o->u.mem.base; rs[1] = o->u.mem.index; }
            for (int k = 0; k < 2; k++)
                if (rs[k] && rs[k] < sizeof seen && !seen[rs[k]]) { seen[rs[k]] = 1; nregs_used++; }
        }
    }
    for (uint32_t i = 0; i < fn->nargs; i++) {
        MIR_reg_t r = MIR_reg(ctx, v[i].name, fn);
        if (r < sizeof seen) seen[r] = 2;
        if (MIR_blk_type_p(v[i].type) || v[i].type == MIR_T_RBLK)
            buf_emit(b, "    int64_t r_%s = (int64_t)(intptr_t)%sp_%s;\n", v[i].name, v[i].type == MIR_T_RBLK ? "" : "&", v[i].name);
        else buf_emit(b, "    %s r_%s = p_%s;\n", c_rtype(v[i].type), v[i].name, v[i].name);
    }
    for (MIR_reg_t r = 1; r < sizeof seen; r++)
        if (seen[r] == 1) buf_emit(b, "    %s r_%s = 0;\n", c_rtype(MIR_reg_type(ctx, r, fn)), MIR_reg_name(ctx, r, fn));
    int rblk = fn->nargs > 0 && v[0].type == MIR_T_RBLK, tmp = 0, dyn = 0;
    for (MIR_insn_t in = DLIST_HEAD(MIR_insn_t, fn->insns); in; in = DLIST_NEXT(MIR_insn_t, in))
        if (in->code == MIR_ALLOCA && in->ops[1].mode != MIR_OP_INT) dyn = 1;
    if (dyn) buf_emit(b, "    void *va_head_ = 0;   /* variable-length arrays: heap blocks, freed by bend and return */\n");
    const char *last_param = fn->nargs ? v[fn->nargs - 1].name : NULL;
    for (MIR_insn_t in = DLIST_HEAD(MIR_insn_t, fn->insns); in; in = DLIST_NEXT(MIR_insn_t, in)) {
        MIR_op_t *o = in->ops;
        MIR_insn_code_t c = in->code;
        const char *cast, *op = c_binop(c, &cast);
        if (c == MIR_LABEL) { buf_emit(b, "L%d:;\n", (int)(intptr_t)in->data); continue; }
        buf_emit(b, "    ");
        switch (c) {
        case MIR_MOV: case MIR_FMOV: case MIR_DMOV:
            c_val(b, fn, &o[0]); buf_emit(b, " = "); c_val(b, fn, &o[1]); break;
        case MIR_EXT8: case MIR_EXT16: case MIR_EXT32: case MIR_UEXT8: case MIR_UEXT16: case MIR_UEXT32: {
            static const char *ct[] = {"int8_t", "int16_t", "int32_t", "uint8_t", "uint16_t", "uint32_t"};
            int k = c == MIR_EXT8 ? 0 : c == MIR_EXT16 ? 1 : c == MIR_EXT32 ? 2 : c == MIR_UEXT8 ? 3 : c == MIR_UEXT16 ? 4 : 5;
            c_val(b, fn, &o[0]); buf_emit(b, " = (%s)", ct[k]); c_val(b, fn, &o[1]); break;
        }
        case MIR_I2F: case MIR_I2D: case MIR_UI2F: case MIR_UI2D: case MIR_F2I: case MIR_D2I: case MIR_F2D: case MIR_D2F: {
            const char *to = c == MIR_I2F || c == MIR_UI2F || c == MIR_D2F ? "float" : c == MIR_F2I || c == MIR_D2I ? "int64_t" : "double";
            c_val(b, fn, &o[0]); buf_emit(b, " = (%s)%s", to, c == MIR_UI2F || c == MIR_UI2D ? "(uint64_t)" : "");
            c_val(b, fn, &o[1]); break;
        }
        case MIR_NEG: c_val(b, fn, &o[0]); buf_emit(b, " = (int64_t)(0 - (uint64_t)"); c_val(b, fn, &o[1]); buf_emit(b, ")"); break;
        case MIR_NEGS: c_val(b, fn, &o[0]); buf_emit(b, " = (int32_t)(0u - (uint32_t)"); c_val(b, fn, &o[1]); buf_emit(b, ")"); break;
        case MIR_FNEG: case MIR_DNEG: c_val(b, fn, &o[0]); buf_emit(b, " = -"); c_val(b, fn, &o[1]); break;
        case MIR_LSH: case MIR_RSH: case MIR_URSH: case MIR_LSHS: case MIR_RSHS: case MIR_URSHS: {
            int w32 = c == MIR_LSHS || c == MIR_RSHS || c == MIR_URSHS;
            const char *vc = c == MIR_LSH || c == MIR_URSH ? "(uint64_t)" : c == MIR_RSH ? "(int64_t)"
                             : c == MIR_RSHS ? "(int32_t)" : "(uint32_t)";
            c_val(b, fn, &o[0]); buf_emit(b, " = %s(%s", w32 ? "(int32_t)" : "(int64_t)", vc);
            c_val(b, fn, &o[1]); buf_emit(b, " %s (", c == MIR_LSH || c == MIR_LSHS ? "<<" : ">>");
            c_val(b, fn, &o[2]); buf_emit(b, " & %d))", w32 ? 31 : 63); break;
        }
        case MIR_JMP: buf_emit(b, "goto "); c_label(b, &o[0]); break;
        case MIR_BT: case MIR_BF:
            buf_emit(b, "if (%s", c == MIR_BF ? "!" : ""); c_val(b, fn, &o[1]); buf_emit(b, ") goto "); c_label(b, &o[0]); break;
        case MIR_RET:
            if (dyn) buf_emit(b, "c2_vfree(&va_head_, 0); ");
            if (in->nops == 0) buf_emit(b, rblk ? "return (void *)(intptr_t)r_%s" : "return", rblk ? v[0].name : "");
            else if (in->nops == 1) { buf_emit(b, "return "); c_val(b, fn, &o[0]); }
            else { buf_emit(b, "{ %s t_ = {", rname); c_val(b, fn, &o[0]); buf_emit(b, ", "); c_val(b, fn, &o[1]); buf_emit(b, "}; return t_; }"); }
            break;
        case MIR_ALLOCA:
            if (o[1].mode == MIR_OP_INT) {             /* fixed size: a local array */
                buf_emit(b, "int64_t a%d_[%lld]; ", tmp, (long long)((o[1].u.i + 7) / 8 + 1));
                c_val(b, fn, &o[0]); buf_emit(b, " = (int64_t)(intptr_t)(((intptr_t)a%d_ + 15) & ~(intptr_t)15)", tmp++);
            } else {
                c_val(b, fn, &o[0]); buf_emit(b, " = (int64_t)(intptr_t)c2_valloc(&va_head_, (size_t)"); c_val(b, fn, &o[1]); buf_emit(b, ")");
            }
            break;
        case MIR_SWITCH:
            buf_emit(b, "switch ("); c_val(b, fn, &o[0]); buf_emit(b, ") {");
            for (unsigned i = 1; i < in->nops; i++) { buf_emit(b, " case %u: goto ", i - 1); c_label(b, &o[i]); buf_emit(b, ";"); }
            buf_emit(b, " }");
            break;
        case MIR_BSTART:
            if (dyn) { c_val(b, fn, &o[0]); buf_emit(b, " = (int64_t)(intptr_t)va_head_"); }
            break;
        case MIR_BEND:
            if (dyn) { buf_emit(b, "c2_vfree(&va_head_, (void *)(intptr_t)"); c_val(b, fn, &o[0]); buf_emit(b, ")"); }
            break;
        case MIR_VA_START:
            buf_emit(b, "va_start(*(va_list *)(intptr_t)"); c_val(b, fn, &o[0]); buf_emit(b, ", p_%s)", last_param); break;
        case MIR_VA_END: buf_emit(b, "va_end(*(va_list *)(intptr_t)"); c_val(b, fn, &o[0]); buf_emit(b, ")"); break;
        case MIR_VA_ARG: {
            const char *t = c_mtype(o[2].u.mem.type);
            buf_emit(b, "%s v%d_ = va_arg(*(va_list *)(intptr_t)", t, tmp); c_val(b, fn, &o[1]); buf_emit(b, ", %s); ", t);
            c_val(b, fn, &o[0]); buf_emit(b, " = (int64_t)(intptr_t)c2_keep(&v%d_, sizeof v%d_)", tmp, tmp); tmp++;
            break;
        }
        case MIR_CALL: {
            MIR_proto_t p = o[0].u.ref->u.proto;
            char pr[80];
            snprintf(pr, sizeof pr, "__ret_%s", p->name);
            size_t np = p->args ? VARR_LENGTH(MIR_var_t, p->args) : 0;
            MIR_var_t *pv = p->args ? VARR_ADDR(MIR_var_t, p->args) : NULL;
            unsigned first_arg = 2 + p->nres;
            if (p->nres == 1) { c_val(b, fn, &o[2]); buf_emit(b, " = "); }
            else if (p->nres == 2) buf_emit(b, "{ %s t_ = ", pr);
            buf_emit(b, "((__p_%s)(intptr_t)", p->name);
            if (o[1].mode == MIR_OP_REF) c_item_ref(b, c_resolve(o[1].u.ref)); else c_val(b, fn, &o[1]);
            buf_emit(b, ")(");
            for (unsigned i = first_arg; i < in->nops; i++) {
                size_t ai = i - first_arg;
                if (i > first_arg) buf_emit(b, ", ");
                if (ai < np && MIR_blk_type_p(pv[ai].type)) {
                    buf_emit(b, "*(%s *)(intptr_t)", c_blk(pv[ai].type, pv[ai].size));
                    if (o[i].u.mem.base) c_reg(b, fn, o[i].u.mem.base); else buf_emit(b, "0");
                } else if (ai < np && pv[ai].type == MIR_T_RBLK) {
                    buf_emit(b, "(void *)(intptr_t)");
                    if (o[i].u.mem.base) c_reg(b, fn, o[i].u.mem.base); else buf_emit(b, "0");
                } else c_val(b, fn, &o[i]);
            }
            buf_emit(b, ")");
            if (p->nres == 2) {
                buf_emit(b, "; "); c_val(b, fn, &o[2]); buf_emit(b, " = t_.r0; "); c_val(b, fn, &o[3]); buf_emit(b, " = t_.r1; }");
            }
            break;
        }
        default:
            if (!op) error("C output: unsupported instruction '%s'", MIR_insn_name(ctx, c));
            if (c >= MIR_BEQ && c <= MIR_DBGE) {      /* compare and branch */
                const char *vc = c_opcast(c);
                buf_emit(b, "if (%s", vc); c_val(b, fn, &o[1]); buf_emit(b, " %s %s", op, vc); c_val(b, fn, &o[2]);
                buf_emit(b, ") goto "); c_label(b, &o[0]);
            } else {
                const char *vc = c_opcast(c);
                int w32 = vc[1] && strstr(vc, "32") != NULL;
                int cmp = op[0] == '=' || op[0] == '!' || op[0] == '<' || op[0] == '>';
                c_val(b, fn, &o[0]);
                buf_emit(b, " = %s(%s", cmp ? "" : w32 ? "(int32_t)" : vc[0] ? "(int64_t)" : "", vc); c_val(b, fn, &o[1]);
                buf_emit(b, " %s %s", op, vc); c_val(b, fn, &o[2]); buf_emit(b, ")");
            }
        }
        buf_emit(b, ";\n");
    }
    for (MIR_insn_t in = DLIST_HEAD(MIR_insn_t, fn->insns); in; in = DLIST_NEXT(MIR_insn_t, in))
        if (in->code == MIR_LABEL) in->data = NULL;
    (void)nregs_used;
    buf_emit(b, "}\n\n");
}

static void c_print_runtime(FILE *f) { fputs(c_runtime, f); fputs(c_runtime_table, f); }

static void c_module(MIR_module_t m, FILE *f) {
    memset(&c_hdr, 0, sizeof c_hdr);
    memset(&c_out, 0, sizeof c_out);
    c_nblk = 0;
    c_is_main_renamed = 0;
    for (MIR_item_t it = DLIST_HEAD(MIR_item_t, m->items); it; it = DLIST_NEXT(MIR_item_t, it))
        if (it->item_type == MIR_func_item && !strcmp(it->u.func->name, "main")) c_is_main_renamed = 1;
    /* declarations: protos (as pointer types), imports, data, functions */
    for (MIR_item_t it = DLIST_HEAD(MIR_item_t, m->items); it; it = DLIST_NEXT(MIR_item_t, it)) {
        if (it->item_type == MIR_proto_item) {
            MIR_proto_t p = it->u.proto;
            char rn[80], pn[80];
            snprintf(rn, sizeof rn, "__ret_%s", p->name);
            snprintf(pn, sizeof pn, "__p_%s", p->name);
            c_ret_struct(rn, p->nres, p->res_types);
            Buf t = {0};
            buf_emit(&t, "typedef ");
            c_signature(&t, pn, rn, p->nres, p->res_types, p->args, p->vararg_p, 0, 1);
            buf_emit(&c_hdr, "%s;\n", t.p);
            free(t.p);
        } else if (it->item_type == MIR_import_item) {
            const char *n = it->u.import_id;               /* C library: looked up once in c2_rt.c; host: extern */
            if (c_is_wrapped(n) || builtin_symbol(n))
                buf_emit(&c_hdr, "static void *c2_i_%s;\n#define c2_imp_%s (c2_i_%s ? c2_i_%s : (c2_i_%s = c2_rt(\"%s\")))\n",
                         n, n, n, n, n, n);
            else buf_emit(&c_hdr, "extern char %s[];\n#define c2_imp_%s ((void *)%s)\n", n, n, n);
        } else if (it->item_type == MIR_func_item) {
            MIR_func_t fn = it->u.func;
            const char *n = !strcmp(fn->name, "main") ? "__c2_main" : fn->name;
            char rn[80];
            snprintf(rn, sizeof rn, "__ret_%s", n);
            c_ret_struct(rn, fn->nres, fn->res_types);
            VARR(MIR_var_t) *args;
            VARR_CREATE(MIR_var_t, args, MIR_get_alloc(ctx), 8);
            for (uint32_t i = 0; i < fn->nargs; i++) VARR_PUSH(MIR_var_t, args, VARR_GET(MIR_var_t, fn->vars, i));
            c_signature(&c_out, n, rn, fn->nres, fn->res_types, args, fn->vararg_p, 0, 0);
            VARR_DESTROY(MIR_var_t, args);
            buf_emit(&c_out, ";\n");
        } else if (it->item_type == MIR_data_item || it->item_type == MIR_bss_item || it->item_type == MIR_ref_data_item) {
            buf_emit(&c_out, "extern union u_%s %s;\n", MIR_item_name(ctx, it), MIR_item_name(ctx, it));
        }
    }
    buf_emit(&c_out, "\n");
    for (MIR_item_t it = DLIST_HEAD(MIR_item_t, m->items); it; it = DLIST_NEXT(MIR_item_t, it)) {
        const char *n = MIR_item_name(ctx, it);
        if (it->item_type == MIR_data_item) {
            MIR_data_t d = it->u.data;
            size_t sz = d->nel * c_tsize(d->el_type);
            buf_emit(&c_out, "union u_%s { uint8_t b[%u]; int64_t a_; double d_; } %s = {{", n, (unsigned)(sz ? sz : 1), n);
            for (size_t i = 0; i < sz; i++) buf_emit(&c_out, "%s%u", i ? "," : "", d->u.els[i]);
            buf_emit(&c_out, "%s}};\n", sz ? "" : "0");
        } else if (it->item_type == MIR_bss_item) {
            buf_emit(&c_out, "union u_%s { uint8_t b[%llu]; int64_t a_; double d_; } %s;\n", n,
                     (unsigned long long)it->u.bss->len, n);
        } else if (it->item_type == MIR_ref_data_item) {
            MIR_ref_data_t r = it->u.ref_data;
            buf_emit(&c_out, "union u_%s { void *p; int64_t a_; } %s = {(char *)", n, n);
            buf_emit(&c_out, "&%s + %lld};\n", c_item_name(r->ref_item), (long long)r->disp);
        }
    }
    buf_emit(&c_out, "\n");
    for (MIR_item_t it = DLIST_HEAD(MIR_item_t, m->items); it; it = DLIST_NEXT(MIR_item_t, it))
        if (it->item_type == MIR_func_item) c_function(it);
    buf_emit(&c_out, "void c2_init(void) {\n");
    for (MIR_item_t it = DLIST_HEAD(MIR_item_t, m->items); it; it = DLIST_NEXT(MIR_item_t, it))
        if (it->item_type == MIR_func_item && !strncmp(it->u.func->name, "__c2_ginit", 10))
            buf_emit(&c_out, "    %s();\n", it->u.func->name);
    buf_emit(&c_out, "}\n");
    if (c_is_main_renamed) {
        Func *mf = find_func("main");
        int np = mf && mf->np >= 2;
        buf_emit(&c_out, "\nint main(int argc, char **argv) {\n    c2_init();\n    return (int)__c2_main(%s);\n}\n",
                 np ? "argc, (int64_t)(intptr_t)argv" : "");
        if (!np) buf_emit(&c_out, "/* (argc, argv unused) */\n");
    }
    fputs("/* generated by c2 -C: compile with any C99 compiler, together with c2_rt.c (c2 -Crt) */\n"
          "#include <stdint.h>\n#include <stdarg.h>\n#include <stddef.h>\n"
          "void *c2_rt(const char *name);\nvoid *malloc(size_t n);\nvoid free(void *p);\nvoid abort(void);\n"
          "static void *c2_valloc(void **head, size_t n) {   /* VLA storage, zeroed by the script */\n"
          "    void **p = (void **)malloc(n + 16); if (!p) abort(); *p = *head; *head = p; return (char *)p + 16;\n}\n"
          "static void c2_vfree(void **head, void *mark) {\n"
          "    while (*head && *head != mark) { void *next = *(void **)*head; free(*head); *head = next; }\n}\n"
          "static float c2_f32(uint32_t u) { union { uint32_t u; float f; } x; x.u = u; return x.f; }\n"
          "static double c2_f64(uint64_t u) { union { uint64_t u; double d; } x; x.u = u; return x.d; }\n"
          "static inline void *c2_keep(void *p, size_t n) { (void)n; return p; }\n"
          , f);
    if (c_hdr.p) fputs(c_hdr.p, f);
    fputs("\n", f);
    if (c_out.p) fputs(c_out.p, f);
}
#endif

/* ============================================================ runtime support (host side) */
#ifndef NO_MIR
static FILE *host_stdin(void) { return stdin; }
static FILE *host_stdout(void) { return stdout; }
static FILE *host_stderr(void) { return stderr; }
/* `long` is 64-bit in C² but 32-bit in the Windows C runtime: wrap every long-typed function */
static int64_t host_clocks_per_sec(void) { return (int64_t)CLOCKS_PER_SEC; }
static int64_t host_clock(void) { return (int64_t)clock(); }
static int64_t host_time(int64_t *t) { int64_t r = (int64_t)time(NULL); if (t) *t = r; return r; }
static int64_t host_atol(const char *s) { return (int64_t)strtoll(s, NULL, 10); }
static int64_t host_strtol(const char *s, char **e, int b) { return (int64_t)strtoll(s, e, b); }
static int64_t host_labs(int64_t x) { return x < 0 ? -x : x; }
static double host_huge_val(void) { return HUGE_VAL; }
static void host_assert_fail(const char *expr, const char *file, int line) {
    fflush(stdout);
    fprintf(stderr, "%s:%d: assertion failed: %s\n", file, line, expr);
    abort();
}
static float host_huge_valf(void) { return (float)INFINITY; }
static int *host_errno(void) { return &errno; }
static int host_signbit(double x) { return signbit(x) != 0; }
static int host_mb_cur_max(void) { return (int)MB_CUR_MAX; }
static uint64_t host_strtoul(const char *s, char **e, int b) { return (uint64_t)strtoull(s, e, b); }
static int64_t host_lrint(double x) { return (int64_t)llrint(x); }
static int64_t host_lround(double x) { return (int64_t)llround(x); }
static double host_scalbln(double x, int64_t n) { return scalbn(x, n > INT_MAX ? INT_MAX : n < INT_MIN ? INT_MIN : (int)n); }
struct host_timespec { int64_t tv_sec, tv_nsec; };
static int host_timespec_get(struct host_timespec *ts, int base) {
    struct timespec t;
    int r = timespec_get(&t, base);
    if (r) { ts->tv_sec = (int64_t)t.tv_sec; ts->tv_nsec = (int64_t)t.tv_nsec; }
    return r;
}
static float host_nanf(void) { return (float)NAN; }
#ifdef _WIN32
static int host_fseek(FILE *f, int64_t off, int w) { return _fseeki64(f, off, w); }
static int64_t host_ftell(FILE *f) { return _ftelli64(f); }
#else
static int host_fseek(FILE *f, int64_t off, int w) { return fseeko(f, (off_t)off, w); }
static int64_t host_ftell(FILE *f) { return (int64_t)ftello(f); }
#endif

#ifdef _WIN32
static void *host_dlopen(const char *path, int flags) { (void)flags; return LoadLibraryA(path); }
static void *host_dlsym(void *h, const char *name) { return GetProcAddress(h, name); }
static int host_dlclose(void *h) { return FreeLibrary(h) ? 0 : -1; }
static char *host_dlerror(void) { return NULL; }
#else
static void *host_dlopen(const char *path, int flags) { return dlopen(path, flags); }
static void *host_dlsym(void *h, const char *name) { return dlsym(h, name); }
static int host_dlclose(void *h) { return dlclose(h); }
static char *host_dlerror(void) { return dlerror(); }
#endif

/* built-in C library: name -> address (wrappers where the C ABI differs from ours) */
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"   /* macOS: sprintf, vsprintf (scripts may use them) */
#endif
static void *builtin_symbol(const char *name) {
    /* not static: CRT functions are dllimport with /MD, so MSVC can't use their addresses in a
       static initializer (C2099) */
    const struct { const char *name; void *addr; } tab[] = {
#define F(n) {#n, (void *)n}
#define W(n, w) {#n, (void *)w}
        F(printf), F(fprintf), F(sprintf), F(snprintf), F(vprintf), F(vfprintf), F(vsprintf), F(vsnprintf),
        F(perror), F(scanf), F(sscanf), F(puts), F(putchar), F(getchar),
        F(fopen), F(fclose), F(fflush), F(fread), F(fwrite), F(fgets), F(fputs), F(fputc), F(fgetc),
        F(feof), F(remove), F(rename), W(putc, fputc), W(getc, fgetc), W(__c2_assert_fail, host_assert_fail), W(fseek, host_fseek), W(ftell, host_ftell),
        W(__c2_stdin, host_stdin), W(__c2_stdout, host_stdout), W(__c2_stderr, host_stderr),
        F(malloc), F(calloc), F(realloc), F(free), F(exit), F(abort), F(atoi), W(atol, host_atol), F(atof),
        W(strtol, host_strtol), F(strtod), F(abs), W(labs, host_labs), F(rand), F(srand), F(getenv),
        F(qsort), F(bsearch), F(atexit),
        F(strlen), F(strcmp), F(strncmp), F(strcpy), F(strncpy), F(strcat), F(strncat), F(strchr),
        F(strrchr), F(strstr), F(memset), F(memcpy), F(memmove), F(memcmp), W(__c2_memset, memset),
        F(sqrt), F(sin), F(cos), F(tan), F(asin), F(acos), F(atan), F(atan2), F(exp), F(log), F(log10),
        F(log2), F(pow), F(fabs), F(floor), F(ceil), F(round), F(trunc), F(fmod), F(hypot), F(fmin), F(fmax),
        F(sqrtf), F(sinf), F(cosf), F(tanf), F(atan2f), F(expf), F(logf), F(powf), F(fabsf), F(floorf),
        F(ceilf), F(roundf), F(fmodf),
        W(__c2_huge_val, host_huge_val), W(__c2_huge_valf, host_huge_valf), W(__c2_nanf, host_nanf),
        F(isalpha), F(isdigit), F(isalnum), F(isspace), F(isupper), F(islower), F(isxdigit), F(ispunct),
        F(toupper), F(tolower),
        W(__c2_clocks_per_sec, host_clocks_per_sec), W(clock, host_clock),
        W(time, host_time),
        W(dlopen, host_dlopen), W(dlsym, host_dlsym), W(dlclose, host_dlclose), W(dlerror, host_dlerror),
        F(tmpfile), F(freopen), F(setbuf), F(setvbuf), F(fscanf), F(vfscanf), F(vscanf), F(vsscanf),
        F(ungetc), F(fgetpos), F(fsetpos), F(rewind), F(clearerr), F(ferror),
        F(atoll), F(strtof), W(strtold, strtod), F(strtoll), W(strtoul, host_strtoul), F(strtoull), F(system),
        F(llabs), F(div), W(ldiv, lldiv), F(lldiv), F(_Exit),
        F(memchr), F(strspn), F(strcspn), F(strpbrk), F(strtok), F(strerror), F(strcoll), F(strxfrm),
        F(sinh), F(cosh), F(tanh), F(asinh), F(acosh), F(atanh), F(exp2), F(expm1), F(log1p), F(logb), F(ilogb),
        F(frexp), F(ldexp), F(modf), F(scalbn), W(scalbln, host_scalbln), F(cbrt), F(erf), F(erfc), F(lgamma),
        F(tgamma), F(nearbyint), F(rint), W(lrint, host_lrint), F(llrint), W(lround, host_lround), F(llround),
        F(remainder), F(remquo), F(copysign), F(nan), F(nextafter), W(nexttoward, nextafter), F(fdim), F(fma),
        F(asinf), F(acosf), F(atanf), F(exp2f), F(log2f), F(log10f), F(truncf), F(hypotf), F(fminf), F(fmaxf),
        F(cbrtf), F(copysignf), W(__c2_signbit, host_signbit),
        F(isblank), F(iscntrl), F(isgraph), F(isprint),
        F(difftime), F(mktime), F(asctime), F(ctime), F(gmtime), F(localtime), F(strftime),
        W(timespec_get, host_timespec_get), W(__c2_errno, host_errno), F(signal), F(raise), F(setlocale),
        W(imaxabs, llabs), W(strtoimax, strtoll), W(strtoumax, host_strtoul),
        F(mblen), F(mbtowc), F(wctomb), F(mbstowcs), F(wcstombs), W(__c2_mb_cur_max, host_mb_cur_max),
        F(wcslen), F(wcscmp), F(wcsncmp), F(wcscpy), F(wcsncpy), F(wcscat), F(wcsncat), F(wcschr), F(wcsrchr),
        F(wcsstr), F(wmemcpy), F(wmemmove), F(wmemset), F(wmemcmp), F(wmemchr), F(wcstod), F(wcstoll), F(wcstoull),
#ifdef _WIN32
        W(strdup, _strdup), F(tmpnam), F(quick_exit), F(at_quick_exit),   /* (not from ucrtbase.dll below:
                                                     with /MT that would be a second C runtime) */
#else
        F(strdup),
#endif
#undef F
#undef W
    };
    for (size_t i = 0; i < sizeof tab / sizeof tab[0]; i++)
        if (strcmp(tab[i].name, name) == 0) return tab[i].addr;
    return NULL;
}
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
#endif

/* ============================================================ library (c2.h)
 * A compile resets the compiler's global state, parses into a fresh MIR context, links it and
 * runs the global initializers; the context then holds the program until the next successful
 * compile or C2_delete.  The compiler's own memory is released after every compile. */
#undef malloc
#undef calloc
#undef realloc
#undef free

typedef struct { char *name; void *addr; } HostSym;
struct C2 {
#ifndef NO_MIR
    MIR_context_t ctx;          /* the loaded program, NULL if none */
#endif
    int main_np;                /* parameters of main(): 0 or 2 */
    int interp;                 /* run with MIR's interpreter instead of generating machine code */
    int opt_level;              /* MIR generator: 0..3 (default 2) */
    int lazy;                   /* generate each function's code on its first call */
    int gen_live;               /* ctx still has the generator (lazy mode) */
    char *defs[64][2];
    int ndefs;
    char *incs[64];
    int nincs;
    HostSym *syms;
    int nsyms, cap_syms;
    char *errors;
    struct C2Lib { char *header, *path; void *handle; int unloaded; } libs[64];   /* C2_load */
    int nlibs;
};

static char *str_dup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = (char *)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

C2 *C2_new(void) {
    C2 *cc = (C2 *)calloc(1, sizeof *cc);
    if (cc) { cc->errors = str_dup(""); cc->opt_level = 2; }
    return cc;
}

void C2_define(C2 *cc, const char *name, const char *value) {
    if (cc->ndefs >= 64) return;
    cc->defs[cc->ndefs][0] = str_dup(name);
    cc->defs[cc->ndefs++][1] = str_dup(value ? value : "1");
}

void C2_include_dir(C2 *cc, const char *dir) {
    if (cc->nincs < 64) cc->incs[cc->nincs++] = str_dup(dir);
}

void C2_add_symbol(C2 *cc, const char *name, void *addr) {
    for (int i = 0; i < cc->nsyms; i++)
        if (strcmp(cc->syms[i].name, name) == 0) { cc->syms[i].addr = addr; return; }
    if (cc->nsyms == cc->cap_syms) {
        int cap = cc->cap_syms ? cc->cap_syms * 2 : 32;
        HostSym *p = (HostSym *)realloc(cc->syms, cap * sizeof *p);
        if (!p) return;
        cc->syms = p;
        cc->cap_syms = cap;
    }
    cc->syms[cc->nsyms].name = str_dup(name);
    cc->syms[cc->nsyms++].addr = addr;
}

const char *C2_errors(C2 *cc) { return cc->errors ? cc->errors : ""; }

void C2_use_interpreter(C2 *cc, int on) { cc->interp = on; }
void C2_set_opt_level(C2 *cc, int level) { cc->opt_level = level < 0 ? 0 : level > 3 ? 3 : level; }
void C2_set_lazy(C2 *cc, int on) { cc->lazy = on; }

#ifndef NO_MIR
static void release_program(C2 *cc) {   /* the loaded program's context (and generator, if lazy) */
    if (!cc->ctx) return;
    if (cc->gen_live) MIR_gen_finish(cc->ctx);
    MIR_finish(cc->ctx);
    cc->ctx = NULL;
    cc->gen_live = 0;
}
#endif

/* every global the compiler uses, back to its initial state (the compiler's heap is freed) */
static void reset_compiler_state(void) {
    cc_free_all();
    /* preprocessor */
    macros = NULL; nmacros = cap_macros = 0;
    memset(pp_files, 0, sizeof pp_files); npp_files = 0; pp_ctx = NULL; pp_main_file = -1;
    memset(pragma_once, 0, sizeof pragma_once); npragma_once = 0;
    memset(include_dirs, 0, sizeof include_dirs); ninclude_dirs = 0;
    memset(pending_defines, 0, sizeof pending_defines); npending_defines = 0;
    pp_counter = 0; ce_tok = NULL; ce_fail = 0;
    memset(conds, 0, sizeof conds); nconds = 0;
    /* lexer */
    src = NULL;
    memset(&current_token, 0, sizeof current_token);
    cur_line = 1;
    strcpy(cur_file, "?");
    /* back end */
#ifndef NO_MIR
    ctx = NULL; mir_module = NULL; cur_fn = NULL; prologue_last = NULL;
    memset(items, 0, sizeof items[0] * nitems); nitems = 0;
    memset(labels, 0, sizeof labels);
#endif
    mir_module_open = 0;
    memset(regs, 0, sizeof regs);
    nregs = nlabels = temp_counter = label_base = string_counter = 0;
    memset(&out_text, 0, sizeof out_text);
    memset(&fn_head, 0, sizeof fn_head); memset(&fn_pro, 0, sizeof fn_pro); memset(&fn_body, 0, sizeof fn_body);
    memset(&last_cmp, 0, sizeof last_cmp);
    /* types and symbols */
    memset(structs, 0, sizeof structs[0] * nstructs); nstructs = 0; anon_struct_counter = 0;
    memset(sigs, 0, sizeof sigs[0] * nsigs); nsigs = 0;
    memset(typedefs, 0, sizeof typedefs[0] * ntypedefs); ntypedefs = 0;
    memset(enum_consts, 0, sizeof enum_consts[0] * nenum_consts); nenum_consts = 0;
    memset(enum_tags, 0, sizeof enum_tags[0] * nenum_tags); nenum_tags = 0;
    nfunc_aliases = npending_nested = nested_counter = 0;
    memset(vars, 0, sizeof vars[0] * nvars); nvars = 0;
    memset(globals, 0, sizeof globals[0] * nglobals); nglobals = 0;
    memset(funcs, 0, sizeof funcs[0] * nfuncs); nfuncs = 0;
    memset(addr_taken, 0, sizeof addr_taken); naddr_taken = 0;
    memset(defined_names, 0, sizeof defined_names[0] * ndefined); ndefined = 0;
    memset(defined_vars, 0, sizeof defined_vars[0] * ndefined_vars); ndefined_vars = 0;
    scope_depth = 0; cur_func = NULL; cur_sret_reg = -1;
    spec_static = spec_extern = 0;
    /* statements */
    memset(break_stack, 0, sizeof break_stack); memset(continue_stack, 0, sizeof continue_stack);
    loop_depth = sw_depth = 0;
    memset(sw_stack, 0, sizeof sw_stack);
    memset(glabels, 0, sizeof glabels); nglabels = 0;
    memset(pending_inits, 0, sizeof pending_inits[0] * npending_inits); npending_inits = 0;
    memset(static_vars, 0, sizeof static_vars[0] * nstatic_vars); nstatic_vars = 0;
    static_counter = ginit_with_statics = ginit_counter = literal_counter = 0;
    nvla = nvla_scopes = vla_ok = param_depth = 0;
    nwhole_read = 0;
}

#ifndef NO_MIR
static C2 *linking;   /* the instance being linked (host symbols for import_resolver) */

static void *lib_symbol(void *handle, const char *name) {
#ifdef _WIN32
    return GetProcAddress(handle, name);
#else
    return dlsym(handle, name);
#endif
}

static void *lookup_symbol(const char *name) {
    if (linking) {
        for (int i = linking->nsyms - 1; i >= 0; i--)
            if (strcmp(linking->syms[i].name, name) == 0) return linking->syms[i].addr;
        for (int i = linking->nlibs - 1; i >= 0; i--) {   /* C2_load libraries, newest first */
            if (linking->libs[i].unloaded) continue;
            void *p = lib_symbol(linking->libs[i].handle, name);
            if (p) return p;
        }
    }
    void *p = builtin_symbol(name);
    if (p) return p;
#ifdef _WIN32
    /* last resort for names outside the table.  C² is built with the static runtime (/MT), so a
       C function found here comes from a separate copy of the CRT: fine for leaf functions, but its
       FILE* and malloc'd memory don't mix with the script's */
    static const char *dlls[] = {"ucrtbase.dll", "msvcrt.dll", "kernel32.dll"};
    for (size_t i = 0; i < 3 && !p; i++) {
        void *h = LoadLibraryA(dlls[i]);
        if (h) p = GetProcAddress(h, name);
    }
#else
    p = dlsym(RTLD_DEFAULT, name);
#endif
    return p;
}

static void *import_resolver(const char *name) { return lookup_symbol(name); }   /* checked before linking */

static void MIR_NO_RETURN mir_error(MIR_error_type_t type, const char *fmt, ...) {
    char msg[1024];
    va_list a;
    (void)type;
    va_start(a, fmt);
    vsnprintf(msg, sizeof msg, fmt, a);
    va_end(a);
    cur_line = 0;
    error("MIR: %s", msg);
    exit(1);   /* not reached */
}

/* release a MIR context whose compile failed somewhere in the middle */
static void discard_context(MIR_context_t c, int gen_on) {
    jmp_buf jb, *saved = err_jmp;
    err_jmp = &jb;
    diag_mute = 1;
    if (setjmp(jb) == 0) {
        if (gen_on) MIR_gen_finish(c);
        if (cur_fn) { MIR_finish_func(c); cur_fn = NULL; }
        if (mir_module_open) { MIR_finish_module(c); mir_module_open = 0; }
        MIR_finish(c);
    }   /* else: MIR refused to wind down this context; leave it (a small leak) */
    diag_mute = 0;
    err_jmp = saved;
}
#endif

static void parse_all(void) {
    while (current_token.type != TOKEN_EOF) parse_toplevel();
}

/* the whole pipeline.  mode 0: C API + JIT, 'T': MIR text -> MIR_scan_string -> JIT,
   'S': print MIR text, 'A': print what the C API built, 'E': print the preprocessed source.
   source NULL: read the file `path`.  Returns 0 on success. */

static int ends_with(const char *s, const char *suffix) {
    size_t n = strlen(s), k = strlen(suffix);
    return n >= k && strcmp(s + n - k, suffix) == 0;
}

static int host_has_symbol(C2 *cc, const char *name) {
    for (int i = 0; i < cc->nsyms; i++)
        if (strcmp(cc->syms[i].name, name) == 0) return 1;
    return 0;
}

static void lib_close(void *handle) {
#ifdef _WIN32
    FreeLibrary(handle);
#else
    dlclose(handle);
#endif
}

static void close_unloaded_libs(C2 *cc) {
    int k = 0;
    for (int i = 0; i < cc->nlibs; i++) {
        if (cc->libs[i].unloaded) {
            if (cc->libs[i].handle) lib_close(cc->libs[i].handle);
            free(cc->libs[i].header);
            free(cc->libs[i].path);
        } else {
            cc->libs[k++] = cc->libs[i];
        }
    }
    cc->nlibs = k;
}

static int compile_unit(C2 *cc, const char *volatile source, const char *path, int mode) {
    jmp_buf jb;
#ifndef NO_MIR
    MIR_context_t volatile newctx = NULL;
    volatile int gen_on = 0;
#endif
    diag_len = 0;
    if (diag_buf) diag_buf[0] = '\0';
    reset_compiler_state();
    for (int i = 0; i < cc->nincs; i++) include_dirs[ninclude_dirs++] = cc->incs[i];
    for (int i = 0; i < cc->ndefs; i++) {
        pending_defines[npending_defines][0] = cc->defs[i][0];
        pending_defines[npending_defines++][1] = cc->defs[i][1];
    }
    c2_ext = path && ends_with(path, ".c2");
    compiling = cc;
    if (c2_ext) {
        pending_defines[npending_defines][0] = "__C2_EXT__";
        pending_defines[npending_defines++][1] = "1";
    }
    err_jmp = &jb;
    if (setjmp(jb)) {                                  /* error(): abandon this compile */
#ifndef NO_MIR
        if (newctx) discard_context(newctx, gen_on);
        linking = NULL;
#endif
        err_jmp = NULL;
        cc_free_all();
        free(cc->errors);
        cc->errors = str_dup(diag_buf ? diag_buf : "");
        return -1;
    }
    char *text = NULL;
    if (!source) {
        text = read_text_file(path);
        if (!text) { cur_line = 0; error("cannot open '%s'", path); }
        source = text;
    }
    Buf pre = {0};                                     /* built-in declarations, then implicit #includes */
    buf_emit(&pre, "%s", prelude_base);
    if (c2_ext)                                        /* .c2: the common headers are always there */
        buf_emit(&pre, "#include <stddef.h>\n#include <stdint.h>\n#include <stdio.h>\n"
                       "#include <string.h>\n#include <stdlib.h>\n#include <math.h>\n");
    for (int i = 0; i < cc->nlibs; i++)
        if (!cc->libs[i].unloaded) buf_emit(&pre, "#include \"%s\"\n", cc->libs[i].header);
    char *code = preprocess_source(source, path, pre.p);
    if (mode == 'E') fputs(code, stdout);
    else {
        use_api = mode != 'S' && mode != 'T';
#ifndef NO_MIR
        newctx = ctx = MIR_init();
        MIR_set_error_func(ctx, mir_error);
#endif
        src = code;
        cur_line = 1;
        next_token();
        prescan_definitions();
        be_module_begin();
        parse_all();
        cur_line = 0;
        for (int i = 0; i < nfuncs; i++)
            if (!funcs[i].defined && !funcs[i].is_import && funcs[i].item_emitted)
                error("function '%s' is used but never defined", funcs[i].name);
        for (int i = 0; i < nglobals; i++) ensure_global_storage(&globals[i]);
        be_module_end();
        if (mode == 'S') fputs(out_text.p, stdout);
#ifndef NO_MIR
        else if (mode == 'A') MIR_output(ctx, stdout);
        else if (mode == 'C') c_module(mir_module, stdout);
        else {
            MIR_module_t m = mir_module;
            if (mode == 'T') {
                MIR_scan_string(ctx, out_text.p);
                m = DLIST_TAIL(MIR_module_t, *MIR_get_module_list(ctx));
            }
            linking = cc;
            for (MIR_item_t it = DLIST_HEAD(MIR_item_t, m->items); it; it = DLIST_NEXT(MIR_item_t, it))
                if (it->item_type == MIR_import_item && !lookup_symbol(it->u.import_id))
                    error("cannot resolve external symbol '%s'", it->u.import_id);
            MIR_load_module(ctx, m);
            if (cc->interp) MIR_link(ctx, MIR_set_interp_interface, import_resolver);
            else {
                MIR_gen_init(ctx);
                MIR_gen_set_optimize_level(ctx, (unsigned)cc->opt_level);
                gen_on = 1;
                MIR_link(ctx, cc->lazy ? MIR_set_lazy_gen_interface : MIR_set_gen_interface, import_resolver);
            }
            for (MIR_item_t it = DLIST_HEAD(MIR_item_t, m->items); it; it = DLIST_NEXT(MIR_item_t, it))
                if (it->item_type == MIR_func_item && strncmp(it->u.func->name, "__c2_ginit", 10) == 0)
                    ((void (*)(void))it->addr)();     /* global initializers */
            int keep_gen = gen_on && cc->lazy;             /* lazy: functions are generated on their first call */
            if (gen_on && !keep_gen) MIR_gen_finish(ctx);
            gen_on = 0;
            linking = NULL;
            Func *mf = find_func("main");
            cc->main_np = mf && mf->defined ? mf->np : 0;
            release_program(cc);                       /* the previous program */
            close_unloaded_libs(cc);                   /* C2_unload: nothing uses them any more */
            cc->ctx = newctx;
            cc->gen_live = keep_gen;
            newctx = NULL;
        }
        if (newctx) { MIR_finish(newctx); newctx = NULL; }   /* print modes */
#endif
    }
    err_jmp = NULL;
    cc_free_all();
#ifndef NO_MIR
    ctx = NULL;
#endif
    free(cc->errors);
    cc->errors = str_dup(diag_buf ? diag_buf : "");
    return 0;
}

static const char *compile_result(C2 *cc, int failed) {
    if (!failed) return "";
    return cc->errors && cc->errors[0] ? cc->errors : "error: compilation failed\n";
}

const char *C2_compile(C2 *cc, const char *filename, const char *source) {
    return compile_result(cc, compile_unit(cc, source, filename ? filename : "<script>", 0) != 0);
}

const char *C2_compile_file(C2 *cc, const char *path) { return compile_result(cc, compile_unit(cc, NULL, path, 0) != 0); }

void *C2_get(C2 *cc, const char *name) {
#ifndef NO_MIR
    if (!cc->ctx) return NULL;
    for (MIR_module_t m = DLIST_HEAD(MIR_module_t, *MIR_get_module_list(cc->ctx)); m; m = DLIST_NEXT(MIR_module_t, m))
        for (MIR_item_t it = DLIST_HEAD(MIR_item_t, m->items); it; it = DLIST_NEXT(MIR_item_t, it)) {
            if (it->item_type != MIR_func_item && it->item_type != MIR_data_item && it->item_type != MIR_bss_item
                && it->item_type != MIR_ref_data_item && it->item_type != MIR_expr_data_item) continue;
            if (strcmp(MIR_item_name(cc->ctx, it), name) == 0)
                return it->item_type == MIR_func_item ? it->addr : it->u.data->u.els;
        }
#endif
    (void)name;
    return NULL;
}

int C2_run_main(C2 *cc, int argc, char **argv) {
    void *m = C2_get(cc, "main");
    if (!m) return -1;
    int ret = cc->main_np >= 2 ? (int)((int64_t (*)(int64_t, char **))m)(argc, argv)
                               : (int)((int64_t (*)(void))m)();
    fflush(stdout);
    return ret;
}

void C2_delete(C2 *cc) {
    if (!cc) return;
#ifndef NO_MIR
    release_program(cc);
#endif
    for (int i = 0; i < cc->nlibs; i++) cc->libs[i].unloaded = 1;
    close_unloaded_libs(cc);
    for (int i = 0; i < cc->ndefs; i++) { free(cc->defs[i][0]); free(cc->defs[i][1]); }
    for (int i = 0; i < cc->nincs; i++) free(cc->incs[i]);
    for (int i = 0; i < cc->nsyms; i++) free(cc->syms[i].name);
    free(cc->syms);
    free(cc->errors);
    free(cc);
}

/* C2_load(c2, "sdl.h", "SDL2"): scripts compiled from now on see sdl.h's declarations (as if every
   script #included it first) and its functions resolve from the SDL2 library: SDL2.dll on Windows,
   libSDL2.dylib / SDL2.dylib on macOS, libSDL2.so / SDL2.so elsewhere (a path may come first:
   "libs/SDL2").  Returns "" or an error message. */
const char *C2_load(C2 *cc, const char *header, const char *library) {
    static char msg[4400];
    if (cc->nlibs >= 64) return "C2_load: too many libraries";
    char full[4096];                                   /* realpath needs PATH_MAX */
#ifdef _WIN32
    if (!_fullpath(full, header, sizeof full)) snprintf(full, sizeof full, "%s", header);
#else
    if (!realpath(header, full)) snprintf(full, sizeof full, "%s", header);
#endif
    FILE *f = fopen(full, "rb");
    if (!f) { snprintf(msg, sizeof msg, "C2_load: cannot open header '%s'", header); return msg; }
    fclose(f);
#ifdef _WIN32
    static const char *exts[] = {".dll"};
#elif defined(__APPLE__)
    static const char *exts[] = {".dylib", ".so"};
#else
    static const char *exts[] = {".so"};
#endif
    const char *base = library;                        /* the file name part, for the lib prefix */
    for (const char *p = library; *p; p++) if (*p == '/' || *p == '\\') base = p + 1;
    void *h = NULL;
    char path[1100];
    for (int here = 0; !h && here < 2; here++) {       /* the system's search, then the current directory */
      if (here && base != library) break;
      for (size_t e = 0; !h && e < sizeof exts / sizeof exts[0]; e++) {
        for (int prefix = 0; !h && prefix < 2; prefix++) {
#ifdef _WIN32
            if (prefix) break;
#endif
            snprintf(path, sizeof path, "%s%.*s%s%s%s", here ? "./" : "", (int)(base - library), library,
                     prefix ? "lib" : "", base, exts[e]);
#ifdef _WIN32
            h = LoadLibraryA(path);
#else
            h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
#endif
        }
      }
    }
    if (!h) { snprintf(msg, sizeof msg, "C2_load: cannot load library '%s' (tried %s and the like)", library, path); return msg; }
    for (int i = 0; i < cc->nlibs; i++)                /* loading a header again replaces it */
        if (!cc->libs[i].unloaded && strcmp(cc->libs[i].header, full) == 0) cc->libs[i].unloaded = 1;
    cc->libs[cc->nlibs].header = str_dup(full);
    cc->libs[cc->nlibs].path = str_dup(path);
    cc->libs[cc->nlibs].handle = h;
    cc->libs[cc->nlibs++].unloaded = 0;
    return "";
}

/* C2_unload(c2, "sdl.h"): the next compiles no longer see it.  The library itself is closed once a
   new program replaced the current one (which may still call into it), or by C2_delete. */
const char *C2_unload(C2 *cc, const char *header) {
    char full[4096];                                   /* realpath needs PATH_MAX */
#ifdef _WIN32
    if (!_fullpath(full, header, sizeof full)) snprintf(full, sizeof full, "%s", header);
#else
    if (!realpath(header, full)) snprintf(full, sizeof full, "%s", header);
#endif
    for (int i = 0; i < cc->nlibs; i++)
        if (!cc->libs[i].unloaded && strcmp(cc->libs[i].header, full) == 0) { cc->libs[i].unloaded = 1; return ""; }
    return "C2_unload: that header was not loaded";
}

/* ============================================================ command line */
#ifndef C2_NO_MAIN
int main(int argc, char **argv) {
    int mode = 0;   /* 0: run (API), 'S': print text, 'T': run via text, 'A': print via API, 'E': preprocess */
    const char *input = NULL;
    C2 *cc = C2_new();
    int argi = 1;
    for (; argi < argc && !input; argi++) {
        const char *a = argv[argi];
        if (strcmp(a, "-S") == 0) mode = 'S';
        else if (strcmp(a, "-T") == 0) mode = 'T';
        else if (strcmp(a, "-A") == 0) mode = 'A';
        else if (strcmp(a, "-E") == 0) mode = 'E';
        else if (strcmp(a, "-C") == 0) mode = 'C';
#ifndef NO_MIR
        else if (strcmp(a, "-Crt") == 0) { c_print_runtime(stdout); return 0; }
#endif
        else if (strcmp(a, "-i") == 0) C2_use_interpreter(cc, 1);
        else if (a[0] == '-' && a[1] == 'O' && a[2] >= '0' && a[2] <= '3' && !a[3]) C2_set_opt_level(cc, a[2] - '0');
        else if (strcmp(a, "-lazy") == 0) C2_set_lazy(cc, 1);
        else if (strcmp(a, "-load") == 0 && argi + 1 < argc) {   /* -load header.h,library */
            char spec[1100];
            snprintf(spec, sizeof spec, "%s", argv[++argi]);
            char *comma = strrchr(spec, ',');
            if (!comma) { fprintf(stderr, "-load wants header.h,library\n"); return 1; }
            *comma = '\0';
            const char *err = C2_load(cc, spec, comma + 1);
            if (*err) { fprintf(stderr, "%s\n", err); return 1; }
        }
        else if (strncmp(a, "-I", 2) == 0) C2_include_dir(cc, a[2] ? a + 2 : (argi + 1 < argc ? argv[++argi] : ""));
        else if (strncmp(a, "-D", 2) == 0) {
            const char *d = a[2] ? a + 2 : (argi + 1 < argc ? argv[++argi] : "");
            char name[64];
            const char *eq = strchr(d, '=');
            snprintf(name, sizeof name, "%.*s", eq ? (int)(eq - d) : (int)strlen(d), d);
            C2_define(cc, name, eq ? eq + 1 : "1");
        } else input = a;
    }
    if (!input) {
        fprintf(stderr, "usage: %s [-S | -T | -A | -E | -C | -i] [-Idir] [-Dname[=value]] [-load header.h,library] file.c|file.c2 [program arguments...]\n"
                        "  (none) JIT and run   -i interpret (no machine code generator)\n"
                        "  -C translate to C (compile it with the output of -Crt: c2_rt.c)\n"
                        "  -O0..-O3 MIR optimization level (default -O2)   -lazy generate functions on first call\n"
                        "  -S print MIR   -E preprocess   -T/-A run/print through the other MIR path\n"
                        "  -load header.h,library  declarations from the header, functions from the library (no extension)\n", argv[0]);
        return 1;
    }
#ifdef NO_MIR
    if (mode != 'S' && mode != 'E') {
        fprintf(stderr, "note: built without MIR, printing MIR text instead (-S)\n");
        mode = 'S';
    }
#endif
    diag_echo = 1;                                     /* messages go straight to stderr */
    if (compile_unit(cc, NULL, input, mode) != 0) return 1;
    int ret = 0;
    if (mode == 0 || mode == 'T') {
        if (!C2_get(cc, "main")) { fprintf(stderr, "error: no main function\n"); return 1; }
        ret = C2_run_main(cc, argc - argi + 1, argv + argi - 1);
    }
    C2_delete(cc);
    return ret;   /* in run modes: main()'s result, like c2m -eg */
}
#endif
