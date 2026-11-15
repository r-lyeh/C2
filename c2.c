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
 * Build (MIR's sources are in 3rd/; no build system needed):
 *   cl /O2 /DNDEBUG /MT c2.c 3rd\mir.c 3rd\mir-gen.c                         (Windows: cl or clang-cl)
 *   cc -O2 -DNDEBUG -fno-tree-sra c2.c 3rd/mir.c 3rd/mir-gen.c -lm -ldl -lpthread -o c2
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
 *             K&R function definitions, defer (TS 25755), _Countof (C2y), GNU ({ statement expressions })
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
__declspec(dllimport) unsigned long __stdcall GetModuleFileNameA(void *, char *, unsigned long);
typedef struct { unsigned long code; } c2_exc_record;  /* EXCEPTION_RECORD / _POINTERS: what C2_crash_handler reads */
typedef struct { c2_exc_record *record; void *context; } c2_exc_pointers;
__declspec(dllimport) void *__stdcall AddVectoredExceptionHandler(unsigned long, long (__stdcall *)(c2_exc_pointers *));
__declspec(dllimport) unsigned long __stdcall RemoveVectoredExceptionHandler(void *);
#else
#include <dlfcn.h>
#include <unistd.h>
#ifdef __APPLE__
int _NSGetExecutablePath(char *buf, unsigned *size);
#endif
#endif
#include "c2.h"

/* __builtin_{add,sub,mul}_overflow: the exact result (sign + 128-bit magnitude), stored truncated in
   *res (rsize bytes, little-endian), 1 if it doesn't fit.  as/bs/rs: signed operands / result.  The
   same text is the -C runtime's (C2_ARITH_SRC is stringified below). */
#define C2_ARITH_SRC(NAME) \
static int NAME(int op, unsigned long long a, int as, unsigned long long b, int bs, void *res, int rsize, int rs) { \
    int an = as && (long long)a < 0, bn = bs && (long long)b < 0, neg = 0, bits = rsize * 8, ovf; \
    unsigned long long am = an ? 0 - a : a, bm = bn ? 0 - b : b, hi = 0, lo = 0; \
    if (op == 2) { \
        unsigned long long a0 = am & 0xffffffffu, a1 = am >> 32, b0 = bm & 0xffffffffu, b1 = bm >> 32; \
        unsigned long long p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1; \
        unsigned long long mid = (p00 >> 32) + (p01 & 0xffffffffu) + (p10 & 0xffffffffu); \
        lo = (p00 & 0xffffffffu) | (mid << 32); \
        hi = p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32); \
        neg = an != bn; \
    } else { \
        if (op == 1) bn = !bn; \
        if (an == bn) { lo = am + bm; hi = lo < am; neg = an; } \
        else if (am >= bm) { lo = am - bm; neg = an; } \
        else { lo = bm - am; neg = bn; } \
    } \
    if (!hi && !lo) neg = 0; \
    if (hi) ovf = 1; \
    else if (rs) ovf = neg ? lo > (1ull << (bits - 1)) : lo > (1ull << (bits - 1)) - 1; \
    else ovf = neg || (bits < 64 && (lo >> (bits & 63)) != 0); \
    unsigned long long v = neg ? 0 - lo : lo; \
    memcpy(res, &v, (size_t)rsize); \
    return ovf; \
}
C2_ARITH_SRC(host_arith_overflow)
#define C2_STR_(...) #__VA_ARGS__
#define C2_STR(...) C2_STR_(__VA_ARGS__)

/* Coroutines (runtime/coro.c2) on minicoro (3rd/minicoro.h): a coroutine is a function with a stack of its own
   that co_yield() pauses and co_resume() continues.  P##co_* are what scripts call as __c2_co_*; the
   same text is the -C runtime's ((calloc), (free): not the compiler's counting macros, defined by the
   time it is stringified).  Every live one is in a list, so a hot reload can refuse to free the
   program while a suspended coroutine's stack still returns into it.
   */
#define C2_CORO_SRC(P) \
typedef struct c2_co { mco_coro *co; void (*fn)(void *); void *arg; struct c2_co *next, *prev; void *dbg; } c2_co; \
static c2_co c2_co_list = {0, 0, 0, &c2_co_list, &c2_co_list, 0}; \
static void c2_co_entry(mco_coro *co) { c2_co *c = (c2_co *)mco_get_user_data(co); c->fn(c->arg); } \
static void *P##co_create(void (*fn)(void *), void *arg, size_t stack) { \
    c2_co *c = (c2_co *)(calloc)(1, sizeof *c); \
    if (!c || !fn) { (free)(c); return NULL; } \
    mco_desc d = mco_desc_init(c2_co_entry, stack ? stack : 1024 * 1024);   /* (untouched pages cost no RAM) */ \
    d.user_data = c; \
    c->fn = fn; c->arg = arg; \
    if (mco_create(&c->co, &d) != MCO_SUCCESS) { (free)(c); return NULL; } \
    c->next = c2_co_list.next; c->prev = &c2_co_list; c2_co_list.next->prev = c; c2_co_list.next = c; \
    return c; \
} \
static int P##co_resume(void *h) { \
    c2_co *c = (c2_co *)h; \
    if (!c || mco_status(c->co) != MCO_SUSPENDED) return 0; \
    void *dbg = P##dbg_top;                  /* (C2_set_debug: each coroutine has its own call stack) */ \
    if (P##resume_depth < 64) P##resume_from[P##resume_depth] = dbg;   /* and C2_backtrace goes on there */ \
    P##resume_depth++; \
    P##dbg_top = c->dbg; \
    mco_resume(c->co); \
    P##resume_depth--; \
    c->dbg = P##dbg_top; \
    P##dbg_top = dbg; \
    return mco_status(c->co) == MCO_SUSPENDED; \
} \
static void P##co_yield(void) { mco_coro *co = mco_running(); if (co) mco_yield(co); } \
static int P##co_status(void *h) { return h ? (int)mco_status(((c2_co *)h)->co) : 0; } \
static void *P##co_running(void) { mco_coro *co = mco_running(); return co ? mco_get_user_data(co) : NULL; } \
static void P##co_destroy(void *h) { \
    c2_co *c = (c2_co *)h; \
    if (!c) return; \
    c->prev->next = c->next; c->next->prev = c->prev; \
    mco_destroy(c->co); \
    (free)(c); \
} \
static int P##co_suspended(void) { \
    int n = 0; \
    for (c2_co *c = c2_co_list.next; c != &c2_co_list; c = c->next) n += mco_status(c->co) != MCO_DEAD; \
    return n; \
}
static void *host_dbg_top;   /* C2_set_debug: the innermost script frame (C2Frame), NULL outside scripts */
static void *host_resume_from[64];   /* ...and, for each coroutine running, the frame that resumed it */
static int host_resume_depth;

/* try/throw (runtime/try.c2): throw(code) sets __c2_throwing; in a program that throws, every call is followed
   by a check of it, and a set one returns from the function at once, through its defers and the
   destructors of its temporaries, up to the try, which clears it and returns the code.  The same text is
   the -C runtime's. */
#define C2_TRY_SRC(P) \
static int P##throwing, P##try_depth, P##exiting; \
static int P##try(void (*fn)(void *), void *arg) { \
    void *dbg = P##dbg_top; \
    P##try_depth++; \
    fn(arg); \
    P##try_depth--; \
    P##dbg_top = dbg; \
    int code = P##throwing; \
    if (!P##exiting) P##throwing = 0;   /* (a sandboxed exit() goes on, to the host) */ \
    return code; \
} \
static void P##throw(int code) { \
    if (!P##try_depth) { fprintf(stderr, "throw(%d) outside of a try\n", code); abort(); } \
    if (!code) { fputs("throw(0): 0 is what try returns when nothing was thrown\n", stderr); abort(); } \
    P##throwing = code; \
}
C2_TRY_SRC(host_)
static int host_exit_code;   /* C2_sandbox: exit(code) */
#ifndef C2_NO_CORO
#define MINICORO_IMPL
#include "3rd/minicoro.h"
C2_CORO_SRC(host_)
#endif

#ifndef NO_MIR
#include "3rd/mir.h"
#include "3rd/mir-gen.h"
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
static int diag_echo, diag_mute, warn_mute;   /* warn_mute: an unrolled body's copies */
static jmp_buf *err_jmp;

static void diag(const char *fmt, ...) {
    char msg[2048];
    va_list a;
    va_start(a, fmt);
    vsnprintf(msg, sizeof msg, fmt, a);
    va_end(a);
    if (diag_mute) return;
    if (warn_mute && strstr(msg, ": warning: ")) return;
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
    TOKEN_TYPEOF, TOKEN_TYPEOF_UNQUAL, TOKEN_AUTO, TOKEN_CONSTEXPR, TOKEN_REGISTER, TOKEN_DEFER, TOKEN_COUNTOF,
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
    unsigned char a_indeterminate, a_nodiscard, a_export, a_musttail, a_persist, a_destructor, a_copy, a_noncomm;   /* [[attributes]] just before this token */
    short a_unroll;                                 /* [[unroll]]: -1; [[unroll(N)]]: N */
    unsigned char a_container;                      /* [[__c2_array]] [[__c2_map]] [[__c2_set]]: C2_CK_* */
    unsigned char a_skip;                           /* [[skip]]: reflection leaves it out */
    const char *a_rename;                           /* [[rename("hp")]]: the saved name; NULL if none */
    const struct FieldMeta *a_meta;                 /* [[clamp(a, b)]] [[clamp(N)]] [[comment("...")]]; NULL if none */
    signed char a_operator;                         /* [[operator+]]: OPR_ADD...; -1: not supported */
    const char *a_deprecated;                     /* " " or the reason; NULL if none */
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

/* [[indeterminate]] (C++26) / [[unclear]] on a local: .c2 doesn't zero it (a big buffer), .c doesn't
   warn when it is read before being set */
enum { C2_CK_ARRAY = 1, C2_CK_MAP, C2_CK_SET };   /* array(T), map(K, V), set(K): [[__c2_array]] ... after the '*' */
static int attr_indeterminate;
static int attr_persist;          /* [[persist]] on a global: keeps its value across hot reloads */
static int attr_destructor;       /* [[destructor]] void T_free(T *): frees a T (temporaries, .c2) */
static int attr_noncomm;           /* [[noncommutative]]: 2 * v doesn't use this v * 2 */
static int attr_copy;             /* [[copy]] T T_copy(const T *): copies a named T where it is stored */
/* [[operator+]] on a .c2 function: the operator on struct and union operands calls it */
enum { OPR_NONE, OPR_ADD, OPR_SUB, OPR_MUL, OPR_DIV, OPR_MOD, OPR_EQ, OPR_LT, OPR_LE, OPR_GT, OPR_GE, OPR_INDEX,
       OPR_ADD_SET, OPR_SUB_SET, OPR_MUL_SET, OPR_DIV_SET, OPR_MOD_SET, OPR_NEG, OPR_COUNT };
static const char *const opr_text[OPR_COUNT] = {"", "+", "-", "*", "/", "%", "==", "<", "<=", ">", ">=", "[]",
                                                "+=", "-=", "*=", "/=", "%=", "-"};   /* OPR_NEG: [[operator-]] with 1 parameter */
static int attr_operator;
static char attr_operator_text[8];
/* bit-fields: gcc's layout on every platform; MSVC's for structs declared after #pragma ms_struct on
   (as gcc and clang do), which C2_load headers get on Windows to match MSVC-built libraries */
static int ms_struct_pragma;

/* Modules (.c2): #import "dir/name" includes dir/name.c2 once, between markers the preprocessor adds.
   A top-level name a module declares without [[export]] is private to it: it gets the internal name
   __m<N>_name, and the lexer maps that module's later uses of `name` to it (not after '.' or '->').
   [[export]]ed names stay as written: visible everywhere, to C code and to C2_get. */
static int mod_stack[16], mod_sp;                  /* the modules being lexed, innermost last */
static int cur_module(void) { return mod_sp ? mod_stack[mod_sp - 1] : -1; }
typedef struct { int module; char name[32]; } Private;
static Private privates[2048];
static int nprivates;
static int name_is_func(const char *name);
static char module_alias[64][32];                  /* by module number: its name (messages) */
static char module_path[64][4096];
static int nmodules;
typedef struct { char name[32]; int module; } ModAlias;   /* name.member: the default name and `as` ones */
static ModAlias mod_aliases[128];
static int nmod_aliases;
static void add_mod_alias(const char *name, int m) {
    for (int i = 0; i < nmod_aliases; i++) if (!strcmp(mod_aliases[i].name, name)) { mod_aliases[i].module = m; return; }
    if (nmod_aliases < 128) { snprintf(mod_aliases[nmod_aliases].name, 32, "%s", name); mod_aliases[nmod_aliases++].module = m; }
}
static int prev_tok_type;                          /* the token before the one being lexed */

static const char *private_name(int module, const char *name) {   /* NULL if not private there */
    static char out[48];
    for (int i = nprivates - 1; i >= 0; i--)
        if (privates[i].module == module && !strcmp(privates[i].name, name)) {
            snprintf(out, sizeof out, "__m%d_%s", module, name);
            return out;
        }
    return NULL;
}
static void private_check(const char *name) {     /* a clearer error for another module's private name */
    for (int i = 0; i < nprivates; i++)
        if (!strcmp(privates[i].name, name) && privates[i].module != cur_module())
            error("'%s' is private to module '%s' (not [[export]]ed)", name, module_alias[privates[i].module]);
}
static const char *demangle(const char *name) {    /* __m3_x -> x */
    if (strncmp(name, "__m", 3) || !isdigit((unsigned char)name[3])) return name;
    const char *p = name + 3;
    while (isdigit((unsigned char)*p)) p++;
    return *p == '_' ? p + 1 : name;
}
/* [[nodiscard]]: warn when a call's result is thrown away; [[deprecated("why")]]: warn at each call.
   C² ignores the other attributes (maybe_unused, fallthrough, noreturn...): it has no warnings for
   them to silence, and MIR can't use the optimizer hints. */
static int attr_nodiscard, attr_export, attr_musttail, attr_unroll, attr_container, attr_skip;
/* every attribute C² knows (C23's, and its own): __has_c_attribute, and a warning for any other in .c2 */
static const char *attr_warned;   /* the end of the last attribute warned about */
static const char *known_attrs[] = {"nodiscard", "deprecated", "maybe_unused", "fallthrough", "noreturn", "_Noreturn",
    "unsequenced", "reproducible", "pure", "indeterminate", "unclear", "export", "unroll", "musttail", "skip", "rename",
    "clamp", "comment", "persist", "destructor", "copy", "operator", "noncommutative", "nocommutative",
    "__c2_array", "__c2_map", "__c2_set", NULL};
static char attr_rename[32];   /* [[rename("hp")]]: a field's saved name */
/* a field's metadata for reflection: [[clamp(0, 100)]] [[clamp(16)]] (text) [[comment("...")]] */
typedef struct FieldMeta { int has_clamp; double lo, hi; int maxlen; char *comment; } FieldMeta;
static FieldMeta attr_meta;
static int attr_meta_used;
static char attr_deprecated[160];   /* "" none, " " without a reason, else the reason */
static char *str_dup(const char *s);
/* snprintf where cutting is expected and fine (no -Wformat-truncation: names are checked elsewhere) */
static int fmt_to(char *out, size_t n, const char *fmt, ...) { va_list ap; va_start(ap, fmt); int r = vsnprintf(out, n, fmt, ap); va_end(ap); return r; }

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
    if (c2_ext) {                                      /* 21.f() reads as the float 21.f, then a call */
        const char *q = b;
        while (isdigit((unsigned char)*q)) q++;
        if (*q == '.' && q > b && !dot_starts_member(b, q) && (isalpha((unsigned char)q[1]) || q[1] == '_')) {
            const char *e = q + 1;
            while (isalnum((unsigned char)*e) || *e == '_' || ((*e == '+' || *e == '-') && (e[-1] == 'e' || e[-1] == 'E'))) e++;
            const char *w = e;
            while (*w == ' ' || *w == '\t') w++;
            if (*w == '(')
                error("'%.*s' is a float literal: write (%.*s).%.*s() to call %.*s(%.*s)", (int)(e - b), b, (int)(q - b), b,
                      (int)(e - q - 1), q + 1, (int)(e - q - 1), q + 1, (int)(q - b), b);
        }
    }
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
        if ((*q == '.' && !dot_starts_member(b, q) && !(c2_ext && q[1] == '.')) || *q == 'e' || *q == 'E') is_float = 1;   /* (0..10: a range) */
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
    if (isalnum((unsigned char)*src) || *src == '_' || (*src == '.' && !dot_starts_member(b, src) && !(c2_ext && src[1] == '.')))
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

static void next_token_raw(void);
static int lex_depth;
static void next_token(void) {                 /* the attributes seen before a token travel with it */
    if (lex_depth++ == 0) { attr_indeterminate = attr_nodiscard = attr_export = attr_musttail = attr_persist = attr_operator = attr_destructor = attr_copy = attr_noncomm = attr_unroll = attr_container = attr_skip = attr_meta_used = 0; attr_rename[0] = '\0'; memset(&attr_meta, 0, sizeof attr_meta); attr_deprecated[0] = '\0'; prev_tok_type = current_token.type; }
    next_token_raw();
    if (--lex_depth == 0) {
        current_token.a_export = (unsigned char)attr_export;
        current_token.a_musttail = (unsigned char)attr_musttail;
        current_token.a_unroll = (short)attr_unroll;
        current_token.a_container = (unsigned char)attr_container;
        current_token.a_skip = (unsigned char)attr_skip;
        current_token.a_rename = attr_rename[0] ? str_dup(attr_rename) : NULL;
        current_token.a_meta = NULL;
        if (attr_meta_used) { FieldMeta *m = cc_malloc(sizeof *m); *m = attr_meta; current_token.a_meta = m; }
        current_token.a_persist = (unsigned char)attr_persist;
        current_token.a_destructor = (unsigned char)attr_destructor;
        current_token.a_copy = (unsigned char)attr_copy;
        current_token.a_noncomm = (unsigned char)attr_noncomm;
        current_token.a_operator = (signed char)attr_operator;
        current_token.a_indeterminate = (unsigned char)attr_indeterminate;
        current_token.a_nodiscard = (unsigned char)attr_nodiscard;
        current_token.a_deprecated = attr_deprecated[0] ? str_dup(attr_deprecated) : NULL;
    }
}

static void next_token_raw(void) {
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
            int wd = 0;                                /* but these (the preprocessor may space out ] ]) */
            for (const char *w = src; *w; w++) {
                if (*w == '"') { for (w++; *w && *w != '"'; w++) if (*w == '\\' && w[1]) w++; if (!*w) break; continue; }
                if (*w == '[') wd++;
                else if (*w == ']' && --wd == 0) break;
                if (isalnum((unsigned char)w[-1]) || w[-1] == '_') continue;
                if (!strncmp(w, "indeterminate", 13) || !strncmp(w, "unclear", 7)) attr_indeterminate = 1;
                if (!strncmp(w, "nodiscard", 9)) attr_nodiscard = 1;
                if ((!strncmp(w, "pure", 4) && !isalnum((unsigned char)w[4]) && w[4] != '_') ||   /* [[pure]]: C23's [[reproducible]], */
                    !strncmp(w, "reproducible", 12) || !strncmp(w, "unsequenced", 11))          /* gcc's pure; a call is for its */
                    if (!attr_nodiscard) attr_nodiscard = 2;                                    /* result: keep it */
                if (c2_ext && (w[-1] == '[' || w[-1] == ',' || w[-1] == ' ') && (isalpha((unsigned char)*w) || *w == '_')) {
                    const char *e = w;                       /* an attribute's name (at the list's top level) */
                    while (isalnum((unsigned char)*e) || *e == '_') e++;
                    const char *b = w - 1;
                    while (b > src && (*b == ' ' || *b == '\t')) b--;
                    int depth0 = 0;                          /* inside (...) of another attribute: not a name */
                    for (const char *q = src; q < w; q++) {
                        if (*q == '"') { for (q++; q < w && *q != '"'; q++) if (*q == '\\') q++; continue; }
                        if (*q == '(') depth0++; else if (*q == ')') depth0--;
                    }
                    const char *a = e;
                    while (*a == ' ' || *a == '\t') a++;
                    int ns = *a == ':';                      /* gnu::x (the preprocessor spaces it: gnu : : x) */
                    if (!depth0 && (*b == '[' || *b == ',') && !ns && w > attr_warned) {
                        attr_warned = e;                     /* (a token read again: warned once) */
                        int known = 0;
                        for (int i = 0; known_attrs[i] && !known; i++)
                            known = strlen(known_attrs[i]) == (size_t)(e - w) && !strncmp(known_attrs[i], w, (size_t)(e - w));
                        if (!known) diag("%s:%d: warning: unknown attribute [[%.*s]], ignored\n", cur_file, cur_line, (int)(e - w), w);
                    }
                }
                if (!strncmp(w, "musttail", 8)) attr_musttail = 1;
                if (!strncmp(w, "skip", 4) && !isalnum((unsigned char)w[4]) && w[4] != '_') attr_skip = 1;
                if (!strncmp(w, "rename", 6) && !isalnum((unsigned char)w[6]) && w[6] != '_') {   /* [[rename("hp")]] */
                    const char *m = w + 6;
                    while (*m == ' ' || *m == '\t' || *m == '(') m++;
                    if (*m != '"') error("[[rename(\"name\")]]: a string with the saved name expected");
                    int k = 0;
                    for (m++; *m && *m != '"' && k < (int)sizeof attr_rename - 1; m++) attr_rename[k++] = *m;
                    attr_rename[k] = '\0';
                    if (!k) error("[[rename(\"\")]]: the name is empty");
                }
                if (!strncmp(w, "clamp", 5) && !isalnum((unsigned char)w[5]) && w[5] != '_') {   /* [[clamp(0, 100)]] [[clamp(16)]] */
                    const char *m = w + 5;
                    double v[2];
                    int nv = 2;
                    while (*m == ' ' || *m == '\t') m++;
                    if (*m != '(') error("[[clamp(min, max)]] for numbers, [[clamp(N)]] for text: numbers expected");
                    m++;
                    for (int k = 0; k < nv; k++) {
                        while (*m == ' ' || *m == '\t') m++;
                        int neg = *m == '-';
                        if (*m == '-' || *m == '+') m++;
                        while (*m == ' ' || *m == '\t') m++;
                        char *e;
                        v[k] = strtod(m, &e);
                        if (e == m) error("[[clamp(min, max)]] for numbers, [[clamp(N)]] for text: numbers expected");
                        if (neg) v[k] = -v[k];
                        for (m = e; *m && strchr("uUlLfF", *m); m++) {}   /* 100u, 1.5f */
                        while (*m == ' ' || *m == '\t') m++;
                        if (k == 0 && *m == ')') { nv = 1; break; }   /* [[clamp(16)]]: text, at most 16 characters */
                        if (*m != (k ? ')' : ',')) error("[[clamp(min, max)]] for numbers, [[clamp(N)]] for text: numbers expected");
                        m++;
                    }
                    if (nv == 1) {
                        if (v[0] < 1 || v[0] > 1 << 30 || v[0] != (int)v[0]) error("[[clamp(N)]]: N is a number of characters, 1 or more");
                        attr_meta.maxlen = (int)v[0]; attr_meta_used = 1;
                    } else {
                        if (v[0] > v[1]) error("[[clamp(%g, %g)]]: the minimum is above the maximum", v[0], v[1]);
                        attr_meta.has_clamp = 1; attr_meta.lo = v[0]; attr_meta.hi = v[1]; attr_meta_used = 1;
                    }
                }
                if (!strncmp(w, "comment", 7) && !isalnum((unsigned char)w[7]) && w[7] != '_') {   /* [[comment("...")]] */
                    const char *m = w + 7;
                    while (*m == ' ' || *m == '\t' || *m == '(') m++;
                    if (*m != '"') error("[[comment(\"text\")]]: a string expected");
                    const char *b0 = ++m;
                    for (; *m && *m != '"'; m++) if (*m == '\\' && m[1]) m++;
                    size_t n = (size_t)(m - b0);               /* kept as written: escapes and all */
                    attr_meta.comment = cc_malloc(n + 1);
                    memcpy(attr_meta.comment, b0, n);
                    attr_meta.comment[n] = '\0';
                    attr_meta_used = 1;
                }
                if (!strncmp(w, "__c2_array", 10)) attr_container = C2_CK_ARRAY;   /* runtime/: array(T) is T * [[__c2_array]] */
                if (!strncmp(w, "__c2_map", 8)) attr_container = C2_CK_MAP;
                if (!strncmp(w, "__c2_set", 8)) attr_container = C2_CK_SET;
                if (!strncmp(w, "unroll", 6) && !isalnum((unsigned char)w[6]) && w[6] != '_') {   /* [[unroll]] [[unroll(N)]] */
                    const char *o = w + 6;
                    while (*o == ' ' || *o == '\t') o++;
                    attr_unroll = -1;
                    if (*o == '(') {
                        for (o++; *o == ' ' || *o == '\t'; o++) {}
                        long n = strtol(o, NULL, 0);
                        if (!isdigit((unsigned char)*o) || n < 1 || n > 1024) error("[[unroll(N)]]: N is a number from 1 to 1024");
                        attr_unroll = (int)n;
                    }
                }
                if (!strncmp(w, "persist", 7) && !isalnum((unsigned char)w[7]) && w[7] != '_') attr_persist = 1;
                if (!strncmp(w, "destructor", 10) && !isalnum((unsigned char)w[10]) && w[10] != '_') attr_destructor = 1;
                if (!strncmp(w, "copy", 4) && !isalnum((unsigned char)w[4]) && w[4] != '_') attr_copy = 1;
                if ((!strncmp(w, "noncommutative", 14) && !isalnum((unsigned char)w[14]) && w[14] != '_') ||
                    (!strncmp(w, "nocommutative", 13) && !isalnum((unsigned char)w[13]) && w[13] != '_')) attr_noncomm = 1;
                if (!strncmp(w, "operator", 8) && !isalnum((unsigned char)w[8]) && w[8] != '_') {   /* [[operator+=]] */
                    const char *o = w + 8;
                    int k = 0;
                    while (*o == ' ' || *o == '\t') o++;
                    if (*o == '[') {                         /* [[operator[]]] */
                        attr_operator_text[k++] = '[';
                        for (o++; *o == ' ' || *o == '\t'; o++) {}
                        if (*o == ']') { attr_operator_text[k++] = ']'; o++; }
                    } else
                        for (; *o && *o != ']' && *o != ',' && *o != '(' && k < (int)sizeof attr_operator_text - 1; o++)
                            if (*o != ' ' && *o != '\t') attr_operator_text[k++] = *o;
                    attr_operator_text[k] = '\0';
                    attr_operator = -1;
                    for (int i = 1; i < OPR_NEG; i++) if (!strcmp(attr_operator_text, opr_text[i])) attr_operator = i;
                    w = o - 1;                               /* (the ']' of [] isn't the attribute's end) */
                }
                if (!strncmp(w, "export", 6) && !isalnum((unsigned char)w[6]) && w[6] != '_') attr_export = 1;
                if (!strncmp(w, "deprecated", 10)) {
                    const char *m = w + 10;
                    while (*m == ' ' || *m == '(') m++;
                    attr_deprecated[0] = ' ';                /* deprecated, no reason */
                    attr_deprecated[1] = '\0';
                    if (*m == '"') {                         /* deprecated("reason") */
                        int k = 0;
                        for (m++; *m && *m != '"' && k < (int)sizeof attr_deprecated - 1; m++) {
                            if (*m == '\\' && m[1]) m++;
                            attr_deprecated[k++] = *m;
                        }
                        attr_deprecated[k] = '\0';
                    }
                }
            }
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
        if (!strncmp(buf, "__c2_module_begin_", 18)) {  /* #import markers */
            if (mod_sp < 16) mod_stack[mod_sp++] = atoi(buf + 18);
            next_token();
            return;
        }
        if (!strcmp(buf, "__c2_module_end")) { if (mod_sp) mod_sp--; next_token(); return; }
        if (c2_ext && prev_tok_type != TOKEN_DOT && prev_tok_type != TOKEN_ARROW) {
            for (int ai = 0; ai < nmod_aliases; ai++) {  /* alias.name: an exported name of an import */
                int m = mod_aliases[ai].module;
                if (strcmp(mod_aliases[ai].name, buf)) continue;
                const char *q = src;
                while (*q == ' ' || *q == '\t') q++;
                if (*q != '.') break;
                q++;
                while (*q == ' ' || *q == '\t') q++;
                if (!(isalpha((unsigned char)*q) || *q == '_')) break;
                char nm[64], tf[130];                    /* doc.parse(): a type doc's doc_parse, when the */
                int k = 0;                               /* module doc has no parse of its own */
                for (const char *r = q; (isalnum((unsigned char)*r) || *r == '_') && k < 63; r++) nm[k++] = *r;
                nm[k] = '\0';
                snprintf(tf, sizeof tf, "%s_%s", buf, nm);
                if (name_is_func(tf) && !name_is_func(nm)) break;
                i = 0;
                while ((isalnum((unsigned char)*q) || *q == '_') && i < 63) buf[i++] = *q++;
                buf[i] = '\0';
                src = q;
                if (private_name(m, buf)) error("'%s' is private to module '%s' (not [[export]]ed)", buf, module_alias[m]);
                break;
            }
        }
        if (!strcmp(buf, "__c2_ms_struct_on") || !strcmp(buf, "__c2_ms_struct_off")) {   /* #pragma ms_struct */
            ms_struct_pragma = !strcmp(buf, "__c2_ms_struct_on");
            next_token();
            return;
        }
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
        if (mod_sp && prev_tok_type != TOKEN_DOT && prev_tok_type != TOKEN_ARROW) {
            const char *pn = private_name(cur_module(), buf);   /* a private name of this module */
            if (pn) snprintf(buf, sizeof buf, "%s", pn);
        }
        /* C2y: _Countof (countof with <stdcountof.h>), defer TS: _Defer (defer with <stddefer.h>);
           .c2 files get countof and defer as keywords */
        if (!strcmp(buf, "_Countof") || (c2_ext && !strcmp(buf, "countof"))) { current_token.type = TOKEN_COUNTOF; return; }
        if (!strcmp(buf, "_Defer") || (c2_ext && !strcmp(buf, "defer"))) { current_token.type = TOKEN_DEFER; return; }
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
typedef struct { const char *src; Token tok; int line, ms, mod_sp, mod_stack[16]; } LexState;
static LexState save_lex(void) {
    LexState s = {src, current_token, cur_line, ms_struct_pragma, mod_sp, {0}};
    memcpy(s.mod_stack, mod_stack, sizeof mod_stack);
    return s;
}
static void restore_lex(LexState s) {
    src = s.src; current_token = s.tok; cur_line = s.line; ms_struct_pragma = s.ms;
    mod_sp = s.mod_sp; memcpy(mod_stack, s.mod_stack, sizeof mod_stack);
}

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
    "void *__c2_memset(void *p, int c, size_t n);\n"      /* used for zero-initialisation */
    "int __c2_arith_overflow(int op, unsigned long long a, int as, unsigned long long b, int bs, void *r, int rsize, int rs);\n";

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
     "float sinhf(float x); float coshf(float x); float tanhf(float x); float asinhf(float x);\n"
     "float acoshf(float x); float atanhf(float x); float expm1f(float x); float log1pf(float x);\n"
     "float logbf(float x); float erff(float x); float erfcf(float x); float lgammaf(float x);\n"
     "float tgammaf(float x); float nearbyintf(float x); float rintf(float x);\n"
     "float remainderf(float x, float y); float nextafterf(float x, float y);\n"
     "float fdimf(float x, float y); int ilogbf(float x); float frexpf(float x, int *e);\n"
     "float ldexpf(float x, int e); float modff(float x, float *ip); float scalbnf(float x, int n);\n"
     "float scalblnf(float x, long n); long long llrintf(float x); long long llroundf(float x);\n"
     "long lrintf(float x); long lroundf(float x); float remquof(float x, float y, int *q);\n"
     "float fmaf(float x, float y, float z); float nexttowardf(float x, double y);\n"
     "#define FP_NAN 0\n#define FP_INFINITE 1\n#define FP_ZERO 2\n#define FP_SUBNORMAL 3\n#define FP_NORMAL 4\n"
     "#define fpclassify(x) (isnan(x) ? FP_NAN : isinf(x) ? FP_INFINITE : (x) == 0 ? FP_ZERO : \\\n"
     "    (sizeof(x) == sizeof(float) ? fabsf(x) < 1.17549435e-38F : fabs(x) < 2.2250738585072014e-308) ? FP_SUBNORMAL : FP_NORMAL)\n"
     "int __c2_signbit(double x);\n"
     "#define HUGE_VALF __c2_huge_valf()\n#define HUGE_VALL __c2_huge_val()\n#define isnan(x) ((x) != (x))\n"
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
    {"stdcountof.h", "#define countof _Countof\n"},
    {"stddefer.h", "#define defer _Defer\n"},
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

static PPTok *pp_prev;     /* the token before the one being expanded (the main loop's and expand_all's) */
static int expand_macro(PPTok **rest, PPTok *tok);

static PPTok *expand_all(PPTok *t) {   /* fully expand a token list (macro arguments) */
    PPTok head = {0}, *cur = &head;
    while (t->kind != PK_EOF) {
        pp_prev = cur == &head ? NULL : cur;
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
    if (c2_ext && m->is_func && pp_prev && (is_punct(pp_prev, ".") || is_punct(pp_prev, "->")))
        return 0;                                  /* .c2: x.set(k, v) is a method, not the set() macro */
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
        if (is_ident(p, "__has_attribute") || is_ident(p, "__has_c_attribute") || is_ident(p, "__has_builtin") ||
            is_ident(p, "__has_feature") || is_ident(p, "__has_extension")) {   /* (name): what C² supports */
            PPTok *start = p;
            p = p->next;
            if (!is_punct(p, "(")) pp_error(start, "'(' expected after %s", start->s);
            char name[128] = "";
            for (p = p->next; p->kind != PK_EOF && !is_punct(p, ")"); p = p->next)   /* gnu::x, x */
                strncat(name, p->s, sizeof name - strlen(name) - 1);
            if (!is_punct(p, ")")) pp_error(start, "')' expected after %s(...", start->s);
            const char *n = name;
            if (!strncmp(n, "gnu::", 5) || !strncmp(n, "clang::", 7)) n = strchr(n, ':') + 2;
            size_t nl = strlen(n);                     /* __x__ is x */
            char bare[128];
            if (nl > 4 && !strncmp(n, "__", 2) && !strcmp(n + nl - 2, "__")) { snprintf(bare, sizeof bare, "%.*s", (int)nl - 4, n + 2); n = bare; }
            const char **attrs = known_attrs;
            static const char *builtins[] = {"__builtin_va_start", "__builtin_va_arg", "__builtin_va_end", "__builtin_va_copy",
                                             "__builtin_add_overflow", "__builtin_sub_overflow", "__builtin_mul_overflow", NULL};
            const char **list = start->s[6] == 'b' ? builtins : start->s[6] == 'f' || start->s[6] == 'e' ? NULL : attrs;
            const char *v = "0";
            for (int i = 0; list && list[i]; i++)
                if (!strcmp(list[i], list == builtins ? name : n)) v = start->s[6] == 'c' ? "202311L" : "1";
            cur = cur->next = new_tok(PK_NUM, v, (int)strlen(v), start);
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

static const char *script_library(const char *name);
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
            pp_prev = cur;
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
        } else if (!strcmp(d, "import")) {          /* C²: #import "dir/name", or #import alias = "dir/name" */
            if (!c2_ext) pp_error(dir, "#import needs a .c2 file");
            char name[1024], path[1200], alias[32] = "";
            if (t->kind == PK_IDENT && is_punct(t->next, "=") && !t->next->bol) {   /* the name first */
                snprintf(alias, sizeof alias, "%s", t->s);
                t = t->next->next;
                if (t->kind != PK_STR || t->bol) pp_error(dir, "#import %s = \"module\": the module's path expected", alias);
            }
            if (t->kind != PK_STR) pp_error(dir, "#import expects \"module\" or name = \"module\" (a path without .c2)");
            snprintf(name, sizeof name, "%.*s.c2", t->len - 2, t->s + 1);
            const char *base = strrchr(name, '/') ? strrchr(name, '/') + 1 : name;
            if (!alias[0]) snprintf(alias, sizeof alias, "%.*s", (int)(strlen(base) - 3), base);
            PPTok *a = t->next;
            if (is_ident(a, "as") && !a->bol)
                pp_error(dir, "#import \"...\" as name is now #import name = \"...\"");
            else if (a->kind != PK_EOF && !a->bol) pp_error(dir, "unexpected '%s' after #import \"...\"", a->s);
            next = skip_line(t);
            char *text = find_file(dir, name, 1, path, sizeof path, NULL);
            if (!text) {                           /* "x/y": x/y.c2, then the folder: x/y/y.c2, x/y/index.c2 */
                char alt[1200];
                snprintf(alt, sizeof alt, "%.*s/%s", t->len - 2, t->s + 1, base);
                text = find_file(dir, alt, 1, path, sizeof path, NULL);
                if (!text) {
                    snprintf(alt, sizeof alt, "%.*s/index.c2", t->len - 2, t->s + 1);
                    text = find_file(dir, alt, 1, path, sizeof path, NULL);
                }
            }
            if (!text) pp_error(dir, "cannot find module \"%.*s\" (%s, %.*s/%s or %.*s/index.c2)", t->len - 2, t->s + 1,
                                name, t->len - 2, t->s + 1, base, t->len - 2, t->s + 1);
            char full[4096];                       /* one module, however it was reached */
#ifdef _WIN32
            if (!_fullpath(full, path, sizeof full)) snprintf(full, sizeof full, "%s", path);
#else
            if (!realpath(path, full)) snprintf(full, sizeof full, "%s", path);
#endif
            int seen = -1;
            for (int i = 0; i < nmodules; i++) if (!strcmp(module_path[i], full)) seen = i;
            if (seen >= 0) { add_mod_alias(alias, seen); free(text); tok = next; continue; }   /* imported once */
            if (nmodules >= 64) pp_error(dir, "too many modules");
            int m = nmodules++;
            snprintf(module_path[m], sizeof module_path[m], "%s", full);
            snprintf(module_alias[m], sizeof module_alias[m], "%.*s", (int)(strlen(base) - 3), base);
            add_mod_alias(alias, m);
            PPTok *body = pp_tokenize(text, add_pp_file(path));
            free(text);
            char mk[40];
            snprintf(mk, sizeof mk, "__c2_module_begin_%d", m);
            PPTok *b = new_tok(PK_IDENT, mk, (int)strlen(mk), dir), *e = new_tok(PK_IDENT, "__c2_module_end", 15, dir);
            b->bol = e->bol = 1;
            b->next = body;
            PPTok *last = b;
            while (last->next->kind != PK_EOF) last = last->next;
            last->next = e;
            e->next = next;
            tok = b;
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
            if (is_ident(t, "comment") && is_punct(t->next, "(") && is_ident(t->next->next, "lib")) {   /* #pragma comment(lib, "SDL3") */
                PPTok *n = t->next->next->next;
                if (!is_punct(n, ",") || n->next->kind != PK_STR || !is_punct(n->next->next, ")"))
                    pp_error(t, "#pragma comment(lib, \"name\") wants a library name");
                char name[1024];
                snprintf(name, sizeof name, "%.*s", n->next->len - 2, n->next->s + 1);
                const char *err = script_library(name);
                if (*err) pp_error(t, "%s", err);
            }
            if (is_ident(t, "ms_struct") && t->next->kind == PK_IDENT && !t->next->bol) {   /* on | off | reset */
                const char *m = !strcmp(t->next->s, "on") ? "__c2_ms_struct_on" : "__c2_ms_struct_off";
                PPTok *k = new_tok(PK_IDENT, m, (int)strlen(m), dir);   /* for the parser */
                k->bol = 1;
                cur = cur->next = k;
            }
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
        {"__has_attribute", "__has_attribute"}, {"__has_c_attribute", "__has_c_attribute"},
        {"__has_builtin", "__has_builtin"}, {"__has_feature", "__has_feature"}, {"__has_extension", "__has_extension"},
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
    X(SWITCH, "switch") X(LADDR, "laddr") X(JMPI, "jmpi")

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
static ItemEnt items[16384];
static int nitems;
static int item_hash[32768];                       /* index + 1 into items, by name (open addressing) */
static unsigned item_slot(const char *name) {
    unsigned h = 2166136261u;
    for (const char *p = name; *p; p++) h = (h ^ (unsigned char)*p) * 16777619u;
    h &= 32767;
    while (item_hash[h] && strcmp(items[item_hash[h] - 1].name, name)) h = (h + 1) & 32767;
    return h;
}

static void register_item(const char *name, MIR_item_t it) {
    unsigned h = item_slot(name);
    if (item_hash[h]) { items[item_hash[h] - 1].item = it; return; }
    if (nitems >= 16384) error("too many items");
    snprintf(items[nitems].name, sizeof items[nitems].name, "%s", name);
    items[nitems++].item = it;
    if (strlen(name) < sizeof items[0].name) item_hash[h] = nitems;   /* (a longer name is never found, as before) */
}

static MIR_item_t find_item(const char *name) {
    unsigned h = item_slot(name);
    if (item_hash[h]) return items[item_hash[h] - 1].item;
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
/* c2 -C: each instruction remembers the line it came from (insn->data), for #line in the C output */
typedef struct SrcLoc { const char *file; int line; struct SrcLoc *next; } SrcLoc;
static int c_lines_on;
static SrcLoc *src_locs, *src_loc_last;
static char *src_files[64];                    /* the file names seen, each once */
static int nsrc_files;
static SrcLoc *src_loc_now(void) {
    if (src_loc_last && src_loc_last->line == cur_line && !strcmp(src_loc_last->file, cur_file)) return src_loc_last;
    const char *f = NULL;
    for (int i = 0; i < nsrc_files && !f; i++) if (!strcmp(src_files[i], cur_file)) f = src_files[i];
    if (!f && nsrc_files < 64 && (src_files[nsrc_files] = (char *)(malloc)(strlen(cur_file) + 1)))   /* (not the per-compile heap) */
        f = strcpy(src_files[nsrc_files++], cur_file);
    SrcLoc *l = (SrcLoc *)(malloc)(sizeof *l);
    if (!l || !f) { (free)(l); return NULL; }
    l->file = f; l->line = cur_line; l->next = src_locs;
    return src_locs = src_loc_last = l;
}
static void src_locs_free(void) {
    while (src_locs) { SrcLoc *n = src_locs->next; (free)(src_locs); src_locs = n; }
    src_loc_last = NULL;
    while (nsrc_files) (free)(src_files[--nsrc_files]);
}

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
        if (c_lines_on) insn->data = src_loc_now();
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
/* temporaries (.c2): a call returning a T by value, T with a [[destructor]], leaves a temporary that is
   destroyed at the end of its statement, unless it was moved: stored (an initializer, `=`), returned, or
   the value of ({ }).  Passing it to a function lends it.  Its slot is zeroed at function entry and
   after each destruction, so one made on a path not taken (?:, &&) destroys a zero T. */
static int ntemps, ndtors;

static int new_slot(const char *name, int size) {
    int r = new_reg(name, RK_I, 0);
    EMIT_PROLOGUE(ALLOCA, R(r), IMM(size < 8 ? 8 : (size + 7) / 8 * 8));
    return r;
}

/* lexer-independent "dry run": sizeof expr and ?: look at an expression's type without keeping
   its code.  Text mode truncates the buffers; API mode removes the instructions. */
typedef struct {
    size_t body_len, pro_len;
    int nregs, temp_counter, ntemps;
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
    m.ntemps = ntemps;
#ifndef NO_MIR
    if (use_api && cur_fn) m.tail = DLIST_TAIL(MIR_insn_t, cur_fn->u.func->insns);
#endif
    return m;
}

static void emit_rollback(EmitMark m) {
    last_cmp.valid = 0;
    ntemps = m.ntemps;                                 /* temporaries of the dropped code are gone too */
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
typedef struct { unsigned char kind, cq, ck; int n; } Deriv;   /* ARR: length (-1 = unknown); FUNC: sig index;
                                                                 ck: PTR of a runtime container (C2_CK_*) */
#define MAX_DERIV 8
/* bfw/bfp: a bit-field member's width and bit position inside its storage unit (an object of the
   declared type at the member's offset); 0 width = not a bit-field */
typedef struct { Base base; unsigned char uns, bcq, bfw, bfp; short nd, eid; int sidx; Deriv d[MAX_DERIV]; } Type;
/* eid: an enum type's index + 1 in enum_infos (reflection: its name and values); 0 for other types */

typedef struct Member { char name[32]; Type ty; int off; unsigned char skip; const char *rename; const FieldMeta *meta; } Member;   /* name "" = anonymous struct/union member;
                                                                    skip: [[skip]] (reflection: left out of dump and saves) */
typedef struct { char name[32]; Member m[64]; int n, size, align, complete, is_union, depth, hidden, skip; } Struct;   /* skip: [[skip]] */
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
    t.d[t.nd].ck = 0;
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
typedef struct EnumTag { char name[32]; int depth; Type base; } EnumTag;
static EnumTag enum_tags[256];
/* every enum with a body, for reflection (not scoped: a type outlives the block that declared it) */
typedef struct { char name[32]; Type base; int first, n, skip; } EnumInfo;
typedef struct { char name[32]; long long val; } EnumVal;
static EnumInfo enum_infos[1024];
static int nenum_infos;
static EnumVal enum_vals[8192];
static int nenum_vals;
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
                 int uninit, serial, dbg; } Var;   /* serial: the order local declarations ran (goto checks);
                                                    dbg: 1 + its entry in the -g frame, 0 if none */
static int decl_serial;   /* global >= 0: static local; sra: members kept in registers;
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
static void dbg_vars_end(int live);
static void leave_scope(void) {
    while (nvla_scopes > 0 && vla_scopes[nvla_scopes - 1].depth == scope_depth)
        EMIT(BEND, R(vla_scopes[--nvla_scopes].save_reg));
    int live = -1;                                     /* -g: the frame forgets this block's variables */
    while (nvars > 0 && vars[nvars - 1].depth == scope_depth) { if (vars[nvars - 1].dbg) live = vars[nvars - 1].dbg - 1; nvars--; }
    if (live >= 0) dbg_vars_end(live);
    hide_structs(scope_depth);                     /* struct tags declared in this block */
    while (nenum_consts > 0 && enum_consts[nenum_consts - 1].depth >= scope_depth) nenum_consts--;
    while (nenum_tags > 0 && enum_tags[nenum_tags - 1].depth >= scope_depth) nenum_tags--;
    while (nfunc_aliases > 0 && func_aliases[nfunc_aliases - 1].depth >= scope_depth) nfunc_aliases--;
    scope_depth--;
}

typedef struct { char name[40]; Type ty; int has_init, deferred, persist, imported, fwd, skip; } Global;   /* deferred: struct completed later */
static Global globals[1024];
static int nglobals;

static Global *find_global(const char *name);
static int name_is_func(const char *name);
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
    int nodiscard;                     /* [[nodiscard]] */
    char *deprecated;                  /* [[deprecated]]: " " or the reason; NULL if not */
    int warned_line;                   /* one deprecation warning per line */
    int in_module;                     /* defined in an #import'ed module (its index + 1); 0: the program's own */
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
static int name_is_func(const char *name) { return find_func(name) != NULL; }

static int name_is_item(const char *name) {
    if (find_global(name) || find_func(name)) return 1;
    if (strncmp(name, "proto_", 6) == 0 && find_func(name + 6)) return 1;
    return strncmp(name, "_str", 4) == 0 || strncmp(name, "__c2_", 5) == 0 || strncmp(name, "proto_", 6) == 0;
}

/* names whose address is taken in the current function body: these scalars need a stack slot */
static char addr_taken[512][32];
static int naddr_taken;

static int is_addr_taken(const char *name) {
    name = demangle(name);                         /* the scan sees the source's names */
    for (int i = 0; i < naddr_taken; i++)
        if (strcmp(addr_taken[i], name) == 0) return 1;
    return 0;
}

static void prescan_addr_taken_ex(const char *s, int reset);
static void prescan_addr_taken(const char *s) { prescan_addr_taken_ex(s, 1); }
static void prescan_addr_taken_ex(const char *s, int reset) {   /* s points just after the function's '{' */
    int depth = 1;
    if (reset) naddr_taken = 0;
    for (; *s && depth > 0; s++) {
        if (*s == '"' || *s == '\'') {         /* skip literals */
            char q = *s++;
            while (*s && *s != q) { if (*s == '\\' && s[1]) s++; s++; }
            if (!*s) break;
            continue;
        }
        if (*s == '{') depth++;
        else if (*s == '}') depth--;
        else if (c2_ext && (isalpha((unsigned char)*s) || *s == '_') && !(isalnum((unsigned char)s[-1]) || s[-1] == '_' || s[-1] == '.')
                 && strncmp(s, "__builtin_va_", 13) != 0) {   /* (va_start & co: below) */
            const char *q = s;                         /* .c2: name.f( may pass &name (method call) */
            char name[32];
            int n = 0;
            while ((isalnum((unsigned char)*q) || *q == '_') && n < 31) name[n++] = *q++;
            name[n] = '\0';
            const char *r = q;
            while (isspace((unsigned char)*r)) r++;
            if (!strcmp(name, "foreach_field") && *r == '(') {   /* what foreach_field(T, X) expands to */
                const char *c = r;
                while (*c && *c != ',' && *c != ')' && *c != ';') c++;
                if (*c == ',') {
                    c++;
                    while (isspace((unsigned char)*c)) c++;
                    char mac[64], text[100];
                    int k = 0;
                    while ((isalnum((unsigned char)*c) || *c == '_') && k < 63) mac[k++] = *c++;
                    mac[k] = '\0';
                    snprintf(text, sizeof text, "%s(__c2_field)", mac);
                    Buf eb = {0};
                    for (PPTok *t = expand_all(pp_tokenize(text, 0)); t->kind != PK_EOF; t = t->next) {
                        buf_append(&eb, " ", 1);
                        buf_append(&eb, t->s, t->len);
                    }
                    buf_append(&eb, "", 0);
                    prescan_addr_taken_ex(eb.p, 0);
                }
            }
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
            while (isspace((unsigned char)*q) || *q == '(') q++;   /* &(a): macros parenthesize */
            if (!(isalpha((unsigned char)*q) || *q == '_')) continue;
            char name[32];
            int n = 0;
            while ((isalnum((unsigned char)*q) || *q == '_') && n < 31) name[n++] = *q++;
            name[n] = '\0';
            while (isspace((unsigned char)*q) || *q == ')') q++;
            /* &a[i], &p->f don't need `a`/`p` itself in memory; &s.f does when s's members would live in
               registers (a small struct local), so s is noted */
            if (*q == '[' || (q[0] == '-' && q[1] == '>')) continue;
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
        buf_emit(&out_text, "\\000");    /* the terminator, explicitly: don't rely on what follows the data */
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
        if ((mt == MT_D || mt == MT_F) && !isfinite(dv)) {   /* MIR text has no nan/inf: the bits */
            union { float f; uint32_t u; double d; uint64_t q; } u;
            if (mt == MT_F) { u.f = (float)dv; buf_emit(&out_text, "%s: u32 %u\n", name, u.u); }
            else { u.d = dv; buf_emit(&out_text, "%s: u64 %llu\n", name, (unsigned long long)u.q); }
            return;
        }
        if (mt == MT_D) fmt_double(b, dv);
        else if (mt == MT_F) { sprintf(b, "%.9g", (float)dv); if (!strpbrk(b, ".eEni")) strcat(b, ".0"); strcat(b, "f"); }   /* MIR text: 1.5f */
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
static int implicit_ctor(Val v, Type to, Val *out);
static Func *implicit_ctor_for(Type from, int null_const, Type to);
static const char *struct_name(int sidx);
/* .c2: `any` (runtime/reflect) is a value and its type.  A parameter of type any takes any value: the
   compiler passes (any){reflect(T), &x} (x itself when it is an lvalue, else a copy) */
static const char *struct_name(int sidx);
static int is_any_type(Type t) {
    if (!is_struct_val(t) || t.nd) return 0;
    const char *n = struct_name(t.sidx);
    return n && !strcmp(n, "any");
}
static Val reflect_of(Type t);
static Val store(Val lv, Val rhs);
static Val make_any(Val v, Type any_t) {
    if (is_func(v.ty) || is_void(v.ty)) error("a %s can't be passed as any", type_name(v.ty));
    Val addr;
    if (v.k == V_MEM && !v.ty.bfw) addr = rvalue(address_of(v));
    else {                                            /* a value, a bit-field or a register local: stored, pointed to */
        Type vt = v.ty;
        vt.bfw = vt.bfp = 0;
        Val r = rvalue(v);
        int slot = new_slot("_anyv", size_of(vt) > 8 ? size_of(vt) : 8);
        store(lv_mem(slot, 0, vt), r);
        addr = rvalue(address_of(lv_mem(slot, 0, vt)));
    }
    Val t = reflect_of(v.ty);
    int base = new_slot("_any", size_of(any_t));
    for (int off = 0; off + 8 <= size_of(any_t); off += 8) EMIT(MOV, MEM(MT_I64, off, base), IMM(0));
    EMIT(MOV, MEM(MT_I64, 0, base), t.op);
    EMIT(MOV, MEM(MT_I64, 8, base), addr.op);
    return lv_mem(base, 0, any_t);
}

static Val convert_ex(Val v, Type to, int is_explicit) {
    if (c2_ext && is_any_type(to) && !is_any_type(v.ty) && v.k == V_MEM && is_struct_val(v.ty)) return make_any(v, to);
    v = rvalue(v);
    if (c2_ext && is_any_type(to) && !is_any_type(v.ty)) return make_any(v, to);
    if (is_void(to)) { v.ty = to; return v; }
    if (is_struct_val(to) || is_struct_val(v.ty)) {
        if (is_struct_val(to) && is_struct_val(v.ty) && to.sidx == v.ty.sidx) { v.ty = to; return v; }
        if (is_struct_val(to) && !is_struct_val(v.ty)) {   /* .c2: string s = "hi" is string("hi") */
            Val r;
            if (implicit_ctor(v, to, &r)) return r;
        }
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
static int unwind_on, no_unwind_check, in_defer;   /* try/throw (see unwind_check) */
static int sandbox_on;                               /* C2_sandbox: exit() and abort() unwind like a throw */
static int sandbox_unwinds(const char *n) {          /* calls after which a sandboxed program checks __c2_throwing */
    static const char *names[] = {"exit", "_exit", "_Exit", "quick_exit", "abort", "try", "__c2_try", "co_resume", "__c2_co_resume", NULL};
    if (!sandbox_on) return 0;
    for (int i = 0; names[i]; i++) if (!strcmp(n, names[i])) return 1;
    return 0;
}
static int call_may_throw(const char *fname, int is_import);
static void temp_moved(Val v);
static int temp_index(Val v);
static int ncopies;
static Func *dtor_for(Type t);
static Val gen_call(Func *f, Val *args, int n);
static Val copy_if_named(Val v);
/* [[pure]] checked: a pure function that writes a global or calls a function that isn't pure gets a
   warning (once per name).  A pure function may write its own locals, and read anything. */
static int pure_on, npure_regs;
static struct { int reg; const char *name; } pure_regs[64];   /* registers holding a global's address */
static char pure_warned[16][40];
static int npure_warned;
static int pure_known(const char *n) {                 /* the C library's pure functions */
    static const char *names[] = {"sin", "cos", "tan", "asin", "acos", "atan", "atan2", "sinh", "cosh", "tanh",
        "asinh", "acosh", "atanh", "exp", "exp2", "expm1", "log", "log2", "log10", "log1p", "pow", "sqrt", "cbrt",
        "hypot", "floor", "ceil", "round", "trunc", "lround", "llround", "rint", "nearbyint", "fmod", "remainder",
        "fabs", "fmin", "fmax", "fdim", "fma", "copysign", "ldexp", "frexp", "modf", "isnan", "isinf", "isfinite",
        "signbit", "abs", "labs", "llabs", "strlen", "strnlen", "strcmp", "strncmp", "memcmp", "strchr", "strrchr",
        "strstr", "strspn", "strcspn", "strpbrk", "memchr", "isalpha", "isdigit", "isalnum", "isspace", "isupper",
        "islower", "isxdigit", "ispunct", "isprint", "iscntrl", "isgraph", "toupper", "tolower", "atoi", "atol",
        "atoll", "atof", "strtol", "strtoll", "strtoul", "strtoull", "strtod", "strtof", NULL};
    size_t len = strlen(n);
    for (int i = 0; names[i]; i++) {                   /* sinf, sinl too */
        size_t k = strlen(names[i]);
        if (!strncmp(n, names[i], k) && (len == k || (len == k + 1 && (n[k] == 'f' || n[k] == 'l')))) return 1;
    }
    return 0;
}
static void pure_warn(const char *what, const char *name) {
    for (int i = 0; i < npure_warned; i++) if (!strcmp(pure_warned[i], name)) return;
    if (npure_warned < 16) snprintf(pure_warned[npure_warned++], 40, "%s", name);
    diag("%s:%d: warning: [[pure]] function '%s' %s '%s'\n", cur_file, cur_line, cur_func->name, what, demangle(name));
}
static Val store_ex(Val lv, Val rhs, int init) {
    if (pure_on && lv.k == V_MEM)                       /* [[pure]]: a write to a global */
        for (int i = 0; i < npure_regs; i++) if (pure_regs[i].reg == lv.base) { pure_warn("writes the global", pure_regs[i].name); break; }
    if (ncopies && is_struct_val(rhs.ty)) rhs = copy_if_named(rhs);   /* [[copy]]: b = a stores a copy of a */
    if (c2_ext && is_struct_val(lv.ty) && !is_struct_val(rhs.ty) && !lv.ty.bfw)   /* string s = "hi": the constructor's */
        rhs = convert(rhs, unconst(lv.ty));             /* temporary, made before it is claimed below */
    if (ntemps && temp_index(rhs) >= 0) {              /* a temporary stored: the variable owns it now */
        Func *d = init ? NULL : dtor_for(lv.ty);
        temp_moved(rhs);
        if (d && lv.k == V_MEM) {                      /* s = s + "x": the old value is freed first */
            Val a = address_of(lv);
            int keep = ntemps;
            ntemps = 0;
            no_unwind_check++;
            gen_call(d, &a, 1);
            no_unwind_check--;
            ntemps = keep;
        }
    }
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
    case TOKEN_COUNTOF: {
        next_token();
        Type ty;
        if (current_token.type == TOKEN_LPAREN && is_type_start(peek_token())) {
            next_token();
            ty = parse_type_name();
            expect(TOKEN_RPAREN);
        } else if (cur_func) {
            EmitMark em = emit_mark();
            no_uninit_warn++;
            ty = parse_unary().ty;
            no_uninit_warn--;
            emit_rollback(em);
        } else { ce_fail = 1; return cv_i(0); }
        if (!is_array(ty)) error("countof of '%s', which is not an array", type_name(ty));
        if (arr_len(ty) < 0) { ce_fail = 1; return cv_i(0); }   /* VLA / unknown length */
        return cv_i(arr_len(ty));
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
    if (si < 0) {                                  /* typedef struct { ... } Vec3;  Vec3(1, 2, 3) */
        Typedef *td = find_typedef(name);
        if (td && is_struct_val(td->ty) && !td->ty.nd) si = td->ty.sidx;
    }
    if (si < 0 || find_var(name) || find_global(name) || find_enum_const(name)) return -1;
    Func *f = find_func(name);                     /* T T(...): T's constructor, T stays a type */
    if (f && !(is_struct_val(f->ret) && f->ret.sidx == si)) return -1;
    return si;
}

static int ctor_ahead(void);
typedef struct EnumTag EnumTag;
static EnumTag *find_enum_tag(const char *name);
/* .c2: an enum's tag is a type name, as a struct's is: `Dir d;` for `enum Dir d;` */
static int implicit_enum(const char *name) {
    return c2_ext && find_enum_tag(name) && !find_var(name) && !find_global(name) && !find_func(name) &&
           !find_enum_const(name) && !find_typedef(name);
}
/* .c2: T(...) with T a struct type builds a T (its constructor function, or the fields in order /
   by name as in an initializer); a declaration T (*p)(...) still starts with T ( * */
static int paren_ctor_ahead(void) {               /* current: '(' ... is it (T(...)...)? */
    LexState ls = save_lex();
    next_token();
    int r = ctor_ahead();
    restore_lex(ls);
    return r;
}
static int ctor_ahead(void) {
    if (!c2_ext || current_token.type != TOKEN_IDENT || implicit_struct(current_token.text) < 0) return 0;
    LexState ls = save_lex();
    next_token();
    int r = 0;
    if (current_token.type == TOKEN_LPAREN) { next_token(); r = current_token.type != TOKEN_ASTERISK; }
    restore_lex(ls);
    return r;
}

static int is_type_start(Token t) {
    switch (t.type) {
    case TOKEN_INT: case TOKEN_CHAR: case TOKEN_SHORT: case TOKEN_LONG: case TOKEN_UNSIGNED: case TOKEN_SIGNED:
    case TOKEN_FLOAT: case TOKEN_DOUBLE: case TOKEN_VOID: case TOKEN_STRUCT: case TOKEN_UNION: case TOKEN_ENUM:
    case TOKEN_CONST: case TOKEN_STATIC: case TOKEN_EXTERN: case TOKEN_REGISTER: case TOKEN_BOOL: case TOKEN_ALIGNAS:
    case TOKEN_TYPEOF: case TOKEN_TYPEOF_UNQUAL: case TOKEN_AUTO: case TOKEN_CONSTEXPR:
        return 1;
    case TOKEN_IDENT: return (find_typedef(t.text) && !find_var(t.text)) || implicit_struct(t.text) >= 0 || implicit_enum(t.text);
    default: return 0;
    }
}

static Type parse_ptrs(Type t) {
    while (current_token.type == TOKEN_ASTERISK) {
        t = ptr_to(t);
        next_token();
        if (current_token.a_container) t.d[t.nd - 1].ck = current_token.a_container;   /* T * [[__c2_array]] */
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

static int pending_default_check;                   /* parse_params_in saw a default */
static int c2_source_file(void) {                  /* is the declaration being parsed in a .c2/.h2 file? */
    size_t n = strlen(cur_file);
    return n > 3 && (!strcmp(cur_file + n - 3, ".c2") || !strcmp(cur_file + n - 3, ".h2"));
}

static int parse_params_in(int si) {
    if (current_token.type == TOKEN_RPAREN) {          /* f(): unknown parameters in C, none in .c2 (C23, C++) */
        next_token();
        sigs[si].unprototyped = !c2_ext;
        return si;
    }
    if (current_token.type == TOKEN_VOID && peek_token().type == TOKEN_RPAREN) { next_token(); next_token(); return si; }
    if (current_token.type == TOKEN_IDENT && !is_type_start(current_token)) {   /* K&R: f(a, b) */
        Sig *g = &sigs[si];
        g->knr = 1;
        for (;;) {
            if (current_token.type != TOKEN_IDENT) error("parameter name expected");
            if (g->np >= MAX_PARAMS) error("too many parameters");
            g->pt[g->np] = int_type();
            fmt_to(g->pn[g->np++], 32, "%s", current_token.text);
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
        if (c2_ext && is_array(pt) && arr_len(pt) > 0 && !decl_saw_static && c2_source_file()) {   /* .c2: int a[3] means exactly 3 */
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
            pending_default_check = 1;
        } else if (g->pdef && g->np > 0 && g->pdef[g->np - 1]) {
            error("parameter '%s' needs a default value: it follows one that has one", pname);
        }
        g->np++;
        if (current_token.type != TOKEN_COMMA) break;
        next_token();
    }
    expect(TOKEN_RPAREN);
    if (pending_default_check) {                       /* a default may use only earlier parameters */
        pending_default_check = 0;
        Sig *g = &sigs[si];
        for (int i = 0; i < g->np; i++)
            for (const char *p = g->pdef ? g->pdef[i] : NULL; p && *p; p++) {
                if (!(isalpha((unsigned char)*p) || *p == '_') ||
                    (p > g->pdef[i] && (isalnum((unsigned char)p[-1]) || p[-1] == '_' || p[-1] == '.'))) continue;
                int n = 0;
                while (isalnum((unsigned char)p[n]) || p[n] == '_') n++;
                for (int j = i + 1; j < g->np; j++)
                    if ((int)strlen(g->pn[j]) == n && !strncmp(p, g->pn[j], n))
                        error("the default of '%s' uses '%s', a later parameter (defaults can use only earlier ones)", g->pn[i], g->pn[j]);
                p += n - 1;
            }
    }
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
                    if (len < 0) error("array size must not be negative");   /* [0]: GNU zero-length array */
                    if (len > 0x7fffffff) error("array too large");
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

/* the name a top-level declaration gets: in a module, without [[export]], a private one (in place) */
static int decl_exported;                          /* the current top-level declaration has [[export]] */
static int decl_skip;                              /* [[skip]] typedef ...: what it names isn't reflected */
static void module_decl_name(char *name, size_t size, int exported) {
    int m = cur_module();
    if (m < 0 || scope_depth != 0 || !name[0]) return;
    size_t fl = strlen(cur_file);                  /* a C header a module includes (<stdarg.h>): as in C, */
    if (fl > 2 && (!strcmp(cur_file + fl - 2, ".h") || !strcmp(cur_file + fl - 3, ".h>"))) return;   /* its names are everyone's (va_list) */
    if (strcmp(demangle(name), name)) {            /* already private (an earlier declaration) */
        if (exported) error("[[export]] must be on the first declaration of '%s'", demangle(name));
        return;
    }
    if (exported) return;
    if (strlen(name) + 8 > size) error("name too long for a private module name: '%s'", name);
    if (nprivates >= 2048) error("too many private names");
    privates[nprivates].module = m;
    snprintf(privates[nprivates++].name, sizeof privates[0].name, "%s", name);
    char tmp[64];
    snprintf(tmp, sizeof tmp, "__m%d_%s", m, name);
    fmt_to(name, size, "%s", tmp);
}

static int refl_string(Type t);
static void refl_name(Type t, char *out, size_t n);
static int is_char_array(Type t) { return is_array(t) && !elem_of(t).nd && elem_of(t).base == TY_CHAR; }
/* [[clamp]] fit the field: numbers, text (or lists of them: each item) */
static void field_meta_check(Member *m) {
    Type t = m->ty;
    for (;;) {
        if (is_array(t) && !is_char_array(t)) t = elem_of(t);
        else if (is_pointer(t) && t.d[t.nd - 1].ck == C2_CK_ARRAY) t = elem_of(t);
        else break;
    }
    char tn[200];
    refl_name(m->ty, tn, sizeof tn);
    if (m->meta->has_clamp && (!(is_integer(t) || is_fp(t)) || is_bool(t) || t.eid || t.nd))
        error("[[clamp(min, max)]] is for numbers, not %s ('%s'); text takes [[clamp(N)]], its most characters", tn, m->name);
    if (m->meta->maxlen && !refl_string(t) && !is_char_array(t))
        error("[[clamp(N)]] is for text (string, char [N]), not %s ('%s'); numbers take [[clamp(min, max)]]", tn, m->name);
}
static Type parse_struct_spec(void) {   /* current token: struct or union */
    int is_union = current_token.type == TOKEN_UNION;
    int skip = current_token.a_skip || decl_skip;       /* [[skip]] struct T, or [[skip]] typedef struct ... */
    next_token();
    if (current_token.a_skip) skip = 1;                  /* struct [[skip]] T */
    char name[32] = "";
    int exported = decl_exported || current_token.a_export;   /* struct [[export]] T or [[export]] struct T */
    if (current_token.type == TOKEN_IDENT) { snprintf(name, sizeof name, "%.31s", current_token.text); next_token(); }
    else if (current_token.type != TOKEN_LBRACE) error("%s name or '{' expected", is_union ? "union" : "struct");
    int si = name[0] ? find_struct(name) : -1;
    if (current_token.type == TOKEN_LBRACE ||
        (current_token.type == TOKEN_SEMI && name[0])) {   /* definition or `struct T;`: a tag in this scope */
        if (si >= 0 && structs[si].depth == scope_depth) {
            if (current_token.type == TOKEN_LBRACE && structs[si].complete)
                error("redefinition of %s %s", is_union ? "union" : "struct", name);
            if (exported) module_decl_name(name, sizeof name, 1);   /* (checks [[export]] comes first) */
        } else {
            module_decl_name(name, sizeof name, exported);
            si = new_struct(name, is_union);
        }
    } else if (si < 0) {
        module_decl_name(name, sizeof name, exported);
        si = new_struct(name, is_union);                 /* first mention: incomplete */
    }
    if (structs[si].is_union != is_union) error("'%s' defined as the wrong kind of tag", name);
    Type ty = mk_type(TY_STRUCT);
    ty.sidx = si;
    if (skip) structs[si].skip = 1;
    if (current_token.type != TOKEN_LBRACE) return ty;
    next_token();
    int off = 0, size = 0, align = 1;
    long long boff = 0;                                  /* bit-fields: next free bit */
    int ms_unit = -1, ms_size = 0, ms_used = 0;          /* MSVC: the open bit-field unit */
    int ms_layout = ms_struct_pragma;
    (void)ms_unit; (void)ms_size; (void)ms_used;
    while (current_token.type != TOKEN_RBRACE) {
        if (current_token.type == TOKEN_STATIC_ASSERT) { parse_static_assert(); continue; }
        if (current_token.type == TOKEN_SEMI) { next_token(); continue; }   /* a stray ';' (an empty macro) */
        int mskip = current_token.a_skip, mfirst = structs[si].n;          /* [[skip]] int secret; */
        const char *mrename = current_token.a_rename;                     /* [[rename("hp")]] int health; */
        const FieldMeta *mmeta = current_token.a_meta;                    /* [[clamp(0, 100)]] int hp; */
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
            memmove(mname, demangle(mname), strlen(demangle(mname)) + 1);   /* members are never module names */
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
                else if (ms_layout) {                  /* MSVC: units of the declared type's size */
                    if (w == 0) { ms_unit = -1; goto next_member; }
                    if (ms_unit < 0 || ms_size != sz || ms_used + w > sz * 8) {
                        int o = (int)((boff + 7) / 8);
                        o = (o + sz - 1) / sz * sz;
                        ms_unit = o; ms_size = sz; ms_used = 0;
                        boff = (long long)(o + sz) * 8;
                    }
                    unit = ms_unit; pos = ms_used; ms_used += (int)w;
                } else {                               /* SysV: no straddling of a unit of the type's size */
                    long long ub = (long long)sz * 8;
                    if (w == 0) { boff = (boff + ub - 1) / ub * ub; goto next_member; }
                    if (boff / ub != (boff + w - 1) / ub) boff = (boff + ub - 1) / ub * ub;
                    unit = (int)(boff / ub) * sz;
                    pos = (int)(boff - (long long)unit * 8);
                    boff += w;
                }
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
        for (int i = mfirst; i < structs[si].n; i++) {
            Member *mm = &structs[si].m[i];
            mm->skip = (unsigned char)mskip; mm->rename = mrename; mm->meta = mmeta;
            if (mmeta) field_meta_check(mm);
        }
        if (mrename && structs[si].n - mfirst != 1) error("[[rename]] names one field: declare it on its own");
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
    int enum_skip = current_token.a_skip || decl_skip;     /* [[skip]] enum E { ... } */
    next_token();
    if (current_token.a_skip) enum_skip = 1;               /* enum [[skip]] E { ... } */
    char tag[32] = "";
    Type base = int_type();
    int fixed = 0;
    int exported = decl_exported || current_token.a_export;
    if (current_token.type == TOKEN_IDENT) {
        snprintf(tag, sizeof tag, "%.31s", current_token.text);
        next_token();
        if (!find_enum_tag(tag)) module_decl_name(tag, sizeof tag, exported);
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
    long long v = 0, lo = 0, hi = 0;
    int first = nenum_consts;
    while (current_token.type != TOKEN_RBRACE) {
        if (current_token.type != TOKEN_IDENT) error("enumerator name expected");
        char name[32];
        snprintf(name, sizeof name, "%.31s", current_token.text);
        module_decl_name(name, sizeof name, exported);
        next_token();
        if (current_token.type == TOKEN_ASSIGN) { next_token(); v = const_int_expr(); }
        if (fixed && trunc_const(v, base) != v) error("enumerator '%s' value %lld does not fit the enum's type", name, v);
        if (nenum_consts == first || v < lo) lo = v;
        if (nenum_consts == first || v > hi) hi = v;
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
    if (!fixed && (lo < INT_MIN || hi > INT_MAX)) {   /* C23 / gcc: the first of int, unsigned, long long that fits */
        if (lo >= 0 && hi <= UINT_MAX) { base = int_type(); base.uns = 1; }
        else base = long_type();
    }
    if (nenum_infos < 1024 && nenum_vals + (nenum_consts - first) <= 8192) {   /* reflection: its name and values */
        EnumInfo *ei = &enum_infos[nenum_infos];
        snprintf(ei->name, sizeof ei->name, "%s", tag);
        ei->first = nenum_vals;
        ei->n = nenum_consts - first;
        ei->skip = enum_skip;
        for (int i = first; i < nenum_consts; i++) {
            snprintf(enum_vals[nenum_vals].name, 32, "%s", enum_consts[i].name);
            enum_vals[nenum_vals++].val = enum_consts[i].val;
        }
        base.eid = (short)++nenum_infos;
        ei->base = base;
        EnumTag *et = tag[0] ? find_enum_tag(tag) : NULL;
        if (et && et->depth == scope_depth) et->base = base;    /* enum E : T; declared before its body */
    }
    for (int i = first; i < nenum_consts; i++) enum_consts[i].type = base;
    if (tag[0] && !find_enum_tag(tag) && nenum_tags < 256) {   /* `enum E` later has this type */
        strcpy(enum_tags[nenum_tags].name, tag);
        enum_tags[nenum_tags].depth = scope_depth;
        enum_tags[nenum_tags++].base = base;
    }
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
            if (is_type_start(current_token) && !ctor_ahead()) t = parse_type_name();   /* (typeof(Vec(1, 2)): a value) */
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
            t.bfw = t.bfp = 0;                          /* typeof(x.bits): the field's type, not its width */
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
        } else if (k.type == TOKEN_IDENT && !have_base && !uns && !sgn && !nlong && !nshort && implicit_enum(k.text)) {
            t = find_enum_tag(k.text)->base;           /* .c2: Dir d; */
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
    decl_skip = current_token.a_skip;                  /* [[skip]] typedef struct { ... } T; */
    next_token();
    Type t = parse_type_spec();
    if (decl_skip && t.base == TY_STRUCT && t.sidx >= 0) structs[t.sidx].skip = 1;
    if (decl_skip && t.eid) enum_infos[t.eid - 1].skip = 1;
    decl_skip = 0;
    parse_typedef_names(t);
}

/* the declarators of a typedef: `typedef` may also come among the specifiers (const char typedef *s;) */
static void parse_typedef_names(Type base) {
    for (;;) {
        char name[32];
        Type t = parse_declarator(base, name, 0);
        if (!find_typedef(name)) module_decl_name(name, sizeof name, decl_exported || current_token.a_export);
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
static Val stmt_expr(void);
static Val reflect_of(Type t);
static Val reflect_globals_of(void);
static Val reflect_functions_of(void);
static Val keyops_of(Type t);
static int refl_skipped(Type t);

typedef struct Member Member;
static Member *find_member(Type st, const char *name, int *off);
static int count_initializer_at(Type elem, int at_init);
static void init_braced_ex(int base, int disp, Type t, TokenType closer);
static void init_value(int base, int disp, Type t);
static void refl_reset(void);
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
/* try/throw: in a program that calls throw(), each call is followed by "if __c2_throwing: leave", where
   leaving destroys the statement's temporaries and returns through the defers (as `return` would) */
static void temps_emit(int mark);
static void emit_return(Val *v);
static void unwind_check(void) {
    Global *g = find_global("__c2_throwing");
    if (!g) return;
    ensure_global_storage(g);
    int a = new_temp(RK_I), t = new_temp(RK_I), l = new_label();
    EMIT(MOV, R(a), REF(g->name));
    EMIT(MOV, R(t), MEM(MT_I32, 0, a));
    EMIT(BF, LBL(l), R(t));
    no_unwind_check++;
    temps_emit(0);
    emit_return(NULL);
    no_unwind_check--;
    place_label(l);
    last_cmp.valid = 0;
}

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
            if (is_struct_val(v.ty) && is_import) {    /* to C: as the platform ABI passes it (registers / copy) */
                MemT bt = abi_arg_type(v.ty);
                if (bt == MT_I64) {
                    int slot = new_slot("_arg", size_of(v.ty));
                    copy_mem(slot, 0, v.base, v.disp, size_of(v.ty));
                    args[i] = rv_reg(slot, ptr_to(v.ty));
                } else args[i] = rv_op(MEM(bt, size_of(v.ty), addr_reg(v.base, v.disp)), v.ty);
                continue;
            }
            if (is_struct_val(v.ty)) {                 /* our own convention: a pointer to a copy */
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
    if (unwind_on && cur_func && !no_unwind_check && !in_defer && call_may_throw(fname, is_import)) unwind_check();
    return res;
}

typedef struct { int base; Type ty; int dtor; } TempRec;
static TempRec temps[256];
static int dtor_sidx[64], dtor_fi[64];

static Func *dtor_for(Type t) {
    if (!ndtors || !is_struct_val(t)) return NULL;
    for (int i = 0; i < ndtors; i++) if (dtor_sidx[i] == t.sidx) return &funcs[dtor_fi[i]];
    return NULL;
}

static void register_destructor(Func *f) {
    if (!c2_ext) error("[[destructor]] needs a .c2 file");
    if (f->np != 1 || !is_pointer(f->pt[0]) || !is_struct_val(elem_of(f->pt[0])) || is_const_obj(elem_of(f->pt[0])))
        error("[[destructor]] '%s' must take one parameter, a T * (the object to free)", f->name);
    int sidx = elem_of(f->pt[0]).sidx, fi = (int)(f - funcs);
    for (int i = 0; i < ndtors; i++)
        if (dtor_sidx[i] == sidx) {
            if (dtor_fi[i] != fi) error("'%s' already has a [[destructor]]: '%s'", type_name(elem_of(f->pt[0])), funcs[dtor_fi[i]].name);
            return;
        }
    if (ndtors >= 64) error("too many [[destructor]]s");
    dtor_sidx[ndtors] = sidx;
    dtor_fi[ndtors++] = fi;
}

/* [[copy]] T T_copy(const T *): a named T (a variable, a member, *p...) stored somewhere else, or returned
   when it isn't the function's own local, is copied with it first, so the two never share what the
   destructor frees.  A temporary moves instead; so does a fresh (T){...} or T(...) value. */
static int copy_sidx[64], copy_fi[64];
static int fresh_base[256], nfresh;                    /* this function's (T){...} / T(...) slots */
static void mark_fresh(int base) { if (nfresh < 256) fresh_base[nfresh++] = base; }

static Func *copy_for(Type t) {
    if (!ncopies || !is_struct_val(t)) return NULL;
    for (int i = 0; i < ncopies; i++) if (copy_sidx[i] == t.sidx) return &funcs[copy_fi[i]];
    return NULL;
}

static void register_copy(Func *f) {
    if (!c2_ext) error("[[copy]] needs a .c2 file");
    if (f->np < 1 || !is_pointer(f->pt[0]) || !is_struct_val(elem_of(f->pt[0])) || !is_struct_val(f->ret) ||
        f->ret.sidx != elem_of(f->pt[0]).sidx || (f->np > 1 && !(f->pdef && f->pdef[1])))
        error("[[copy]] '%s' must take a T * (or const T *) and return a new T", f->name);
    int sidx = f->ret.sidx, fi = (int)(f - funcs);
    if (!dtor_for(f->ret)) error("[[copy]] '%s': '%s' needs its [[destructor]] first (a copy is something to free)", f->name, type_name(f->ret));
    for (int i = 0; i < ncopies; i++)
        if (copy_sidx[i] == sidx) {
            if (copy_fi[i] != fi) error("'%s' already has a [[copy]]: '%s'", type_name(f->ret), funcs[copy_fi[i]].name);
            return;
        }
    if (ncopies >= 64) error("too many [[copy]]s");
    copy_sidx[ncopies] = sidx;
    copy_fi[ncopies++] = fi;
}

static int temp_index(Val v);
static int is_fresh(Val v) {
    if (v.k != V_MEM || v.disp != 0) return 0;
    for (int i = 0; i < nfresh; i++) if (fresh_base[i] == v.base) return 1;
    return 0;
}
static int is_own_local(Val v) {                       /* a local of the current function, not static */
    if (v.k == V_SRA) return 1;
    if (v.k != V_MEM || v.disp != 0 || v.ix) return 0;
    for (int i = nvars - 1; i >= 0; i--)
        if (vars[i].is_mem && vars[i].reg == v.base && vars[i].global < 0) return 1;
    return 0;
}

static int temp_index(Val v) {
    if (v.k != V_MEM || v.disp != 0) return -1;
    for (int i = ntemps - 1; i >= 0; i--) if (temps[i].base == v.base) return i;
    return -1;
}

static void temp_moved(Val v) {                        /* stored, returned...: no longer destroyed */
    int i = temp_index(v);
    if (i < 0) return;
    memmove(&temps[i], &temps[i + 1], (ntemps - i - 1) * sizeof temps[0]);
    ntemps--;
}

static void zero_slot_prologue(int base, int size) {   /* once, at function entry */
    for (int off = 0; off < size; off += 8)
        EMIT_PROLOGUE(MOV, MEM(size - off >= 8 ? MT_I64 : MT_U8, off, base), IMM(0));
    for (int off = size / 8 * 8 + (size % 8 ? 1 : 0); off < size; off++)
        EMIT_PROLOGUE(MOV, MEM(MT_U8, off, base), IMM(0));
}

static void temp_add(Val r) {                          /* r: a call's T result, in its own slot */
    Func *d = dtor_for(r.ty);
    if (!d || !cur_func || r.k != V_MEM || r.disp != 0) return;
    if (ntemps >= 256) error("too many temporaries in one statement");
    zero_slot_prologue(r.base, size_of(r.ty));
    temps[ntemps].base = r.base;
    temps[ntemps].ty = r.ty;
    temps[ntemps++].dtor = (int)(d - funcs);
}

static Val gen_call(Func *f, Val *args, int n);
static void temps_emit(int mark) {                     /* destroy temps[mark..], newest first (kept listed) */
    for (int i = ntemps - 1; i >= mark; i--) {
        TempRec t = temps[i];
        Val a = address_of(lv_mem(t.base, 0, t.ty));
        int keep = ntemps;
        ntemps = 0;                                    /* (the destructor's own result is not a temporary) */
        no_unwind_check++;                             /* (and a throw in it doesn't stop the others) */
        gen_call(&funcs[t.dtor], &a, 1);
        no_unwind_check--;
        ntemps = keep;
        zero_mem(t.base, 0, size_of(t.ty));
        last_cmp.valid = 0;
    }
}
static void temps_flush(int mark) {                    /* the end of a statement: destroy, newest first */
    if (ntemps > mark) temps_emit(mark);
    if (ntemps > mark) ntemps = mark;
}

static Val parse_expr(void);
static int expr_makes_temps(void) {                    /* dry run: does the expression ahead leave temporaries? */
    if (!ndtors || !c2_ext || !cur_func) return 0;
    LexState ls = save_lex();
    EmitMark em = emit_mark();
    int m = ntemps;
    parse_expr();
    int r = ntemps > m;
    emit_rollback(em);
    restore_lex(ls);
    return r;
}

static Func *last_called;                              /* for [[nodiscard]]: the call evaluated last */
static Val gen_call_(Func *f, Val *args, int n);
static Val gen_call(Func *f, Val *args, int n) {
    Val r = gen_call_(f, args, n);
    if (ndtors && c2_ext) temp_add(r);
    return r;
}
static Val gen_call_(Func *f, Val *args, int n) {
    if (pure_on && f != cur_func && f->nodiscard != 2 && !pure_known(f->name) && strncmp(f->name, "__c2_", 5))
        pure_warn("calls", f->name);
    if (f->deprecated && f->warned_line != cur_line && !no_uninit_warn) {
        f->warned_line = cur_line;
        if (f->deprecated[0] == ' ') diag("%s:%d: warning: '%s' is deprecated\n", cur_file, cur_line, f->name);
        else diag("%s:%d: warning: '%s' is deprecated: %s\n", cur_file, cur_line, f->name, f->deprecated);
    }
    last_called = f;
    ensure_func_items(f);
    char pname[40];
    snprintf(pname, sizeof pname, "proto_%s", f->name);
    Sig g = sig_of_func(f);
    g.unprototyped = f->unprototyped && !f->defined;
    return gen_call_sig(&g, f->name, REF(f->name), pname, f->is_import, f->is_import, args, n);
}

/* long is 64-bit in C², but on Windows the C library reads %ld as 32 bits.  So a literal format
   string given to the printf / scanf families is fixed while compiling: %ld %lu %lx ... become
   %lld %llu %llx ... (no run-time cost; a format that isn't a literal is passed as it is). */
static int fmt_arg = -1;                               /* the argument index of the format, or -1 */

static int long_format_arg(const Func *f) {
#if defined(_WIN32) || defined(C2_TEST_LONG_FORMATS)
    static const struct { const char *name; int arg; } fmts[] = {
        {"printf", 0}, {"fprintf", 1}, {"sprintf", 1}, {"snprintf", 2}, {"vprintf", 0}, {"vfprintf", 1},
        {"vsprintf", 1}, {"vsnprintf", 2}, {"scanf", 0}, {"fscanf", 1}, {"sscanf", 1}, {"vscanf", 0},
        {"vfscanf", 1}, {"vsscanf", 1}};
    if (f->is_import)
        for (size_t i = 0; i < sizeof fmts / sizeof fmts[0]; i++)
            if (!strcmp(f->name, fmts[i].name)) return fmts[i].arg;
#else
    (void)f;
#endif
    return -1;
}

static void widen_long_formats(int arg) {              /* current token: maybe the format literal */
    if (arg != fmt_arg || current_token.type != TOKEN_STR || current_token.wide) return;
    const char *s = current_token.str;
    int n = current_token.str_len, changed = 0;
    Buf b = {0};
    for (int i = 0; i < n; i++) {
        buf_append(&b, s + i, 1);
        if (s[i] != '%') continue;
        if (i + 1 < n && s[i + 1] == '%') { buf_append(&b, "%", 1); i++; continue; }
        int j = i + 1;
        while (j < n && s[j] && strchr("-+ #0123456789.*'", s[j])) j++;   /* flags, width, precision */
        if (j + 1 < n && s[j] == 'l' && s[j + 1] != 'l' && s[j + 1] && strchr("diouxXn", s[j + 1])) {
            buf_append(&b, s + i + 1, j - i);          /* ... up to the 'l' */
            buf_append(&b, "l", 1);
            i = j;
            changed = 1;
        }
    }
    if (!changed) { free(b.p); return; }
    int len = (int)b.len;
    buf_append(&b, "", 1);
    current_token.str = b.p;
    current_token.str_len = len;
}

/* argument arrays live here, not on the stack: one holds 64 Vals (12 KB), and calls nested in
   calls and in ({ }) would overflow small stacks (1 MB on Windows, less on a game's threads) */
#define ARGS_MAX (MAX_PARAMS + 32)
static Val *args_chunks[256];
static int args_depth;
static Val *args_push(void) {
    if (args_depth >= 256) error("calls nested too deeply");
    if (!args_chunks[args_depth]) args_chunks[args_depth] = malloc(ARGS_MAX * sizeof(Val));
    return args_chunks[args_depth++];
}
static void args_pop(void) { args_depth--; }

static int parse_args(Val *args) {   /* current token: '(' */
    next_token();
    int n = 0;
    if (current_token.type != TOKEN_RPAREN) {
        for (;;) {
            if (n >= ARGS_MAX) error("too many arguments");
            widen_long_formats(n);
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
    const char *p = demangle(f->pn[i]);
    name = demangle(name);
    size_t n = strlen(name);
    return !strcmp(p, name) || (!strncmp(p, name, n) && !strncmp(p + n, "_arg", 4));
}

/* argument i of a .c2 call; an `int a[3]` parameter wants an array of exactly 3 elements */
static Val c2_arg(Callee *f, int i) {
    widen_long_formats(i);
    Val raw = parse_assign();
    if (i < f->np && is_any_type(f->pt[i]) && !is_any_type(raw.ty)) return make_any(raw, f->pt[i]);   /* the lvalue itself */
    if (f->plen && i < f->np && f->plen[i] > 0) {
        int want = f->plen[i];
        if (!is_array(raw.ty) || arr_len(raw.ty) < 0)
            error("argument %d of '%s' must be an array of %d elements, not '%s' (declare the parameter [static %d] to take pointers)",
                  i + 1, f->name, want, type_name(raw.ty), want);
        if (arr_len(raw.ty) != want)
            error("argument %d of '%s' must be an array of %d elements, not %d", i + 1, f->name, want, arr_len(raw.ty));
    }
    return rvalue(raw);
}

/* .c2 call arguments: positional, then named (.b = 2); missing ones take their default value,
   evaluated at the call (its source text is parsed here), where the other parameters are visible
   by name with the values this call gives them (`int d = w * 2`).  first: the receiver of x.f(...) */
static void fill_defaults(Callee *f, Val *byp, int *set);
static int parse_args_c2(Callee *f, Val *args, Val *first) {   /* current token: '(' */
    Val *byp = args, *extras = args + f->np;          /* filled in place: no 12 KB copies on the stack */
    int set[MAX_PARAMS] = {0}, pos = 0, extra = 0, named = 0;
    if (first) { byp[0] = *first; set[0] = 1; pos = 1; }
    next_token();
    while (current_token.type != TOKEN_RPAREN) {
        int dot = current_token.type == TOKEN_DOT;
        if (dot || (current_token.type == TOKEN_IDENT && peek_token().type == TOKEN_COLON)) {   /* .name = value, name: value */
            if (dot) next_token();
            if (current_token.type != TOKEN_IDENT) error("parameter name expected after '.'");
            char pname[64];
            snprintf(pname, sizeof pname, "%s", current_token.text);
            next_token();
            expect(dot ? TOKEN_ASSIGN : TOKEN_COLON);
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
    fill_defaults(f, byp, set);
    return f->np + extra;
}

/* the parameters a call left out (set[i] == 0) get their default values, which may use the given ones */
static void fill_defaults(Callee *f, Val *byp, int *set) {
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
}

/* .c2: a T made from a value that isn't one, by T's constructor T(x, ...) when its first parameter takes
   x and the others have defaults: `string s = "hi";`, f("hi") for f(string), return "hi" from a string f */
static Func *implicit_ctor_for(Type from, int null_const, Type to) {   /* the constructor that would convert */
    if (!c2_ext || !cur_func || !is_struct_val(to) || is_struct_val(from)) return NULL;
    const char *n = struct_name(to.sidx);
    Func *c = n ? find_func(n) : NULL;
    if (!c || c == cur_func || !is_struct_val(c->ret) || c->ret.sidx != to.sidx || c->np < 1 || is_struct_val(c->pt[0])) return NULL;
    for (int i = 1; i < c->np; i++) if (!c->pdef || !c->pdef[i]) return NULL;
    if (is_pointer(c->pt[0]) != (is_pointer(from) || is_array(from) || null_const)) return NULL;   /* no int -> char * */
    return c;
}
static int implicit_ctor(Val v, Type to, Val *out) {
    Func *c = implicit_ctor_for(v.ty, v.op.k == OK_INT && v.op.i == 0, to);
    if (!c) return 0;
    Type p0 = c->pt[0];
    Val *args = args_push();
    args[0] = convert(v, p0);
    int set[MAX_PARAMS] = {0};
    set[0] = 1;
    if (c->np > 1) { Callee ce = callee_of_func(c); fill_defaults(&ce, args, set); }
    *out = gen_call(c, args, c->np);
    args_pop();
    return 1;
}

static Val copy_if_named(Val v) {                      /* v, or a copy of it (a temporary) */
    Func *c = copy_for(v.ty);
    if (!c || !cur_func || c == cur_func || temp_index(v) >= 0 || is_fresh(v)) return v;
    if (v.k != V_MEM && v.k != V_SRA) return v;
    Val *args = args_push();
    args[0] = address_of(v);
    int set[MAX_PARAMS] = {0};
    set[0] = 1;
    if (c->np > 1) { Callee ce = callee_of_func(c); fill_defaults(&ce, args, set); }
    Val r = gen_call(c, args, c->np);
    args_pop();
    return r;
}

/* a struct's name for method and overload lookups: its tag, or the typedef that names an anonymous one */
static const char *struct_name(int sidx) {
    const char *n = structs[sidx].name;
    if (n[0] != '<') return demangle(n);
    for (int i = 0; i < ntypedefs; i++)
        if (is_struct_val(typedefs[i].ty) && typedefs[i].ty.sidx == sidx && !typedefs[i].ty.nd) return demangle(typedefs[i].name);
    return NULL;
}

/* the receiver part of an overload's symbol: vec2 (for vec2, vec2 * and const vec2 *), str (char *),
   int, unsigned_int, double_ptr...; "" when the type has no usable name */
static void recv_key(Type t, char *out, size_t size) {
    out[0] = '\0';
    if (is_array(t)) t = ptr_to(elem_of(t));
    if (is_pointer(t) && is_struct_val(elem_of(t))) t = elem_of(t);
    if (is_struct_val(t)) {
        const char *n = struct_name(t.sidx);
        if (n) snprintf(out, size, "%s", n);
        return;
    }
    if (is_pointer(t) && elem_of(t).base == TY_CHAR && !elem_of(t).nd) { snprintf(out, size, "str"); return; }
    const char *tn = type_name(unconst(t));
    size_t k = 0;
    for (const char *p = tn; *p && k + 5 < size; p++) {
        if (!strncmp(p, "const ", 6)) { p += 5; continue; }
        if (*p == '*') { k += snprintf(out + k, size - k, "%sptr", k && out[k - 1] != '_' ? "_" : ""); continue; }
        if (*p == ' ') { if (k && out[k - 1] != '_') out[k++] = '_'; continue; }
        if (isalnum((unsigned char)*p) || *p == '_') out[k++] = *p;
    }
    while (k && out[k - 1] == '_') k--;
    out[k] = '\0';
}

/* the functions sharing an overloaded name, as calls spell it */
typedef struct { char name[40]; int fi[16]; int n; } OvlSet;
static OvlSet ovl_sets[256];
static int novl_sets;
static OvlSet *ovl_set(const char *name, int create) {
    for (int i = 0; i < novl_sets; i++) if (!strcmp(ovl_sets[i].name, name)) return &ovl_sets[i];
    if (!create) return NULL;
    if (novl_sets >= 256) error("too many overloaded names");
    OvlSet *o = &ovl_sets[novl_sets++];
    snprintf(o->name, sizeof o->name, "%s", name);
    o->n = 0;
    return o;
}

static void ovl_list(OvlSet *o, char *buf, size_t size) {   /* "vec2_load, vec3_load" */
    buf[0] = '\0';
    for (int i = 0; i < o->n; i++) {
        size_t k = strlen(buf);
        snprintf(buf + k, size - k, "%s%s", i ? ", " : "", demangle(funcs[o->fi[i]].name));
    }
}

/* the overload of o whose receiver is t: exactly, or after the integer promotions; error if none */
static Func *pick_overload(OvlSet *o, Type t) {
    char want[64], have[64];
    for (int pass = 0; pass < 2; pass++) {
        if (pass == 1) {
            if (!is_integer(t) || same_type(promote(t), t)) break;
            t = promote(t);
        }
        recv_key(t, want, sizeof want);
        for (int i = 0; i < o->n; i++) {
            Func *f = &funcs[o->fi[i]];
            recv_key(f->pt[0], have, sizeof have);
            if (want[0] && !strcmp(want, have)) return f;
        }
    }
    char list[512];
    ovl_list(o, list, sizeof list);
    error("no '%s' takes a '%s' first (there are: %s)", demangle(o->name), type_name(t), list);
    return NULL;
}

/* f(x, ...) with f overloaded: the receiver x's type picks the function */
static Func *resolve_overload_call(OvlSet *o) {    /* current token: '(' */
    LexState ls = save_lex();
    next_token();
    if (current_token.type == TOKEN_RPAREN) error("'%s' is overloaded: its first argument picks the function", demangle(o->name));
    if (current_token.type == TOKEN_DOT || (current_token.type == TOKEN_IDENT && peek_token().type == TOKEN_COLON))
        error("'%s' is overloaded: its first argument (the receiver) can't be named", demangle(o->name));
    Type t = dry_run_type(parse_assign, 1);
    restore_lex(ls);
    return pick_overload(o, t);
}

/* parse_toplevel: an overloaded function declared in .c2 gets the symbol <receiver>_<name> (vec2_load);
   one declared in a plain C header keeps its name.  Either joins the name's overload set. */
static void overload_symbol(Type ft, char *name, size_t size, char *set_name, size_t set_size) {
    snprintf(set_name, set_size, "%s", name);
    Sig *g = &sigs[func_sig(ft)];
    if (g->np < 1 || g->unprototyped) error("'%s' is overloaded, so each one needs a first parameter (its receiver)", demangle(name));
    if (!c2_source_file()) return;
    char key[64];
    recv_key(g->pt[0], key, sizeof key);
    if (!key[0]) error("'%s' is overloaded, and its receiver type '%s' has no name to tell it apart", demangle(name), type_name(g->pt[0]));
    const char *base = demangle(name);
    char out[64];
    fmt_to(out, sizeof out, "%.*s%s_%s", (int)(base - name), name, key, base);
    if (strlen(out) >= size) error("overloaded name '%s_%s' is too long", key, base);
    snprintf(name, size, "%s", out);
}

static void overload_join(const char *set_name, Func *f) {
    OvlSet *o = ovl_set(set_name, 1);
    int fi = (int)(f - funcs);
    char k1[64], k2[64];
    recv_key(f->pt[0], k1, sizeof k1);
    for (int i = 0; i < o->n; i++) {
        if (o->fi[i] == fi) return;
        recv_key(funcs[o->fi[i]].pt[0], k2, sizeof k2);
        if (!strcmp(k1, k2))
            error("'%s' and '%s' overload '%s' for the same receiver '%s' (T, T * and const T * are one receiver)",
                  demangle(funcs[o->fi[i]].name), demangle(f->name), demangle(set_name), k1);
    }
    if (o->n >= 16) error("too many overloads of '%s'", demangle(set_name));
    o->fi[o->n++] = fi;
}

static Val parse_call(Func *f) {
    Val *args = args_push();
    Callee ce = callee_of_func(f);
    int saved_fmt = fmt_arg;
    fmt_arg = long_format_arg(f);
    int n = c2_ext && !f->unprototyped ? parse_args_c2(&ce, args, NULL) : parse_args(args);
    fmt_arg = saved_fmt;
    Val r = gen_call(f, args, n);
    args_pop();
    return r;
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

/* .c2 f-strings: f"{n} apples, {kg:.2f} kg" is string_fmt("%lld apples, %.2f kg", n, kg), a string
   temporary (runtime/string).  {expr} or {expr:spec}, spec as printf's (flags, width, precision, conversion);
   the conversion comes from the type when the spec has none.  {{ and }} are braces; a ?: goes in (). */
static int named_arg_ahead(void) {                    /* current: '(' then name: ... ? */
    LexState ls = save_lex();
    next_token();
    int r = peek_token().type == TOKEN_COLON;
    restore_lex(ls);
    return r;
}

static Val parse_fstring(void) {                       /* current: the "..." after f */
    Func *fmtf = find_func("string_fmt"), *dataf = find_func("string_data");
    if (!fmtf || !dataf) error("f\"...\" makes a string: #import \"runtime/string\"");
    int len = current_token.str_len;
    char *text = malloc(len + 1);
    memcpy(text, current_token.str, len);
    text[len] = '\0';
    next_token();
    Buf fmt = {0};
    Val *args = args_push();
    int n = 1;
    for (int i = 0; i < len;) {
        char ch = text[i];
        if ((ch == '{' || ch == '}') && text[i + 1] == ch) { buf_emit(&fmt, "%c", ch); i += 2; continue; }
        if (ch == '}') error("f-string: a single '}' (write }} for a brace)");
        if (ch == '%') { buf_emit(&fmt, "%%%%"); i++; continue; }
        if (ch != '{') { buf_emit(&fmt, "%c", ch); i++; continue; }
        int j = i + 1, depth = 0, colon = -1;
        char q = 0;
        for (; j < len; j++) {
            char c = text[j];
            if (q) { if (c == '\\') j++; else if (c == q) q = 0; continue; }
            if (c == '"' || c == '\'') q = c;
            else if (c == '(' || c == '[' || c == '{') depth++;
            else if (c == ')' || c == ']') depth--;
            else if (c == '}') { if (!depth) break; depth--; }
            else if (c == ':' && !depth && colon < 0) colon = j;
        }
        if (j >= len) error("f-string: '{' without its '}' (write {{ for a brace)");
        int ee = colon >= 0 ? colon : j;
        char expr[256], spec[32];
        if (ee - i - 1 >= (int)sizeof expr) error("f-string: expression too long");
        if (colon >= 0 && j - colon - 1 >= (int)sizeof spec) error("f-string: format spec too long");
        snprintf(expr, sizeof expr, "%.*s", ee - i - 1, text + i + 1);
        snprintf(spec, sizeof spec, "%.*s", colon >= 0 ? j - colon - 1 : 0, colon >= 0 ? text + colon + 1 : "");
        i = j + 1;
        if (strspn(expr, " \t") == strlen(expr)) error("f-string: empty {}");
        if (n >= ARGS_MAX) error("f-string: too many {}");
        LexState ls = save_lex();                      /* the expression, from its text */
        src = expr;
        next_token();
        Val v = parse_assign();
        if (current_token.type != TOKEN_EOF) error("f-string: bad expression {%s}", expr);
        restore_lex(ls);
        size_t sl = strlen(spec);
        char conv = sl && isalpha((unsigned char)spec[sl - 1]) ? spec[sl - 1] : 0;
        if (conv) spec[--sl] = '\0';
        if (strspn(spec, "-+ #0123456789.") != sl)
            error("f-string: {%s:%s%.*s}: the spec is printf's: flags, width, precision, then a conversion", expr, spec, conv ? 1 : 0, &conv);
        Type ty = v.ty;
        const char *mod = "";
        Func *vtext = find_func("value_text");          /* runtime/reflect: any value as text */
        if (vtext && !conv && ((is_struct_val(ty) && ty.sidx != fmtf->ret.sidx) || (ty.eid && !ty.nd)) && !refl_skipped(ty)) {
            Val *va = args_push();                     /* {hero}: Player { ... }, {dir}: EAST */
            Val tmp = v;
            if (!is_struct_val(ty)) {                  /* an enum value: into memory, for its address */
                tmp = rvalue(v);
                int slot = new_slot("_ev", size_of(ty));
                Val m = lv_mem(slot, 0, ty);
                store_ex(m, tmp, 0);
                tmp = m;
            }
            va[0] = reflect_of(ty);
            va[1] = address_of(tmp);
            v = gen_call(vtext, va, 2);
            args_pop();
            ty = v.ty;
        }
        if (is_struct_val(ty)) {
            if (ty.sidx != fmtf->ret.sidx && vtext && conv && !refl_skipped(ty))
                error("f-string: {%s:%c} on a '%s': {%s} prints its fields", expr, conv, type_name(ty), expr);
            if (ty.sidx != fmtf->ret.sidx) error("f-string: {%s} is a '%s', which has no text form", expr, type_name(ty));
            if (conv && conv != 's') error("f-string: {%s:%c} on a string", expr, conv);
            Val a = address_of(v);
            v = gen_call(dataf, &a, 1);
            conv = 's';
        } else {
            v = rvalue(v);
            ty = v.ty;
            int is_str = is_pointer(ty) && elem_of(ty).base == TY_CHAR && !elem_of(ty).nd;
            if (is_pointer(ty)) {
                if (!conv) conv = is_str ? 's' : 'p';
                if (conv != 'p' && !(conv == 's' && is_str)) error("f-string: {%s:%c} on a '%s'", expr, conv, type_name(ty));
            } else if (is_float(ty) || is_double(ty)) {
                if (!conv) conv = 'g';
                if (!strchr("fFeEgGaA", conv)) error("f-string: {%s:%c} on a '%s'", expr, conv, type_name(ty));
                v = convert(v, mk_type(TY_DOUBLE));
            } else if (is_integer(ty)) {
                if (!conv) conv = ty.base == TY_CHAR && !ty.uns ? 'c' : ty.uns == 1 ? 'u' : 'd';
                if (conv == 'c') v = convert(v, int_type());
                else if (strchr("fFeEgGaA", conv)) v = convert(v, mk_type(TY_DOUBLE));
                else if (strchr("diouxX", conv)) {
                    Type lt = long_type();
                    lt.uns = ty.uns == 1;
                    v = convert(v, lt);
                    mod = "ll";
                } else error("f-string: {%s:%c} on a '%s'", expr, conv, type_name(ty));
            } else error("f-string: {%s} is a '%s', which has no text form", expr, type_name(ty));
        }
        buf_emit(&fmt, "%%%s%s%c", spec, mod, conv);
        args[n++] = rvalue(v);
    }
    free(text);
    const char *item = be_string(fmt.p ? fmt.p : "", (int)fmt.len);
    free(fmt.p);
    int r = new_temp(RK_I);
    EMIT(MOV, R(r), REF(item));
    args[0] = rv_reg(r, ptr_to(mk_type(TY_CHAR)));
    Val res = gen_call(fmtf, args, n);
    args_pop();
    return res;
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
        if (c2_ext && current_token.type == TOKEN_STR && !current_token.wide && !strcmp(t.text, "f")) return parse_fstring();
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
        if (current_token.type == TOKEN_LPAREN && (!strcmp(t.text, "__builtin_add_overflow") ||
            !strcmp(t.text, "__builtin_sub_overflow") || !strcmp(t.text, "__builtin_mul_overflow"))) {
            int op = t.text[10] == 'a' ? 0 : t.text[10] == 's' ? 1 : 2;   /* (a, b, &result): 1 if it overflowed */
            next_token();
            Val a = rvalue(parse_assign());
            expect(TOKEN_COMMA);
            Val b = rvalue(parse_assign());
            expect(TOKEN_COMMA);
            Val r = rvalue(parse_assign());
            expect(TOKEN_RPAREN);
            if (!is_integer(a.ty) || !is_integer(b.ty)) error("%s: the operands must be integers", t.text);
            if (!is_pointer(r.ty) || !is_integer(elem_of(r.ty)) || is_bool(elem_of(r.ty)))
                error("%s: the third argument must point to an integer (not bool)", t.text);
            Type ull = long_type();
            ull.uns = 1;
            Val args[8] = {rv_op(IMM(op), int_type()), convert(a, ull), rv_op(IMM(!a.ty.uns), int_type()),
                           convert(b, ull), rv_op(IMM(!b.ty.uns), int_type()), r,
                           rv_op(IMM(size_of(elem_of(r.ty))), int_type()), rv_op(IMM(!elem_of(r.ty).uns), int_type())};
            Func *f = find_func("__c2_arith_overflow");
            if (!f) error("internal: __c2_arith_overflow is not declared");
            return gen_call(f, args, 8);
        }
        if (current_token.type == TOKEN_LPAREN && strcmp(t.text, "reflect") == 0 && c2_ext && !find_func(t.text) &&
            !find_var(t.text) && !find_global(t.text)) {   /* .c2: reflect(T) or reflect(expr), like sizeof */
            next_token();
            Type rt;
            if (is_type_start(current_token) && !ctor_ahead()) rt = parse_type_name();
            else {
                EmitMark em = emit_mark();             /* not evaluated */
                no_uninit_warn++;
                rt = parse_expr().ty;
                no_uninit_warn--;
                emit_rollback(em);
            }
            expect(TOKEN_RPAREN);
            return reflect_of(rt);
        }
        if (current_token.type == TOKEN_LPAREN && strcmp(t.text, "__c2_keyops") == 0 && c2_ext) {   /* map keys: see keyops_of */
            next_token();
            Type kt = dry_run_type(parse_expr, 0);
            for (int depth = 0; current_token.type != TOKEN_EOF; next_token()) {
                if (current_token.type == TOKEN_LPAREN) depth++;
                else if (current_token.type == TOKEN_RPAREN && depth-- == 0) break;
            }
            expect(TOKEN_RPAREN);
            return keyops_of(kt);
        }
        if (current_token.type == TOKEN_LPAREN && strcmp(t.text, "reflect_functions") == 0 && c2_ext && !find_func(t.text) &&
            !find_var(t.text) && !find_global(t.text)) {   /* .c2: the program's functions, callable by name */
            next_token();
            expect(TOKEN_RPAREN);
            return reflect_functions_of();
        }
        if (current_token.type == TOKEN_LPAREN && strcmp(t.text, "reflect_globals") == 0 && c2_ext && !find_func(t.text) &&
            !find_var(t.text) && !find_global(t.text)) {   /* .c2: the program's globals, by name */
            next_token();
            expect(TOKEN_RPAREN);
            return reflect_globals_of();
        }
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
        if (current_token.type == TOKEN_LPAREN && c2_ext && !find_func(t.text) && implicit_struct(t.text) >= 0) {
            Type st = mk_type(TY_STRUCT);              /* .c2: T(1, 2, z: 3) is (T){1, 2, .z = 3} */
            st.sidx = implicit_struct(t.text);
            int size = size_of(st), base;
            next_token();
            if (!cur_func) error("'%s(...)' outside a function: use (%s){...}", t.text, t.text);
            base = new_slot("_ctor", size);
            zero_mem(base, 0, size);
            mark_fresh(base);
            init_braced_ex(base, 0, st, TOKEN_RPAREN);
            return lv_mem(base, 0, st);
        }
        if (current_token.type == TOKEN_LPAREN && c2_ext && cur_func && implicit_struct(t.text) >= 0 && find_func(t.text) &&
            !(peek_token().type == TOKEN_IDENT && named_arg_ahead())) {
            int si = implicit_struct(t.text);          /* T(t), t already a T: t itself (string(f"...")) */
            LexState ls = save_lex();
            EmitMark em = emit_mark();
            next_token();
            Val a = current_token.type == TOKEN_RPAREN ? rv_op(IMM(0), mk_type(TY_VOID)) : parse_assign();
            int same = current_token.type == TOKEN_RPAREN && is_struct_val(a.ty) && a.ty.sidx == si;
            emit_rollback(em);
            restore_lex(ls);
            if (same) {
                next_token();
                a = parse_assign();
                expect(TOKEN_RPAREN);
                if (temp_index(a) >= 0) return a;
                if (dtor_for(a.ty)) error("%s(x) with x a %s already: x.copy() makes a second one", t.text, t.text);
                int base = new_slot("_ctor", size_of(a.ty));
                copy_mem(base, 0, a.base, a.disp, size_of(a.ty));
                return lv_mem(base, 0, a.ty);
            }
        }
        OvlSet *ovl = c2_ext && !find_global(t.text) ? ovl_set(t.text, 0) : NULL;
        if (ovl && current_token.type == TOKEN_LPAREN) return parse_call(resolve_overload_call(ovl));
        if (ovl) {
            char list[512];
            ovl_list(ovl, list, sizeof list);
            error("'%s' is overloaded: name the one you mean (%s)", demangle(ovl->name), list);
        }
        if (current_token.type == TOKEN_LPAREN && !find_global(t.text)) {
            Func *f = find_func(t.text);
            if (!f) {                                  /* C89 implicit declaration: int name() */
                private_check(t.text);
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
            if (pure_on && npure_regs < 64) { pure_regs[npure_regs].reg = a; pure_regs[npure_regs++].name = g->name; }
            return lv_mem(a, 0, g->ty);
        }
        Func *f = find_func(t.text);
        if (f) {   /* function designator: its address, typed as a function (decays to a pointer) */
            ensure_func_items(f);
            int a = new_temp(RK_I);
            EMIT(MOV, R(a), REF(f->name));
            return rv_op(R(a), func_of(f->ret, func_sig_index(f)));
        }
        if (c2_ext && current_token.type == TOKEN_DOT && (find_typedef(t.text) || find_struct(t.text) >= 0)) {
            next_token();                              /* T.f(...): T_f(...), a function that belongs to the type */
            if (current_token.type != TOKEN_IDENT) error("a function name expected after '%s.'", t.text);
            char fname[96];
            fmt_to(fname, sizeof fname, "%s_%s", t.text, current_token.text);
            Func *tf = find_func(fname);
            if (!tf) error("no function '%s' for %s.%s(...)", fname, t.text, current_token.text);
            next_token();
            if (current_token.type != TOKEN_LPAREN) error("'(' expected after %s.%s", t.text, fname + strlen(t.text) + 1);
            Val *args = args_push();
            Callee ce = callee_of_func(tf);
            int n = parse_args_c2(&ce, args, NULL);
            Val r = gen_call(tf, args, n);
            args_pop();
            return r;
        }
        private_check(t.text);
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
/* the function x.fname() would call if x had no member fname: T_fname or fname, taking a T or T * first */
static Func *method_func_for(Val v, const char *fname) {
    Type st = is_pointer(v.ty) ? elem_of(v.ty) : v.ty;
    if (!is_struct_val(st) || !struct_name(st.sidx)) return NULL;
    char tname[96];
    snprintf(tname, sizeof tname, "%s_%s", struct_name(st.sidx), fname);
    Func *cand[2] = {find_func(tname), find_func(fname)};
    for (int i = 0; i < 2; i++) {
        Func *f = cand[i];
        if (!f || f->unprototyped || f->np < 1) continue;
        Type p0 = is_pointer(f->pt[0]) ? elem_of(f->pt[0]) : f->pt[0];
        if (is_struct_val(p0) && p0.sidx == st.sidx) return f;
    }
    return NULL;
}

/* .c2: a runtime container's methods are its macros: seen.add(x) is set_add(seen, x), a.push(v) is
   array_push(a, v).  The receiver gets a hidden name (the same register, or a pointer to it) and the
   call is expanded from text, as the preprocessor would have: the macros may reassign it. */
static int container_counter;
static Val container_method(Val v, const char *macro) {
    char rname[32], text[2048];
    snprintf(rname, sizeof rname, "__c2_recv%d", container_counter++);
    if (nvars >= 2048) error("too many variables");
    Var *h = &vars[nvars++];
    memset(h, 0, sizeof *h);
    snprintf(h->name, sizeof h->name, "%s", rname);
    h->depth = scope_depth;
    h->global = -1;
    h->serial = ++decl_serial;
    int deref = 0, back = -1;
    if (v.k == V_REG) {                                            /* a local in a register: through memory, */
        h->ty = v.ty;                                              /* copied back after the call */
        h->is_mem = 1;
        h->reg = new_slot("_recv", 8);
        EMIT(MOV, MEM(MT_I64, 0, h->reg), v.op);
        back = v.op.r;
    } else if (v.k == V_MEM) {                                       /* in memory: through a pointer to it */
        Val a = address_of(v);
        h->ty = a.ty;
        h->reg = new_temp(RK_I);
        EMIT(MOV, R(h->reg), a.op);
        deref = 1;
    } else {                                                       /* a value: a copy (read-only use) */
        v = rvalue(v);
        h->ty = v.ty;
        h->reg = new_temp(RK_I);
        EMIT(MOV, R(h->reg), v.op);
    }
    LexState args = save_lex();                     /* current: '(' (or '[': ages["bo"]; or '+=': seen += x); */
    const char *from = args.src;                      /* its arguments, as source text */
    int close = current_token.type == TOKEN_LBRACKET ? TOKEN_RBRACKET : current_token.type == TOKEN_LPAREN ? TOKEN_RPAREN : 0;
    next_token();
    const char *to = NULL;
    for (int depth = 0; ; next_token()) {
        TokenType t = current_token.type;
        if (close ? t == close && depth == 0
                  : depth == 0 && (t == TOKEN_SEMI || t == TOKEN_COMMA || t == TOKEN_RPAREN || t == TOKEN_RBRACKET
                                   || t == TOKEN_RBRACE || t == TOKEN_EOF)) break;
        if (t == TOKEN_EOF) error(close == TOKEN_RBRACKET ? "']' expected" : "')' expected");
        if (t == TOKEN_LPAREN || t == TOKEN_LBRACKET || t == TOKEN_LBRACE) depth++;
        if (t == TOKEN_RPAREN || t == TOKEN_RBRACKET || t == TOKEN_RBRACE) depth--;
        to = src;                                     /* (just past the argument's last token) */
    }
    if (close) {
        to = src - 1;                                 /* the ')' or ']' */
        while (to > from && *to != (close == TOKEN_RBRACKET ? ']' : ')')) to--;
    } else if (!to) error("%s: a value expected after the operator", macro);
    while (from < to && isspace((unsigned char)*from)) from++;
    snprintf(text, sizeof text, "%s(%s%s%s%s%.*s)", macro, deref ? "(*" : "", rname, deref ? ")" : "",
             from < to ? ", " : "", (int)(to - from), from);
    if (close) next_token();                          /* past ')' or ']' */
    LexState after = save_lex();
    PPTok *t = expand_all(pp_tokenize(text, 0));      /* the macro, expanded */
    Buf b = {0};
    for (; t->kind != PK_EOF; t = t->next) { buf_append(&b, " ", 1); buf_append(&b, t->s, t->len); }
    buf_append(&b, "", 0);
    prescan_addr_taken_ex(b.p, 0);                    /* its locals whose address it takes live in memory */
    int line = cur_line;
    src = b.p;
    next_token();
    Val r = parse_expr();
    if (current_token.type != TOKEN_EOF) error("%s: unexpected '%s' in the expanded call", macro, current_token.text);
    if (back >= 0) {                                  /* the macro may have moved it (grown, freed) */
        if (r.k == V_REG || (r.k == V_RVAL && r.op.k == OK_REG)) r = rv_reg(to_reg(rvalue(r)), r.ty);
        EMIT(MOV, R(back), MEM(MT_I64, 0, h->reg));
    }
    restore_lex(after);
    cur_line = line;
    return r;
}

static int oper_fits(Type p, Type t);
static Val method_call(Val v, int arrow, const char *fname) {
    int ck = is_pointer(v.ty) && !arrow ? v.ty.d[v.ty.nd - 1].ck : 0;
    if (ck && c2_ext) {                               /* seen.add(x): set_add(seen, x) */
        char mname[80];
        snprintf(mname, sizeof mname, "%s_%s", ck == C2_CK_ARRAY ? "array" : ck == C2_CK_MAP ? "map" : "set", fname);
        if (find_macro(mname)) return container_method(v, mname);
    }
    /* x.f() on a struct T (or T *) first looks for T_f(): string_len() for s.len(); then f() */
    Type st = is_pointer(v.ty) ? elem_of(v.ty) : v.ty;
    Func *f = NULL;
    if (is_struct_val(st) && struct_name(st.sidx)) {
        char tname[96];
        snprintf(tname, sizeof tname, "%s_%s", struct_name(st.sidx), fname);
        f = find_func(tname);
        if (!f && strcmp(struct_name(st.sidx), structs[st.sidx].name)) {   /* a private struct: __m1_T_f */
            snprintf(tname, sizeof tname, "%s_%s", structs[st.sidx].name, fname);
            f = find_func(tname);
        }
    }
    Func *anyf = NULL;                                 /* any_f: a method of every value (runtime/reflect) */
    if (!f && c2_ext && !is_any_type(st)) {
        char aname[96];
        snprintf(aname, sizeof aname, "any_%s", fname);
        anyf = find_func(aname);
        if (anyf && (anyf->np < 1 || !is_any_type(anyf->pt[0]))) anyf = NULL;
    }
    OvlSet *ovl = !f && c2_ext ? ovl_set(fname, 0) : NULL;
    if (ovl && anyf) {                                 /* f(x) when one of f's takes an x first, else any_f(x) */
        int fits = 0;
        for (int i = 0; i < ovl->n && !fits; i++) fits = funcs[ovl->fi[i]].np > 0 && oper_fits(funcs[ovl->fi[i]].pt[0], v.ty) > 0;
        if (!fits) ovl = NULL;
    }
    if (ovl) f = pick_overload(ovl, v.ty);              /* 21.print(): int_print */
    if (!f) {
        Func *plain = find_func(fname);
        if (anyf && (!plain || plain->np < 1 || plain->unprototyped || !oper_fits(plain->pt[0], v.ty))) f = anyf;
        else f = plain;
    }
    if (f && f == anyf) {                              /* x.print(): any_print((any){reflect(T), &x}) */
        Val self = make_any(v, f->pt[0]);
        Val *args = args_push();
        Callee ce = callee_of_func(f);
        int n = parse_args_c2(&ce, args, &self);
        Val r = gen_call(f, args, n);
        args_pop();
        return r;
    }
    if (!f) error("no member or function '%s' (nor a %s_%s) for %s.%s(...)", fname,
                  is_struct_val(st) && struct_name(st.sidx) ? struct_name(st.sidx) : "T", fname, type_name(v.ty), fname);
    if (f->unprototyped || f->np == 0) error("'%s' takes no parameters: it cannot be called as x.%s()", fname, fname);
    Type p0 = f->pt[0];
    Val self;
    if (!arrow && is_pointer(p0) && !is_pointer(v.ty) && !is_array(v.ty) && !is_func(v.ty)) self = address_of(v);
    else self = rvalue(v);
    if (arrow && !is_pointer(self.ty)) error("'->' on '%s', which is not a pointer", type_name(self.ty));
    Val *args = args_push();
    Callee ce = callee_of_func(f);
    int n = parse_args_c2(&ce, args, &self);
    Val r = gen_call(f, args, n);
    args_pop();
    return r;
}

/* ============================================================ [[operator X]] (.c2)
 * `[[operator+]] vec3 vec3_add(vec3 a, vec3 b);` makes `a + b` call vec3_add(a, b) when an operand is a
 * struct or union: operators keep their C meaning everywhere else.  A struct operand must match its
 * parameter exactly (T, T * or const T *: a pointer parameter gets &operand); the other operand converts
 * as in any call.  `!=` is !(==); `a += b` calls [[operator+=]], or else does a = a + b.  [[operator[]]]
 * returning a pointer makes v[i] assignable.  [[operator-]] with one parameter is unary minus.  Missing
 * comparisons come from `<`: a > b is b < a, a <= b is !(b < a), a >= b is !(a < b).
 * The functions keep their names: the host and C see them. */
static int oper_func[512], oper_code[512], oper_nc[512], noper;

static Type struct_of(Type t) { return is_pointer(t) && is_struct_val(elem_of(t)) ? elem_of(t) : t; }

static void register_operator(Func *f, int code, const char *text) {
    if (code < 0) error("[[operator%s]] is not supported (operators: + - * / %% == < <= > >= [] += -= *= /= %%=, "
                        "unary -; != comes from ==)", text);
    if (!c2_ext) error("[[operator%s]] needs a .c2 file", text);
    int req = 0;                                       /* parameters without a default: the operands */
    while (req < f->np && !(f->pdef && f->pdef[req])) req++;
    if (f->vararg) error("[[operator%s]] '%s' can't be variadic", text, f->name);
    if (code == OPR_SUB && req == 1) code = OPR_NEG;   /* -a */
    if (code == OPR_NEG) {
        if (!is_struct_val(struct_of(f->pt[0]))) error("[[operator-]] '%s': the parameter must be a struct or union (T, T * or const T *)", f->name);
    } else {
        if (req != 2) error("[[operator%s]] '%s' must take 2 parameters%s, not %d (more are fine when they have defaults)",
                            text, f->name, code == OPR_SUB ? " (or 1 for unary -)" : "", req);
        if (!is_struct_val(struct_of(f->pt[0])) && !is_struct_val(struct_of(f->pt[1])))
            error("[[operator%s]] '%s': a parameter must be a struct or union (T, T * or const T *)", text, f->name);
    }
    if (code >= OPR_ADD_SET && code <= OPR_MOD_SET && !(is_pointer(f->pt[0]) && is_struct_val(elem_of(f->pt[0])) && !is_const_obj(elem_of(f->pt[0]))))
        error("[[operator%s]] '%s': the first parameter must be a T *, the object it changes", text, f->name);
    if (code == OPR_INDEX && !is_struct_val(struct_of(f->pt[0])))
        error("[[operator[]]] '%s': the first parameter must be the struct (T, T * or const T *)", f->name);
    int fi = (int)(f - funcs);
    for (int i = 0; i < noper; i++) if (oper_func[i] == fi) {
        if (oper_code[i] != code) error("'%s' is already [[operator%s]]", f->name, opr_text[oper_code[i]]);
        return;                                         /* declared again */
    }
    if (noper >= 512) error("too many [[operator]] functions");
    oper_func[noper] = fi;
    oper_nc[noper] = 0;
    oper_code[noper++] = code;
}

/* does operand type t fit parameter p: exactly for structs (2), by conversion otherwise (1) */
static int oper_fits(Type p, Type t) {
    if (is_struct_val(t)) {
        Type q = struct_of(p);
        return is_struct_val(q) && q.sidx == t.sidx ? 2 : 0;
    }
    if (is_struct_val(struct_of(p))) return 0;
    if (same_type(unconst(p), unconst(t))) return 2;
    int pp = is_pointer(p), tp = is_pointer(t) || is_array(t) || is_func(t);
    if (pp != tp) return 0;                            /* a char doesn't fit a char *, nor the reverse */
    if (pp) {                                          /* pointers: the same pointee, const aside, fits best */
        Type pe = elem_of(p), te = is_func(t) ? t : elem_of(t);
        pe.bcq = te.bcq = 0;
        return same_type(unconst(pe), unconst(te)) ? 2 : 1;
    }
    return 1;                                          /* numbers convert */
}

/* the [[operator]] function for these operands; NULL if none.  Several that fit equally: an error. */
static Func *find_operator(int code, Type a, Type b) {   /* (OPR_NEG: b is unused) */
    Func *best = NULL;
    int best_score = 0, tie = 0;
    for (int i = 0; i < noper; i++) {
        if (oper_code[i] != code) continue;
        Func *f = &funcs[oper_func[i]];
        int s0 = oper_fits(f->pt[0], a), s1 = code == OPR_NEG ? 2 : oper_fits(f->pt[1], b);
        if (!s0 || !s1) continue;
        if (s0 + s1 > best_score) { best = f; best_score = s0 + s1; tie = 0; }
        else if (s0 + s1 == best_score) tie = 1;
    }
    if (tie) {
        Buf names = {0};
        for (int i = 0; i < noper; i++) {
            Func *f = &funcs[oper_func[i]];
            int s1 = code == OPR_NEG ? 2 : oper_fits(f->pt[1], b);
            if (oper_code[i] == code && oper_fits(f->pt[0], a) + s1 == best_score && oper_fits(f->pt[0], a) && s1)
                buf_emit(&names, "%s%s", names.len ? ", " : "", f->name);
        }
        char at[160];
        snprintf(at, sizeof at, "%s", type_name(a));
        if (code == OPR_NEG) error("-'%s' is ambiguous: [[operator-]] %s fit equally", at, names.p);
        error("'%s' %s '%s' is ambiguous: [[operator%s]] %s fit equally", at, opr_text[code], type_name(b),
              opr_text[code], names.p);
    }
    return best;
}

static Val call_operator_n(Func *f, Val a, Val b, int n) {
    Val *args = args_push();
    Val in[2] = {a, b};
    for (int i = 0; i < n; i++) {
        Type p = f->pt[i];
        if (is_struct_val(in[i].ty) && is_pointer(p)) {   /* T * parameter: the operand's address */
            if (is_const_obj(in[i].ty) && !is_const_obj(elem_of(p)))
                error("[[operator]] '%s' takes a '%s', which could change the const operand", f->name, type_name(p));
            if (in[i].k != V_MEM && in[i].k != V_SRA) {    /* a value without an address: a temporary */
                int slot = new_slot("_op", size_of(in[i].ty));
                Val tmp = lv_mem(slot, 0, unconst(in[i].ty));
                store_ex(tmp, in[i], 1);
                in[i] = tmp;
            }
            args[i] = address_of(in[i]);
        } else args[i] = rvalue(in[i]);
    }
    if (f->np > n) {                                   /* the other parameters take their defaults */
        int set[MAX_PARAMS] = {0};
        for (int i = 0; i < n; i++) set[i] = 1;
        Callee ce = callee_of_func(f);
        fill_defaults(&ce, args, set);
    }
    Val r = gen_call(f, args, f->np);
    args_pop();
    return r;
}
static Val call_operator(Func *f, Val a, Val b) { return call_operator_n(f, a, b, 2); }

static Val truth_not(Val v) {                          /* !v, as 0 or 1 */
    v = rvalue(v);
    if (is_struct_val(v.ty) || is_void(v.ty)) error("! on '%s'", type_name(v.ty));
    int t = new_temp(RK_I);
    if (is_float(v.ty)) EMIT(FEQ, R(t), v.op, FIMM(0.0));
    else if (is_double(v.ty)) EMIT(DEQ, R(t), v.op, DIMM(0.0));
    else if (is_int32(v.ty)) EMIT(EQS, R(t), v.op, IMM(0));
    else EMIT(EQ, R(t), v.op, IMM(0));
    return rv_reg(t, int_type());
}

/* a binary operator with a struct operand: its [[operator]] function, or an error */
static const char *binop_text(TokenType op) {
    switch (op) {
    case TOKEN_PLUS: return "+"; case TOKEN_MINUS: return "-"; case TOKEN_ASTERISK: return "*";
    case TOKEN_SLASH: return "/"; case TOKEN_PERCENT: return "%"; case TOKEN_EQ: return "==";
    case TOKEN_NE: return "!="; case TOKEN_LT: return "<"; case TOKEN_LE: return "<=";
    case TOKEN_GT: return ">"; case TOKEN_GE: return ">="; case TOKEN_SHL: return "<<";
    case TOKEN_SHR: return ">>"; case TOKEN_AMPERSAND: return "&"; case TOKEN_PIPE: return "|";
    case TOKEN_CARET: return "^"; default: return "?";
    }
}

static void oper_noncomm_set(Func *f) {              /* [[noncommutative]] on an operator function */
    for (int i = 0; i < noper; i++) if (oper_func[i] == (int)(f - funcs)) oper_nc[i] = 1;
}
static int oper_noncomm(Func *f) {
    for (int i = 0; i < noper; i++) if (oper_func[i] == (int)(f - funcs)) return oper_nc[i];
    return 0;
}

static Val apply_operator(TokenType op, Val a, Val b) {
    int code = op == TOKEN_PLUS ? OPR_ADD : op == TOKEN_MINUS ? OPR_SUB : op == TOKEN_ASTERISK ? OPR_MUL :
               op == TOKEN_SLASH ? OPR_DIV : op == TOKEN_PERCENT ? OPR_MOD : op == TOKEN_EQ || op == TOKEN_NE ? OPR_EQ :
               op == TOKEN_LT ? OPR_LT : op == TOKEN_LE ? OPR_LE : op == TOKEN_GT ? OPR_GT : op == TOKEN_GE ? OPR_GE : 0;
    Func *f = code ? find_operator(code, a.ty, b.ty) : NULL;
    if (!f && (code == OPR_ADD || code == OPR_MUL || code == OPR_EQ) && is_arith(a.ty) && is_struct_val(b.ty)) {
        Func *g = find_operator(code, b.ty, a.ty);    /* 2 * v is v * 2 (unless [[noncommutative]]) */
        if (g && !oper_noncomm(g)) {
            Val r = call_operator(g, b, a);
            return op == TOKEN_NE ? truth_not(r) : r;
        }
    }
    if (!f && code >= OPR_LE && code <= OPR_GE) {      /* from <: a > b is b < a, a <= b is !(b < a), a >= b is !(a < b) */
        int swap = code != OPR_GE;
        Func *lt = swap ? find_operator(OPR_LT, b.ty, a.ty) : find_operator(OPR_LT, a.ty, b.ty);
        if (lt) {
            Val r = swap ? call_operator(lt, b, a) : call_operator(lt, a, b);
            return code == OPR_GT ? r : truth_not(r);
        }
    }
    if (!f) {
        char at[160];
        snprintf(at, sizeof at, "%s", type_name(a.ty));
        if (!code) error("'%s' %s '%s': C² has no [[operator%s]] (operators: + - * / %% == < <= > >= [] += -= *= /= %%=, unary -)",
                         at, binop_text(op), type_name(b.ty), binop_text(op));
        if (code >= OPR_LE && code <= OPR_GE)
            error("no [[operator%s]] (nor [[operator<]]) for '%s' %s '%s'", opr_text[code], at, binop_text(op), type_name(b.ty));
        Func *g = is_struct_val(b.ty) ? find_operator(code, b.ty, a.ty) : NULL;
        if (g && oper_noncomm(g))
            error("no [[operator%s]] for '%s' %s '%s' ('%s' takes them the other way round, and is [[noncommutative]])",
                  opr_text[code], at, binop_text(op), type_name(b.ty), g->name);
        error("no [[operator%s]] for '%s' %s '%s'", opr_text[code], at, binop_text(op), type_name(b.ty));
    }
    Val r = call_operator(f, a, b);
    return op == TOKEN_NE ? truth_not(r) : r;
}

static Val postfix_ops(Val v);
static Val parse_postfix(void) { return postfix_ops(parse_primary()); }
static Val postfix_ops(Val v) {                    /* v[i] v.m v->m v(...) v++ v-- after a primary */
    for (;;) {
        if (current_token.type == TOKEN_LBRACKET && noper && is_struct_val(v.ty)) {   /* [[operator[]]] */
            next_token();
            Val idx = parse_expr();
            expect(TOKEN_RBRACKET);
            Func *f = find_operator(OPR_INDEX, v.ty, idx.ty);
            if (!f) error("no [[operator[]]] for '%s'[%s]", type_name(v.ty), type_name(idx.ty));
            Val r = call_operator(f, v, idx);
            v = is_pointer(r.ty) && !is_void(elem_of(r.ty)) ? lv_mem(to_reg(r), 0, elem_of(r.ty)) : r;   /* v[i] = x */
            continue;
        }
        if (current_token.type == TOKEN_LBRACKET && c2_ext && is_pointer(v.ty) && v.ty.d[v.ty.nd - 1].ck
            && v.ty.d[v.ty.nd - 1].ck != C2_CK_ARRAY) {    /* ages["bo"]: ages.at("bo");  seen[x]: seen.has(x) */
            v = container_method(v, v.ty.d[v.ty.nd - 1].ck == C2_CK_MAP ? "map_at" : "set_has");
            continue;
        }
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
            if (c2_ext && current_token.type == TOKEN_LPAREN) {   /* a member wins over a function of that name */
                Func *mf = method_func_for(v, mname);
                if (mf) diag("%s:%d: warning: '%s' is a member of '%s', so .%s() calls it, not %s()\n",
                             cur_file, cur_line, mname, type_name(is_pointer(v.ty) ? elem_of(v.ty) : v.ty), mname, mf->name);
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
            Val *args = args_push();
            Callee ce = callee_of_sig(&sigs[si], "(function pointer)");
            int n = c2_ext && !sigs[si].unprototyped ? parse_args_c2(&ce, args, NULL) : parse_args(args);
            fv = rvalue(fv);
            Opnd callee = R(to_reg(fv));
            v = gen_call_sig(&sigs[si], "(function pointer)", callee, sig_proto_name(si), 1, 0, args, n);
            args_pop();
        } else if (current_token.type == TOKEN_INC || current_token.type == TOKEN_DEC) {
            TokenType op = current_token.type;
            next_token();
            v = inc_dec(v, op, 1);
        } else {
            return v;
        }
    }
}

static int fn_used_defer, fn_used_goto;
static int goto_label(const char *name, int define);
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
        Val v = parse_unary();
        if (noper && is_struct_val(v.ty)) {             /* [[operator-]] with one parameter */
            Func *f = find_operator(OPR_NEG, v.ty, v.ty);
            if (!f) error("no [[operator-]] with one parameter for -'%s'", type_name(v.ty));
            return call_operator_n(f, v, v, 1);
        }
        v = rvalue(v);
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
    case TOKEN_ANDAND:                                 /* GNU &&label: the label's address, for goto *p */
        if (peek_token().type == TOKEN_IDENT && cur_func) {
            next_token();
            if (fn_used_defer || in_defer) error("labels in a function that uses defer are not supported");
            fn_used_goto = 1;
            int l = goto_label(current_token.text, 0), r = new_temp(RK_I);
            next_token();
            EMIT(LADDR, R(r), LBL(l));
            return rv_reg(r, ptr_to(mk_type(TY_VOID)));
        }
        return parse_postfix();
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
    case TOKEN_COUNTOF: {                              /* C2y: the number of elements of an array */
        next_token();
        Type t;
        if (current_token.type == TOKEN_LPAREN && is_type_start(peek_token())) {
            next_token();
            t = parse_type_name();
            expect(TOKEN_RPAREN);
        } else {
            EmitMark em = emit_mark();
            no_uninit_warn++;
            t = parse_unary().ty;
            no_uninit_warn--;
            emit_rollback(em);
        }
        if (!is_array(t)) error("countof of '%s', which is not an array (a pointer has no element count)", type_name(t));
        Type r = long_type();
        r.uns = 1;
        if (is_vla(t)) return rv_reg(vla_len_reg[-2 - arr_len(t)], r);
        if (arr_len(t) < 0) error("countof of an array of unknown length");
        return rv_op(IMM(arr_len(t)), r);
    }
    case TOKEN_LPAREN:
        if (peek_token().type == TOKEN_LBRACE) {       /* GNU statement expression ({ ...; value; }) */
            next_token();
            Val v = stmt_expr();
            expect(TOKEN_RPAREN);
            return postfix_ops(v);                     /* ({ ... s; }).x */
        }
        if (is_type_start(peek_token()) && !paren_ctor_ahead()) {   /* cast */
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
                    mark_fresh(base);
                }
                init_value(base, 0, t);
                return postfix_ops(lv_mem(base, 0, t));   /* (struct P){1, 2}.x */
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
        if (noper && (is_struct_val(v.ty) || is_struct_val(rhs.ty))) { v = apply_operator(op, v, rhs); continue; }
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
/* a null pointer constant: 0, or (void *)0 (NULL) */
static int is_null_const(Val v) {
    return v.k == V_RVAL && v.op.k == OK_INT && v.op.i == 0 &&
           (is_integer(v.ty) || (is_pointer(v.ty) && is_void(elem_of(v.ty)) && !is_const_obj(elem_of(v.ty))));
}

static Val parse_cond(void) {
    Val c = parse_logical(1);
    if (current_token.type != TOKEN_QUESTION) return c;
    next_token();
    int cr = cond_reg(c);
    /* the result type depends on both branches: look at them first without keeping the code */
    LexState ls = save_lex();
    EmitMark em = emit_mark();
    Val va = rvalue(parse_expr());
    expect(TOKEN_COLON);
    Val vb = rvalue(parse_cond());
    Type ta = va.ty, tb = vb.ty;
    int tmp_a = temp_index(va) >= 0, tmp_b = temp_index(vb) >= 0;
    emit_rollback(em);
    if (tmp_a != tmp_b && dtor_for(ta) && dtor_for(tb))
        error("?: mixes a temporary '%s' with one that is not: make both temporaries, or neither", type_name(ta));
    restore_lex(ls);

    Type rt;
    if (is_arith(ta) && is_arith(tb)) rt = common_type(ta, tb);
    else if (is_struct_val(ta) && is_struct_val(tb) && ta.sidx == tb.sidx) rt = ta;
    else if (is_void(ta) && is_void(tb)) rt = ta;
    else if (is_pointer(ta) && is_pointer(tb) && is_null_const(va)) rt = tb;   /* c ? NULL : p is a p */
    else if (is_pointer(ta) && is_pointer(tb) && is_null_const(vb)) rt = ta;
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
            if (tmp_a && tmp_b) temp_moved(v);         /* the result is the temporary now */
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
    if (is_struct_val(rt)) {
        Val res = lv_mem(r, 0, rt);
        if (tmp_a && tmp_b) temp_add(res);
        return res;
    }
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
        LexState at_op = save_lex();
        static const TokenType bin[] = {TOKEN_PLUS, TOKEN_MINUS, TOKEN_ASTERISK, TOKEN_SLASH, TOKEN_PERCENT,
                                        TOKEN_AMPERSAND, TOKEN_PIPE, TOKEN_CARET, TOKEN_SHL, TOKEN_SHR};
        TokenType b = bin[op - TOKEN_PLUS_ASSIGN];
        next_token();
        if (c2_ext && (op == TOKEN_PLUS_ASSIGN || op == TOKEN_MINUS_ASSIGN) && is_pointer(lhs.ty)
            && lhs.ty.d[lhs.ty.nd - 1].ck == C2_CK_SET) {   /* seen += x: seen.add(x);  seen -= x: seen.del(x) */
            restore_lex(at_op);
            return container_method(lhs, op == TOKEN_PLUS_ASSIGN ? "set_add" : "set_del");
        }
        if (lhs.k == V_RVAL) error("compound assignment to something that is not an lvalue");
        if (is_const_obj(lhs.ty)) error("assignment to a const object of type '%s'", type_name(lhs.ty));
        Val rhs = parse_assign();
        if (noper && is_struct_val(lhs.ty) && op <= TOKEN_MOD_ASSIGN) {   /* a += b: [[operator+=]], or a = a + b */
            static const int set[] = {OPR_ADD_SET, OPR_SUB_SET, OPR_MUL_SET, OPR_DIV_SET, OPR_MOD_SET};
            Func *f = find_operator(set[op - TOKEN_PLUS_ASSIGN], lhs.ty, rhs.ty);
            if (f) return call_operator(f, lhs, rhs);
            return store(lhs, apply_operator(b, lhs, rhs));
        }
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
/* `char a[] = ("wat");`: a string literal in parentheses still initializes the array.  Returns the
   number of '(' before such a literal (0 if there is none), consuming nothing. */
static int paren_str_inits(Type et) {
    if (current_token.type != TOKEN_LPAREN) return 0;
    LexState ls = save_lex();
    int n = 0;
    while (current_token.type == TOKEN_LPAREN) { next_token(); n++; }
    int ok = str_inits(et);
    if (ok) {
        next_token();
        for (int i = 0; i < n && ok; i++) { ok = current_token.type == TOKEN_RPAREN; next_token(); }
    }
    restore_lex(ls);
    return ok ? n : 0;
}
static int str_units(void) { return current_token.str_len / (current_token.wide ? WCHAR_SIZE : 1); }

/* element count of `T x[] = <init>`; at_init: the current token is the initializer itself
   (otherwise it is the '=' before it).  Designators ([5] = ...) are taken into account. */
static int count_initializer_at(Type elem, int at_init) {
    LexState s = save_lex();
    if (!at_init) next_token();
    int n = 0;
    for (int np = paren_str_inits(elem); np > 0; np--) next_token();
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
        return str_inits(elem_of(t)) || paren_str_inits(elem_of(t));
    if (is_struct_val(t)) {
        Type et = dry_run_type(parse_assign, 1);
        return (is_struct_val(et) && et.sidx == t.sidx) || implicit_ctor_for(et, 0, t);   /* .c2: {"text"} for a string */
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
/* closer: '}' for { ... }, ')' for a .c2 constructor T(...) */
static void init_braced_ex(int base, int disp, Type t, TokenType closer) {
    IFrame st[64];
    int sp = 1;
    st[0].t = t;
    st[0].disp = disp;
    st[0].idx = 0;
    while (current_token.type != closer) {
        int colon = c2_ext && current_token.type == TOKEN_IDENT && peek_token().type == TOKEN_COLON;   /* .c2: x: 1 */
        if (current_token.type == TOKEN_DOT || current_token.type == TOKEN_LBRACKET || colon) {
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
                } else if (current_token.type == TOKEN_DOT || colon) {
                    if (!is_struct_val(f->t)) error("member designator for '%s', which is not a struct", type_name(f->t));
                    if (!colon) next_token();
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
                    if (colon) break;
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
            expect(colon ? TOKEN_COLON : TOKEN_ASSIGN);
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
    expect(closer);
}
static void init_braced(int base, int disp, Type t) { init_braced_ex(base, disp, t, TOKEN_RBRACE); }

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
        int np = paren_str_inits(et);
        for (int i = 0; i < np; i++) next_token();
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
            for (int i = 0; i < np; i++) expect(TOKEN_RPAREN);
            return;
        }
        if (current_token.type == TOKEN_STR)
            error(current_token.wide ? "L\"...\" initializes wchar_t arrays only" : "a string literal initializes char arrays only");
        if (c2_ext) {                                  /* .c2: int b[3] = a; (typeof(a) v = a;) copies it */
            Type et = dry_run_type(parse_assign, 0);
            if (is_array(et) && same_type(et, t) && arr_len(et) == arr_len(t)) {
                Val v = parse_assign();
                copy_mem(base, disp, to_reg(address_of(v)), 0, size_of(t));
                return;
            }
        }
        error("array initializer must be a list");
    }
    store_ex(lv_mem(base, disp, t), parse_assign(), 1);   /* scalar, or struct copied from an expression */
}

static void init_object(int base, int disp, Type t, int top) { (void)top; init_value(base, disp, t); }


/* ============================================================ parser: statements */
static int break_stack[128], continue_stack[128], loop_depth;   /* label ids; continue = -1 for switch */

typedef struct { int n, cap, def; long long *val; int *lab; Type ty; int depth; } SwitchCtx;
static SwitchCtx *sw_stack[64];
static int sw_depth;
static int unrolling, unroll_sw_base;   /* inside an [[unroll]] body; the switch depth around the loop */

static Func *declare_function(Type ft, const char *name);
static void emit_global_init(Global *g);

typedef struct { char name[32]; int label, defined, line, goto_serial, goto_line; } GotoLabel;
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
        g->goto_serial = -1;
    }
    if (!define && !g->defined && g->goto_serial < 0) { g->goto_serial = decl_serial; g->goto_line = cur_line; }   /* a forward goto */
    if (define) {
        if (unrolling) error("[[unroll]]: the loop body has a label ('%s'), which can't be repeated", name);
        if (g->defined) error("duplicate label '%s'", name);
        g->defined = 1;
        for (int i = 0; c2_ext && g->goto_serial >= 0 && i < nvars; i++)   /* .c2: skipped the zeroing */
            if (vars[i].serial > g->goto_serial && vars[i].global < 0)
                diag("%s:%d: warning: the goto at line %d jumps past the declaration of '%s', which then isn't zeroed\n",
                     cur_file, cur_line, g->goto_line, vars[i].name);
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

static int dbg_keep_vars(void);
static void dbg_var(Var *v, int param);
static Var *declare_var(const char *name, Type t) {
    for (int i = nvars - 1; i >= 0 && vars[i].depth == scope_depth; i--)
        if (strcmp(vars[i].name, name) == 0) error("redeclaration of '%s'", name);
    if (is_void(t)) error("variable '%s' declared void", name);
    if (is_struct_val(t) && !structs[t.sidx].complete) error("variable '%s' has incomplete type", name);
    if (nvars >= 2048) error("too many variables");
    Var *v = &vars[nvars++]; memset(v, 0, sizeof *v); v->serial = ++decl_serial;
    strcpy(v->name, name);
    v->ty = t;
    v->depth = scope_depth;
    v->global = -1;
    v->is_mem = is_aggregate(t) || is_addr_taken(name) || dbg_keep_vars();   /* (-g: in memory, for crash reports) */
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
    dbg_var(v, 0);
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
static int globals_final;                         /* end of the file: tentative `int a[];` gets one element */
static void ensure_global_storage(Global *g) {
    if (!g->deferred) return;
    if (is_array(g->ty) && arr_len(g->ty) < 0) {      /* `extern int a[];` then `int a[10];` later in the file */
        if (!globals_final) { if (!g->fwd) { be_simple_item("forward", g->name); g->fwd = 1; } return; }
        g->ty = set_arr_len(g->ty, 1);
        g->deferred = 0;
        be_bss(g->name, size_of(g->ty));
        return;
    }
    if (!structs[g->ty.sidx].complete) error("variable '%s' has incomplete type", g->name);
    g->deferred = 0;
    be_bss(g->name, size_of(g->ty));
}

static Global *add_global(const char *name, Type t) {
    if (nglobals >= 1024) error("too many globals");
    Global *g = &globals[nglobals++];
    snprintf(g->name, sizeof g->name, "%s", name);
    g->ty = t;
    g->has_init = g->persist = g->imported = g->deferred = g->fwd = g->skip = 0;
    return g;
}

/* storage + initializer of a global (or static local); current token is after the declarator */
/* GNU: `static void *tab[] = {&&a, &&b};` in a function: the labels exist only there, so the
   initializer runs there, the first time the declaration is reached (a guard flag) */
static int init_has_label_addr(void) {
    if (!cur_func || current_token.type != TOKEN_ASSIGN) return 0;
    LexState ls = save_lex();
    int found = 0;
    TokenType prev = TOKEN_ASSIGN;
    for (int depth = 0; current_token.type != TOKEN_EOF && !found; next_token()) {
        TokenType t = current_token.type;
        if (depth == 0 && (t == TOKEN_COMMA || t == TOKEN_SEMI)) break;
        if (t == TOKEN_LBRACE || t == TOKEN_LPAREN || t == TOKEN_LBRACKET) depth++;
        if (t == TOKEN_RBRACE || t == TOKEN_RPAREN || t == TOKEN_RBRACKET) depth--;
        if (t == TOKEN_ANDAND && prev != TOKEN_IDENT && prev != TOKEN_NUM && prev != TOKEN_RBRACKET && peek_token().type == TOKEN_IDENT)
            found = 1;
        prev = t;
    }
    restore_lex(ls);
    return found;
}
static void static_init_here(Global *g) {             /* current token: '=' */
    char flag[40];
    snprintf(flag, sizeof flag, "%.33s_once", g->name);
    be_bss(g->name, size_of(g->ty));
    be_bss(flag, 4);
    next_token();
    g->has_init = 1;
    int f = new_temp(RK_I), t = new_temp(RK_I), done = new_label();
    EMIT(MOV, R(f), REF(flag));
    EMIT(MOV, R(t), MEM(MT_I32, 0, f));
    EMIT(BT, LBL(done), R(t));
    EMIT(MOV, MEM(MT_I32, 0, f), IMM(1));
    int a = new_temp(RK_I);
    EMIT(MOV, R(a), REF(g->name));
    if (is_aggregate(g->ty)) init_object(a, 0, g->ty, 1);   /* storage is bss: already zero */
    else store_ex(lv_mem(a, 0, g->ty), parse_assign(), 1);
    place_label(done);
}

static void define_global_storage(Global *g, int defer_dynamic) {
    Type t = g->ty;
    if (defer_dynamic && init_has_label_addr()) { static_init_here(g); return; }
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
    int indet_all = current_token.a_indeterminate;     /* [[indeterminate]] before the declaration */
    Type base = parse_type_spec();
    if (spec_typedef) { parse_typedef_names(base); return; }
    int is_static = spec_static, spec_extern_local = spec_extern, cx = spec_constexpr;
    if (current_token.type == TOKEN_SEMI) { next_token(); return; }   /* struct X {...}; */
    for (;;) {
        char name[32];
        int tm = ntemps;
        vla_ok = !is_static && !spec_extern_local;
        Type t = parse_declarator(base, name, 0);
        vla_ok = 0;
        spec_constexpr = cx;
        int indet = indet_all || current_token.a_indeterminate;   /* ... or after it: int x [[unclear]]; */
        if (constexpr_decl(t, name)) {
        } else if (is_func(t) && current_token.type == TOKEN_LBRACE) {
            if (!c2_ext) error("nested function '%s': nested functions need a .c2 file", name);
            if (unrolling) error("[[unroll]]: the loop body defines a function ('%s'); define it before the loop", name);
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
            Var *v = &vars[nvars++]; memset(v, 0, sizeof *v); v->serial = ++decl_serial;
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
                Var *v = &vars[nvars++]; memset(v, 0, sizeof *v); v->serial = ++decl_serial;
                strcpy(v->name, name);
                v->ty = g->ty;
                v->depth = scope_depth;
                v->is_mem = 1;
                v->reg = -1;
                v->global = (int)(g - globals);
            } else if (is_static) {
                if (unrolling) error("[[unroll]]: the loop body has a static variable ('%s'); declare it before the loop", name);
                for (int i = nvars - 1; i >= 0 && vars[i].depth == scope_depth; i--)
                    if (strcmp(vars[i].name, name) == 0) error("redeclaration of '%s'", name);
                if (is_struct_val(t) && !structs[t.sidx].complete) error("variable '%s' has incomplete type", name);
                char gname[40];
                snprintf(gname, sizeof gname, "__c2_s%d_%.24s", static_counter++, name);
                Global *g = add_global(gname, t);
                if (nvars >= 2048) error("too many variables");
                Var *v = &vars[nvars++]; memset(v, 0, sizeof *v); v->serial = ++decl_serial;
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
                    v->uninit = !indet;
                    store_ex(lv, zero_of(t), 1);
                } else if (is_aggregate(t) && indet) {     /* .c2 [[indeterminate]]: left as it is */
                } else if (is_aggregate(t)) {
                    zero_mem(lv.base, lv.disp, size_of(t));    /* .c2: everything starts zeroed */
                } else {
                    store_ex(lv, zero_of(t), 1);
                }
            }
        }
        temps_flush(tm);
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
    if (expr_makes_temps()) {                          /* value first, then the temporaries go, then the jump */
        int m = ntemps;
        Val v = rvalue(parse_expr());
        if (v.op.k != OK_INT && v.op.k != OK_FLT && v.op.k != OK_DBL) {
            RegKind rk = reg_kind(v.ty);
            int t = new_temp(rk);
            EMIT_OP(mov_insn(rk), R(t), v.op);
            v.op = R(t);
        }
        temps_flush(m);
        emit_cond_jump(v, when_true, label);
        return;
    }
    if (cj_is_logical()) cj_or(when_true, label);
    else emit_cond_jump(parse_expr(), when_true, label);
}

static int loop_scope[128];        /* scope_depth when each loop / switch began */
static void push_loop(int brk, int cont) {
    if (loop_depth >= 128) error("nesting too deep");
    break_stack[loop_depth] = brk;
    loop_scope[loop_depth] = scope_depth;
    continue_stack[loop_depth++] = cont;
}

/* defer (defer TS / C2y; `_Defer` everywhere, `defer` in .c2): `defer stmt;` runs stmt when its block
   is left: at the closing brace, return, break or continue, latest first.  The statement is parsed
   where it stands (checks only) and compiled again at each exit, with the names it saw there. */
typedef struct { LexState at; int depth, nvars, line; } Defer;
static Defer defers[128];
static int ndefers, defer_loop_base, stmt_direct;

static void emit_defer(Defer *d) {
    LexState after = save_lex();
    int saved_n = nvars, sd = scope_depth, lb = defer_loop_base;
    Var *saved = NULL;
    if (nvars > d->nvars) {                          /* names declared after the defer are not seen */
        saved = malloc((nvars - d->nvars) * sizeof *saved);
        memcpy(saved, vars + d->nvars, (nvars - d->nvars) * sizeof *saved);
    }
    nvars = d->nvars;
    scope_depth = 100000 + 100 * in_defer;           /* its own blocks don't touch the live ones */
    defer_loop_base = loop_depth;
    in_defer++;
    restore_lex(d->at);
    parse_statement();
    in_defer--;
    restore_lex(after);
    if (saved) { memcpy(vars + d->nvars, saved, (saved_n - d->nvars) * sizeof *saved); free(saved); }
    nvars = saved_n;
    scope_depth = sd;
    defer_loop_base = lb;
}

static void run_defers(int above_depth) {           /* the defers of the blocks deeper than above_depth */
    for (int i = ndefers - 1; i >= 0 && defers[i].depth > above_depth; i--) emit_defer(&defers[i]);
}

static void parse_defer(int direct) {
    int line = cur_line;
    next_token();
    if (!direct) error("defer must be a statement of a { block }");
    if (fn_used_goto) error("defer in a function that uses goto or labels is not supported");
    if (ndefers >= 128) error("too many defers in a function");
    Defer d = {save_lex(), scope_depth, nvars, line};
    EmitMark em = emit_mark();                       /* parse it now for errors; compile it at each exit */
    int nd = ndefers, lb = defer_loop_base;
    defer_loop_base = loop_depth;
    no_uninit_warn++;
    in_defer++;
    parse_statement();
    in_defer--;
    no_uninit_warn--;
    defer_loop_base = lb;
    emit_rollback(em);
    ndefers = nd;
    defers[ndefers++] = d;
    fn_used_defer = 1;
}

static void leave_block_defers(void) {              /* at a block's closing brace */
    run_defers(scope_depth - 1);
    while (ndefers > 0 && defers[ndefers - 1].depth >= scope_depth) ndefers--;
}

/* GNU statement expression: ({ stmt; ... expr; }) is the value of its last expression statement.
   Current token: the '{'. */
static int is_expr_stmt_start(void) {
    switch (current_token.type) {
    case TOKEN_LBRACE: case TOKEN_SEMI: case TOKEN_BREAK: case TOKEN_CONTINUE: case TOKEN_GOTO: case TOKEN_IF:
    case TOKEN_WHILE: case TOKEN_DO: case TOKEN_FOR: case TOKEN_SWITCH: case TOKEN_CASE: case TOKEN_DEFAULT:
    case TOKEN_RETURN: case TOKEN_DEFER: case TOKEN_STATIC_ASSERT: case TOKEN_TYPEDEF: return 0;
    case TOKEN_IDENT: if (peek_token().type == TOKEN_COLON && !is_type_start(current_token)) return 0; break;
    default: break;
    }
    return !is_type_start(current_token) || ctor_ahead();
}

static Val stmt_expr(void) {
    if (!cur_func) error("statement expression outside a function");
    next_token();
    enter_scope();
    Val result = rv_op(IMM(0), mk_type(TY_VOID));
    while (current_token.type != TOKEN_RBRACE && current_token.type != TOKEN_EOF) {
        if (current_token.type == TOKEN_IDENT && peek_token().type == TOKEN_COLON && !is_type_start(current_token)) {
            if (fn_used_defer || in_defer) error("labels in a function that uses defer are not supported");
            fn_used_goto = 1;                          /* label: — the statement after it may still be the value */
            place_label(goto_label(current_token.text, 1));
            next_token(); next_token();
            continue;
        }
        if (!is_expr_stmt_start()) { stmt_direct = 1; parse_statement(); result = rv_op(IMM(0), mk_type(TY_VOID)); continue; }
        int tm = ntemps;
        Val v = parse_expr();
        expect(TOKEN_SEMI);
        if (current_token.type != TOKEN_RBRACE || is_void(v.ty)) { rvalue(v); temps_flush(tm); result = rv_op(IMM(0), mk_type(TY_VOID)); continue; }
        if (is_struct_val(v.ty)) {                   /* a struct: into a slot of its own */
            int size = size_of(v.ty), base = new_slot("_sx", size), moved = ntemps && temp_index(v) >= 0;
            mark_fresh(base);                          /* the value of ({ }) moves: array_pop(a) */
            copy_mem(base, 0, v.base, v.disp, size);
            result = lv_mem(base, 0, v.ty);
            if (moved) { temp_moved(v); temp_add(result); }   /* still a temporary, of the outer statement */
        } else {                                     /* kept in a fresh register: the block's defers run after */
            Val r = rvalue(v);
            RegKind rk = reg_kind(r.ty);
            int t = new_temp(rk);
            EMIT_OP(mov_insn(rk), R(t), r.op);
            result = rv_reg(t, r.ty);
        }
    }
    expect(TOKEN_RBRACE);
    leave_block_defers();
    leave_scope();
    return result;
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
        f->is_import = !is_defined_in_file(name) && !is_defined_in_file(demangle(name));   /* (a module's private __mN_f) */
    }
    return f;
}

/* C2_set_debug: a call stack the host can print (C2_backtrace, crash handler).  Each function keeps a
   frame {prev, function, file, line} in its stack slot, linked from __c2_dbg_top on entry and unlinked
   at every return; each statement stores its line.  So a crash report says where every frame was. */
static int dbg_on, dbg_slot = -1, dbg_live;
#define DBG_VARS 24                                    /* a frame shows up to this many variables at once */
#define DBG_FRAME (40 + 24 * DBG_VARS)                 /* prev, func, file, line, live; {name, kind, addr}... */
static int dbg_keep_vars(void) { return dbg_on && cur_func != NULL; }
static void dbg_var(Var *v, int param) {               /* -g: the frame learns where v is */
    v->dbg = 0;
    if (dbg_slot < 0 || !v->is_mem || v->global >= 0 || !v->name[0] || dbg_live >= DBG_VARS) return;
    Type t = v->ty;
    int cat = is_array(t) ? 8 : is_struct_val(t) ? 7 : is_pointer(t) || is_func(t) ? 4 : is_float(t) || is_double(t) ? 3 :
              is_bool(t) ? 5 : t.base == TY_CHAR && !t.uns ? 6 : is_integer(t) ? (t.uns ? 2 : 1) : 0;
    if (!cat) return;
    int ix = dbg_live++, off = 40 + 24 * ix, r = new_temp(RK_I);
    EMIT(MOV, R(r), REF(be_string(v->name, (int)strlen(v->name))));
    EMIT(MOV, MEM(MT_I64, off, dbg_slot), R(r));
    EMIT(MOV, MEM(MT_I64, off + 8, dbg_slot), IMM((long long)(cat << 8 | (cat >= 7 ? 0 : size_of(t))) | (param ? 1 << 16 : 0)));
    EMIT(MOV, MEM(MT_I64, off + 16, dbg_slot), R(v->reg));
    EMIT(MOV, MEM(MT_I64, 32, dbg_slot), IMM(dbg_live));
    v->dbg = ix + 1;
}
static void dbg_vars_end(int live) {                   /* a block ended: its variables are gone */
    if (dbg_slot < 0 || !cur_func) return;
    dbg_live = live;
    EMIT(MOV, MEM(MT_I64, 32, dbg_slot), IMM(live));
}
static void dbg_enter(void) {
    Global *g = find_global("__c2_dbg_top");
    if (!g) return;
    ensure_global_storage(g);
    dbg_slot = new_slot("_dbg", DBG_FRAME);
    dbg_live = 0;
    const char *strs[2] = {demangle(cur_func->name), cur_file};
    int a = new_temp(RK_I), t = new_temp(RK_I);
    EMIT(MOV, R(a), REF(g->name));
    EMIT(MOV, R(t), MEM(MT_I64, 0, a));
    EMIT(MOV, MEM(MT_I64, 0, dbg_slot), R(t));
    for (int i = 0; i < 2; i++) {
        int r = new_temp(RK_I);
        EMIT(MOV, R(r), REF(be_string(strs[i], (int)strlen(strs[i]))));
        EMIT(MOV, MEM(MT_I64, 8 + 8 * i, dbg_slot), R(r));
    }
    EMIT(MOV, MEM(MT_I64, 24, dbg_slot), IMM(cur_line));
    EMIT(MOV, MEM(MT_I64, 32, dbg_slot), IMM(0));
    EMIT(MOV, MEM(MT_I64, 0, a), R(dbg_slot));
    for (int i = 0; i < nvars; i++)                    /* the parameters, as they are now */
        if (vars[i].depth == scope_depth && vars[i].global < 0) dbg_var(&vars[i], 1);
}
static void dbg_leave(void) {
    Global *g = find_global("__c2_dbg_top");
    if (dbg_slot < 0 || !g) return;
    int a = new_temp(RK_I), t = new_temp(RK_I);
    EMIT(MOV, R(a), REF(g->name));
    EMIT(MOV, R(t), MEM(MT_I64, 0, dbg_slot));
    EMIT(MOV, MEM(MT_I64, 0, a), R(t));
}
static void dbg_line(void) {
    if (dbg_slot >= 0 && cur_func) EMIT(MOV, MEM(MT_I64, 24, dbg_slot), IMM(cur_line));
}

static int running_on;                     /* C2_safe_reload */
static void emit_hook(const char *name) {
    if (running_on && cur_func && strncmp(cur_func->name, "__c2_", 5)) {   /* __c2_running += 1 (enter) or -1 */
        Global *g = find_global("__c2_running");
        if (g) {
            int a = new_temp(RK_I), t = new_temp(RK_I);
            EMIT(MOV, R(a), REF(g->name));
            EMIT(MOV, R(t), MEM(MT_I64, 0, a));
            EMIT(ADD, R(t), R(t), IMM(strcmp(name, "__on_enter") ? -1 : 1));
            EMIT(MOV, MEM(MT_I64, 0, a), R(t));
        }
    }
    if (dbg_on && cur_func) {
        if (!strcmp(name, "__on_enter")) dbg_enter();
        else dbg_leave();
    }
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
static int tail_param_var[MAX_PARAMS];      /* -g: the variable (in memory) of each, or -1 */
static int parse_args(Val *args);

static int must_tail, ret_musttail;                                 /* [[musttail]] return ...: a jump, or an error */
static int try_tail_call_(Val *args);
static int try_tail_call(void) {   /* current token: the first after `return`; 1 if it emitted the jump */
    Val *args = args_push();
    int r = try_tail_call_(args);
    args_pop();
    return r;
}
static int try_tail_call_(Val *args) {
    if (current_token.type != TOKEN_IDENT || !cur_func || tail_label < 0 || find_var(current_token.text) ||
        find_func(current_token.text) != cur_func || peek_token().type != TOKEN_LPAREN) {
        if (must_tail && current_token.type == TOKEN_IDENT && peek_token().type == TOKEN_LPAREN && cur_func)
            error("[[musttail]]: only a call of the function itself ('%s') can become a jump", cur_func->name);
        if (must_tail) error("[[musttail]] needs `return f(...);` with f the function itself");
        return 0;
    }
    LexState ls = save_lex();
    EmitMark em = emit_mark();
    Func *f = cur_func;
    next_token();
    Callee ce = callee_of_func(f);
    no_uninit_warn++;                                  /* (parsed again below if this is not a tail call) */
    int n = c2_ext ? parse_args_c2(&ce, args, NULL) : parse_args(args);
    no_uninit_warn--;
    if (current_token.type != TOKEN_SEMI) {           /* f(x) + 1 ... */
        if (must_tail) error("[[musttail]]: the call to '%s' is not the whole return value", f->name);
        emit_rollback(em); restore_lex(ls); return 0;
    }
    const char *why = NULL;
    if (hook_func("__on_enter") || hook_func("__on_leave")) why = "";   /* hooks want every call: no warning */
    else if (ndefers > 0) why = "a defer runs before it returns";
    else if (f->vararg) why = "it has '...'";
    else if (n != f->np) why = "the argument count differs";
    else if (naddr_taken > 0) why = "the address of a local or parameter is taken";
    else if (nvla_scopes > 0) why = "a variable-length array is in scope";
    else if (is_struct_val(f->ret)) why = "it returns a struct";
    for (int i = 0; !why && i < f->np; i++)
        if (is_struct_val(f->pt[i])) why = "it has struct parameters";
    if (why) {
        if (must_tail) error("[[musttail]]: the call to '%s' can't become a jump: %s", f->name, why[0] ? why : "hooks are on");
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
    for (int i = 0; i < n; i++)                        /* -g keeps the parameters in memory: the new values too */
        if (tail_param_var[i] >= 0) {
            Opnd o[2] = {MEM(mem_type(f->pt[i]), 0, vars[tail_param_var[i]].reg), R(tmp[i])};
            emit_ops(0, mov_insn(reg_kind(f->pt[i])), o, 2);
        }
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
            run_defers(0); emit_hook("__on_leave");
            emit_ops(0, I_RET, z, nq);
        } else if (is_struct_val(rt)) {
            Opnd a = R(cur_sret_reg);
            run_defers(0); emit_hook("__on_leave");
            emit_ops(0, I_RET, &a, 1);
        } else if (is_void(rt)) { run_defers(0); emit_hook("__on_leave"); emit_ops(0, I_RET, NULL, 0); }
        else { run_defers(0); emit_hook("__on_leave"); EMIT(RET, zero_of(rt).op); }
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
        temps_emit(0);
        run_defers(0); emit_hook("__on_leave");
        emit_ops(0, I_RET, ro, nq);
    } else if (is_struct_val(rt)) {                   /* hidden result buffer */
        copy_mem(cur_sret_reg, 0, r.base, r.disp, size_of(rt));
        Opnd a = R(cur_sret_reg);
        temps_emit(0);
        run_defers(0); emit_hook("__on_leave");
        emit_ops(0, I_RET, &a, 1);
    } else {
        if ((r.op.k != OK_REG || ndefers > 0 || ntemps > 0) && r.op.k != OK_INT && r.op.k != OK_FLT && r.op.k != OK_DBL) {
            RegKind rk = reg_kind(plain);              /* keep the value in a register across the hook (and defers) */
            int t = new_temp(rk);
            EMIT_OP(mov_insn(rk), R(t), r.op);
            r.op = R(t);
        }
        temps_emit(0);
        run_defers(0); emit_hook("__on_leave");
        EMIT(RET, r.op);
    }
}

/* is this expression statement just a call: f(...);  s.a(...).b(...);  p->f(...); */
static int stmt_is_call(void) {
    if (current_token.type != TOKEN_IDENT) return 0;
    LexState ls = save_lex();
    int ok = 1, called = 0;
    next_token();
    for (;;) {
        if (current_token.type == TOKEN_LPAREN) {
            int d = 0;
            do {
                if (current_token.type == TOKEN_LPAREN) d++;
                else if (current_token.type == TOKEN_RPAREN) d--;
                else if (current_token.type == TOKEN_EOF) { ok = 0; break; }
                next_token();
            } while (d > 0);
            called = 1;
        } else if (current_token.type == TOKEN_DOT || current_token.type == TOKEN_ARROW) {
            next_token();
            if (current_token.type != TOKEN_IDENT) { ok = 0; break; }
            next_token();
            called = 0;
        } else break;
        if (!ok) break;
    }
    int r = ok && called && current_token.type == TOKEN_SEMI;
    restore_lex(ls);
    return r;
}

/* [[unroll]] / [[unroll(N)]] before a loop: the body is compiled N times in a row, the condition
   tested between the copies (so any trip count works), and one jump back per N iterations.  With no N,
   `for (i = A; i < B; i++)` with constant A and B unrolls fully (up to 64 copies); otherwise 4. */

static int unroll_count(int attr, LexState *init, LexState *cond, LexState *inc) {
    if (attr > 0) return attr;
    if (!init || !cond || !inc) return 4;
    LexState here = save_lex();
    int n = 4;
    char var[64] = "";
    long long a = 0, b = 0, step = 0;
    int op = 0, ok = 0;
    restore_lex(*init);                                /* [type] i = A ; */
    for (int k = 0; k < 8 && current_token.type != TOKEN_SEMI && current_token.type != TOKEN_EOF; k++) {
        if (current_token.type == TOKEN_IDENT && peek_token().type == TOKEN_ASSIGN) {
            snprintf(var, sizeof var, "%s", current_token.text);
            next_token(); next_token();
            ce_fail = 0;
            CV v = ce_cond();
            ok = !ce_fail && !v.fp && current_token.type == TOKEN_SEMI;
            a = v.i;
            break;
        }
        next_token();
    }
    if (ok) {                                          /* i < B ; */
        restore_lex(*cond);
        ok = current_token.type == TOKEN_IDENT && !strcmp(current_token.text, var);
        if (ok) {
            next_token();
            op = current_token.type;
            ok = op == TOKEN_LT || op == TOKEN_LE || op == TOKEN_GT || op == TOKEN_GE || op == TOKEN_NE;
            if (ok) {
                next_token();
                ce_fail = 0;
                CV v = ce_cond();
                ok = !ce_fail && !v.fp && current_token.type == TOKEN_SEMI;
                b = v.i;
            }
        }
    }
    if (ok) {                                          /* i++ ++i i-- --i i += K i -= K */
        restore_lex(*inc);
        ok = 0;
        if (current_token.type == TOKEN_INC || current_token.type == TOKEN_DEC) {
            step = current_token.type == TOKEN_INC ? 1 : -1;
            next_token();
            ok = current_token.type == TOKEN_IDENT && !strcmp(current_token.text, var);
            if (ok) next_token();
        } else if (current_token.type == TOKEN_IDENT && !strcmp(current_token.text, var)) {
            next_token();
            TokenType t = current_token.type;
            if (t == TOKEN_INC || t == TOKEN_DEC) { step = t == TOKEN_INC ? 1 : -1; ok = 1; next_token(); }
            else if (t == TOKEN_PLUS_ASSIGN || t == TOKEN_MINUS_ASSIGN) {
                next_token();
                ce_fail = 0;
                CV v = ce_cond();
                ok = !ce_fail && !v.fp;
                step = t == TOKEN_PLUS_ASSIGN ? v.i : -v.i;
            }
        }
        ok = ok && current_token.type == TOKEN_RPAREN && step != 0;
    }
    if (ok) {
        long long trips = -1;
        if (step > 0 && (op == TOKEN_LT || op == TOKEN_NE)) trips = a < b ? (b - a + step - 1) / step : 0;
        if (step > 0 && op == TOKEN_LE) trips = a <= b ? (b - a) / step + 1 : 0;
        if (step < 0 && (op == TOKEN_GT || op == TOKEN_NE)) trips = a > b ? (a - b - step - 1) / -step : 0;
        if (step < 0 && op == TOKEN_GE) trips = a >= b ? (a - b) / -step + 1 : 0;
        if (op == TOKEN_NE && trips >= 0 && (b - a) % step != 0) trips = -1;
        if (trips >= 1 && trips <= 64) n = (int)trips;
    }
    restore_lex(here);
    return n;
}
/* one copy of an unrolled body: what can't be repeated is an error, its warnings are said once */
static void unroll_body(int copy, int brk, int cont, int on) {
    if (!on) { push_loop(brk, cont); parse_statement(); loop_depth--; return; }   /* a plain loop */
    int sw = unroll_sw_base;
    if (unrolling++ == 0) unroll_sw_base = sw_depth;
    if (copy > 0) warn_mute++;
    push_loop(brk, cont);
    parse_statement();
    loop_depth--;
    if (copy > 0) warn_mute--;
    if (--unrolling == 0) unroll_sw_base = sw;
}

/* ---- for (T x : things): one loop for everything (.c2).  The header is rewritten as a plain for's
   (declarations; condition; increment), parsed from a buffer of its own; the body stays where it is.
     ranges     for (int i : 0..n)  0..n..2  n..0 (n-1 down to 0)  n..0..-2  -3..3   (half-open [low, high))
     arrays     T [N], array(T): for (T x : a) copies each item, for (T *x : a) points at it
     maps, sets for (auto *e : m) its entries (e.key, e.value); m.keys(), m.values(); a set's keys
     text       string, char [N], const char *: for (char c : s) bytes, for (u32 c : s) UTF-8 characters
     types      for (const c2field *f : reflect(T)) its fields; any T with bool T_next(T *it, X *out) */
static int foreach_counter;
static void fe_addr(const char *fmt, ...) {            /* a variable the generated header takes the address of */
    if (naddr_taken >= 512) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(addr_taken[naddr_taken++], 32, fmt, ap);
    va_end(ap);
}
static void fe_trim(char *s) {
    char *b = s;
    while (*b == ' ' || *b == '\t' || *b == '\n' || *b == '\r') b++;
    memmove(s, b, strlen(b) + 1);
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\n' || s[n - 1] == '\r')) s[--n] = '\0';
}
static Type fe_text_type(const char *text, int is_type) {   /* the type of an expression (or a type) written as text */
    LexState ls = save_lex();
    int line = cur_line;
    src = text;
    next_token();
    Type t = is_type ? parse_type_name() : dry_run_type(parse_expr, 0);   /* (a dry run puts the lexer back) */
    if (is_type && current_token.type != TOKEN_EOF) error("for (%s ...): unexpected '%s'", text, current_token.text);
    restore_lex(ls);
    cur_line = line;
    return t;
}
static int fe_int_literal(const char *text, long long *v) {   /* -30, +10, 0 (the preprocessor spaces "- 10") */
    char s[64];
    int k = 0;
    for (const char *t = text; *t && k < 63; t++) if (*t != ' ' && *t != '\t') s[k++] = *t;
    s[k] = '\0';
    const char *p = s;
    if (*p == '+' || *p == '-') p++;
    if (!isdigit((unsigned char)*p)) return 0;
    for (const char *q = p; *q; q++) if (!isdigit((unsigned char)*q)) return 0;
    *v = strtoll(s, NULL, 10);
    return 1;
}
/* the header after "for (": returns the number of declarations it generated (0: not a for-each, nothing
   consumed).  The lexer is left on the '(' of the generated header; *resume continues after the ')'. */
static int foreach_header(LexState *resume) {
    const char *p = src, *colon = NULL, *close = NULL;
    int depth = 0, semi = 0, lines = 0;
    for (const char *q = p; *q; q++) {
        if (*q == '\n') lines++;
        if (*q == '"' || *q == '\'') { char c = *q; for (q++; *q && *q != c; q++) { if (*q == '\\' && q[1]) q++; if (*q == '\n') lines++; } if (!*q) break; continue; }
        if (q[0] == '/' && q[1] == '/') { while (*q && *q != '\n') q++; if (!*q) break; lines++; continue; }
        if (q[0] == '/' && q[1] == '*') { for (q += 2; *q && !(q[0] == '*' && q[1] == '/'); q++) if (*q == '\n') lines++; if (!*q) break; q++; continue; }
        if (*q == '(' || *q == '[' || *q == '{') depth++;
        else if (*q == ')' || *q == ']' || *q == '}') { if (depth == 0) { close = q; break; } depth--; }
        else if (*q == ';' && depth == 0) semi = 1;
        else if (*q == ':' && depth == 0 && !colon && q[1] != ':' && (q == p || q[-1] != ':')) colon = q;
    }
    int sep = 1;
    if (!close || semi || !colon) return 0;
    int n = ++foreach_counter;
    char dtext[256], etext[1024], vname[64], dtype[256];
    fmt_to(dtext, sizeof dtext, "%.*s", (int)(colon - p), p);
    fmt_to(etext, sizeof etext, "%.*s", (int)(close - colon - sep), colon + sep);
    fe_trim(dtext);
    fe_trim(etext);
    char disp[1024];                                   /* etext for messages, as written: "a..b..-1", not "a . . b . . - 1" */
    {
        int k = 0;
        for (const char *q = etext; *q && k < (int)sizeof disp - 1; q++) {
            if (*q == ' ' && k > 0 && (disp[k - 1] == '.' || disp[k - 1] == '(' || (disp[k - 1] == '-' && k > 1 && disp[k - 2] == '.'))) continue;
            if (*q == ' ' && (q[1] == '.' || q[1] == ')' || q[1] == '(')) continue;
            disp[k++] = *q;
        }
        disp[k] = '\0';
    }
    size_t dl = strlen(dtext);                         /* T x: the name last */
    size_t ns = dl;
    while (ns > 0 && (isalnum((unsigned char)dtext[ns - 1]) || dtext[ns - 1] == '_')) ns--;
    if (ns == dl || ns == 0 || isdigit((unsigned char)dtext[ns])) error("for (T name : ...): a declaration expected before ':', not '%s'", dtext);
    fmt_to(vname, sizeof vname, "%s", dtext + ns);
    fmt_to(dtype, sizeof dtype, "%.*s", (int)ns, dtext);
    fe_trim(dtype);
    int is_ptr = dtype[strlen(dtype) - 1] == '*';
    int is_auto = strstr(dtype, "auto") != NULL;
    int line0 = cur_line;
    Buf b = {0};
    int ndecls = 0;
    char cond[1400] = "", inc[200] = "";
    /* ranges: a..b, a..b..step */
    char parts[3][400];
    int np = 0;
    {
        int d = 0;
        const char *s = etext, *start = etext;
        for (; *s; s++) {
            if (*s == '"' || *s == '\'') { char c = *s; for (s++; *s && *s != c; s++) if (*s == '\\' && s[1]) s++; if (!*s) break; continue; }
            if (*s == '(' || *s == '[' || *s == '{') d++;
            else if (*s == ')' || *s == ']' || *s == '}') d--;
            else if (d == 0 && s[0] == '.' && (s == etext || s[-1] != '.')) {   /* "..", or ". ." as the preprocessor spaced it */
                const char *t = s + 1;
                while (*t == ' ' || *t == '\t') t++;
                if (*t != '.' || t[1] == '.') continue;
                if (np < 3) fmt_to(parts[np++], sizeof parts[0], "%.*s", (int)(s - start), start);
                s = t;
                start = s + 1;
            }
        }
        if (np) {
            if (np > 2) error("for (... : %s): a range is from..to or from..to..step", disp);
            fmt_to(parts[np++], sizeof parts[0], "%s", start);
            for (int i = 0; i < np; i++) { fe_trim(parts[i]); if (!parts[i][0]) error("for (... : %s): a range's part is missing", disp); }
        }
    }
    if (np) {
        long long from, to, step = 0;
        int cf = fe_int_literal(parts[0], &from), ct = fe_int_literal(parts[1], &to);
        if (np == 3) {
            if (!fe_int_literal(parts[2], &step) || step == 0)
                error("for (... : %s): the step is a constant, not 0 (..10, ..-10); a variable step: a plain for loop", disp);
            if (cf && ct && ((step > 0 && from > to) || (step < 0 && from < to)))
                error("for (... : %s): the step %lld goes the other way: %s", disp, step, step > 0 ? "a descending range takes a negative step" : "an ascending range takes a positive step");
        } else if (ct && to == 0 && !(cf && from <= 0)) step = -1;   /* n..0: down */
        else if (cf && from == 0) step = 1;                          /* 0..n: up */
        else if (cf && ct) step = from <= to ? 1 : -1;
        else error("for (... : %s): which way? give a step: %s..%s..1 counts up, %s..%s..-1 down", disp, parts[0], parts[1], parts[0], parts[1]);
        char ty[300];
        if (is_auto) fmt_to(ty, sizeof ty, "typeof((%s) + (%s))", parts[0], parts[1]);
        else fmt_to(ty, sizeof ty, "%s", dtype);
        long long k = step < 0 ? -step : step;
        if (step > 0) {
            buf_emit(&b, "%s %s = (%s), fe__e%d = (%s);", ty, vname, parts[0], n, parts[1]);
            fmt_to(cond, sizeof cond, "%s < fe__e%d", vname, n);
            fmt_to(inc, sizeof inc, "%s += %lld", vname, k);
        } else {                                       /* tested before each step down: unsigned works */
            buf_emit(&b, "%s %s = (%s) - 1 + %lld, fe__e%d = (%s) + %lld;", ty, vname, parts[0], k, n, parts[1], k);
            fmt_to(cond, sizeof cond, "%s >= fe__e%d && ((%s -= %lld), 1)", vname, n, vname, k);
        }
        ndecls = 1;
    } else {
        char base[1100], member[16] = "";
        fmt_to(base, sizeof base, "%s", etext);
        {                                              /* m.keys(), m.values() (spaced: "m . keys ( )") */
            char *e = base + strlen(base);
            while (e > base && e[-1] == ' ') e--;
            if (e > base && e[-1] == ')') {
                e--;
                while (e > base && e[-1] == ' ') e--;
                if (e > base && e[-1] == '(') {
                    e--;
                    while (e > base && e[-1] == ' ') e--;
                    char *w = e;
                    while (w > base && (isalnum((unsigned char)w[-1]) || w[-1] == '_')) w--;
                    char *d = w;
                    while (d > base && d[-1] == ' ') d--;
                    if (d > base && d[-1] == '.') {
                        size_t wl = (size_t)(e - w);
                        if (wl == 4 && !strncmp(w, "keys", 4)) { strcpy(member, "key"); d[-1] = '\0'; }
                        else if (wl == 6 && !strncmp(w, "values", 6)) { strcpy(member, "value"); d[-1] = '\0'; }
                        else if (wl == 6 && !strncmp(w, "glyphs", 6))
                            error("for (... : %s): glyphs aren't walked yet (they need Unicode's tables); u32 walks characters", disp);
                        fe_trim(base);
                    }
                }
            }
        }
        Type t = fe_text_type(base, 0);
        int ck = is_pointer(t) ? t.d[t.nd - 1].ck : 0;
        if (member[0] && ck != C2_CK_MAP) error("for (... : %s): keys() and values() are a map's", disp);
        int bytes = !strcmp(dtype, "char") || !strcmp(dtype, "u8") || !strcmp(dtype, "i8") || !strcmp(dtype, "unsigned char") ||
                    !strcmp(dtype, "signed char") || !strcmp(dtype, "const char");
        int chars = !strcmp(dtype, "u32") || !strcmp(dtype, "char32_t") || !strcmp(dtype, "uint32_t") || !strcmp(dtype, "unsigned") ||
                    !strcmp(dtype, "unsigned int");
        int text_s = refl_string(t), text_z = is_pointer(t) && !ck && elem_of(t).base == TY_CHAR && !elem_of(t).nd,
            text_a = is_array(t) && elem_of(t).base == TY_CHAR && !elem_of(t).nd && (bytes || chars);
        if (text_s || text_z || text_a) {
            if (!bytes && !chars)
                error("for (%s : %s): text walks as char (its bytes) or u32 (its UTF-8 characters)", dtext, disp);
            if (text_s) {
                buf_emit(&b, "string *fe__s%d = &(%s); const char *fe__p%d = fe__s%d.data(), *fe__e%d = fe__p%d + fe__s%d.len(); %s %s;",
                         n, etext, n, n, n, n, n, dtype, vname);
                ndecls = 3;
            } else if (text_a) {
                buf_emit(&b, "const char *fe__p%d = (%s), *fe__e%d = fe__p%d + countof(%s); %s %s;", n, etext, n, n, etext, dtype, vname);
                ndecls = 2;
            } else {
                buf_emit(&b, "const char *fe__p%d = (%s), *fe__e%d = NULL; %s %s;", n, etext, n, dtype, vname);
                ndecls = 2;
            }
            char bound[80];
            if (text_z) fmt_to(bound, sizeof bound, "*fe__p%d", n);
            else if (text_a) fmt_to(bound, sizeof bound, "fe__p%d < fe__e%d && *fe__p%d", n, n, n);
            else fmt_to(bound, sizeof bound, "fe__p%d < fe__e%d", n, n);
            if (bytes) {
                fmt_to(cond, sizeof cond, "%s && ((%s = *fe__p%d), 1)", bound, vname, n);
                fmt_to(inc, sizeof inc, "fe__p%d++", n);
            } else {
                fmt_to(cond, sizeof cond, "%s && ((%s = __c2_utf8(&fe__p%d, fe__e%d)), 1)", bound, vname, n, n);
                fe_addr("fe__p%d", n);
            }
        } else if (is_array(t) || ck) {               /* arrays, array(T), maps, sets */
            char first[1200], count[1200];
            if (is_array(t)) {
                fmt_to(first, sizeof first, "&(%s)[0]", base);
                fmt_to(count, sizeof count, "countof(%s)", base);
            } else {
                buf_emit(&b, "typeof(%s) fe__a%d = (%s); ", base, n, base);
                ndecls++;
                fmt_to(first, sizeof first, "fe__a%d", n);
                fmt_to(count, sizeof count, ck == C2_CK_ARRAY ? "array_count(fe__a%d)" : "map_count(fe__a%d)", n);
            }
            if (ck == C2_CK_SET && !member[0]) strcpy(member, "key");   /* a set: its keys */
            char item[1300];                           /* one item, written with the expression (for its type) */
            if (member[0]) fmt_to(item, sizeof item, "(%s)->%s", base, member);
            else fmt_to(item, sizeof item, "*(%s)", base);   /* (not [0]: a map's m[k] is by key) */
            {                                          /* T *x: a pointer to each item, when T is the item's type */
                Type st0 = fe_text_type(item, 0);
                if (!is_auto) {
                    Type dt0 = fe_text_type(dtype, 1);
                    Type se = st0, de = is_pointer(dt0) ? elem_of(dt0) : dt0;
                    se.bcq = de.bcq = 0;
                    is_ptr = is_pointer(dt0) && same_type(unconst(de), unconst(se));
                }
            }
            if (is_ptr && !member[0]) {                /* in place */
                char ty[1400];
                if (is_auto) fmt_to(ty, sizeof ty, "typeof(*(%s)) *", first);
                else fmt_to(ty, sizeof ty, "%s", dtype);
                buf_emit(&b, "%s %s = %s, *fe__e%d = %s + %s;", ty, vname, first, n, vname, count);
                ndecls++;
                fmt_to(cond, sizeof cond, "%s < fe__e%d", vname, n);
                fmt_to(inc, sizeof inc, "%s++", vname);
            } else {
                buf_emit(&b, "typeof(*(%s)) *fe__p%d = %s, *fe__e%d = fe__p%d + %s; ", first, n, first, n, n, count);
                char ty[1400];
                if (!is_auto) fmt_to(ty, sizeof ty, "%s", dtype);
                else if (is_ptr) fmt_to(ty, sizeof ty, "typeof(%s) *", item);   /* m.keys() by pointer */
                else fmt_to(ty, sizeof ty, "typeof(%s)", item);
                buf_emit(&b, "%s %s;", ty, vname);
                ndecls += 2;
                char src_item[200];
                if (member[0]) fmt_to(src_item, sizeof src_item, "fe__p%d->%s", n, member);
                else fmt_to(src_item, sizeof src_item, "*fe__p%d", n);
                Type st = fe_text_type(item, 0);
                Type dt = is_auto ? st : fe_text_type(dtype, 1);
                if (is_ptr) fmt_to(cond, sizeof cond, "fe__p%d < fe__e%d && ((%s = &%s), 1)", n, n, vname, src_item);
                else if (is_struct_val(dt) || is_struct_val(st) || is_array(st)) {   /* a copy that owns nothing */
                    if (!is_auto && !(is_struct_val(dt) && is_struct_val(st) && dt.sidx == st.sidx))
                        error("for (%s : %s): the items are %s", dtext, disp, type_name(st));
                    fmt_to(cond, sizeof cond, "fe__p%d < fe__e%d && (memcpy(&%s, &%s, sizeof %s), 1)", n, n, vname, src_item, vname);
                    fe_addr("%s", vname);
                } else fmt_to(cond, sizeof cond, "fe__p%d < fe__e%d && ((%s = %s), 1)", n, n, vname, src_item);
                fmt_to(inc, sizeof inc, "fe__p%d++", n);
            }
        } else if (is_pointer(t) && is_struct_val(elem_of(t)) && struct_name(elem_of(t).sidx) &&
                   !strcmp(struct_name(elem_of(t).sidx), "c2type")) {   /* reflect(T): its fields */
            buf_emit(&b, "const c2type *fe__t%d = (%s); const c2field *fe__e%d = fe__t%d.fields + fe__t%d.count; "
                         "%s %s = member_next(fe__t%d.fields, fe__e%d);",
                     n, etext, n, n, n, is_auto ? "const c2field *" : dtype, vname, n, n);
            ndecls = 3;
            fmt_to(cond, sizeof cond, "%s < fe__e%d", vname, n);
            fmt_to(inc, sizeof inc, "%s = member_next(%s + 1, fe__e%d)", vname, vname, n);
        } else {
            Type st = is_pointer(t) ? elem_of(t) : t;   /* a type with T_next(T *it, X *out) */
            char next[96] = "";
            if (is_struct_val(st) && struct_name(st.sidx)) fmt_to(next, sizeof next, "%s_next", struct_name(st.sidx));
            if (!next[0] || !find_func(next))
                error("for (... : %s): a %s can't be walked (ranges a..b, arrays, array(T), maps, sets, text, reflect(T), "
                      "or a type T with bool T_next(T *it, X *out))", etext, type_name(t));
            if (is_auto) error("for (auto ... : %s): give the type %s gives", disp, next);
            buf_emit(&b, "typeof(%s) fe__i%d = (%s); %s %s;", etext, n, etext, dtype, vname);
            ndecls = 2;
            fmt_to(cond, sizeof cond, "%s(%sfe__i%d, &%s)", next, is_pointer(t) ? "" : "&", n, vname);
            fe_addr("fe__i%d", n);
            fe_addr("%s", vname);
        }
    }
    /* the generated header, lexed in place of the written one */
    Buf raw = {0};
    buf_emit(&raw, "(");
    buf_append(&raw, b.p, b.len);
    buf_emit(&raw, " %s ; %s )", cond, inc);
    buf_append(&raw, "", 0);
    Buf h = {0};                                       /* macros expanded (array_count, map_count) */
    for (PPTok *t = expand_all(pp_tokenize(raw.p, 0)); t->kind != PK_EOF; t = t->next) { buf_append(&h, " ", 1); buf_append(&h, t->s, t->len); }
    buf_append(&h, "", 0);
    src = close;                                       /* where to come back: after the ')' */
    cur_line = line0 + lines;
    next_token();                                      /* ')' */
    next_token();                                      /* the body's first token */
    *resume = save_lex();
    src = h.p;
    cur_line = line0;
    next_token();                                      /* '(' */
    return ndecls;
}

/* foreach_field(T, X): a compile-time loop over T's fields: X(name) once per field, in order ([[skip]]
   fields left out, an anonymous struct's or union's fields included), as a block.  X is a macro, so the
   field's name can be pasted (x.name, #name): the same code for every field, checked and compiled as
   if written out by hand. */
static void field_names(int si, Buf *b, const char *macro) {
    for (int i = 0; i < structs[si].n; i++) {
        Member *m = &structs[si].m[i];
        if (m->skip) continue;
        if (!m->name[0]) { if (is_struct_val(m->ty)) field_names(m->ty.sidx, b, macro); continue; }
        buf_emit(b, " %s(%s)", macro, m->name);
    }
}
static void parse_foreach_field(void) {               /* current token: foreach_field */
    next_token();
    expect(TOKEN_LPAREN);
    Type t = parse_type_name();
    if (!is_struct_val(t) || !structs[t.sidx].complete) error("foreach_field(T, X): T is a struct or union, not '%s'", type_name(t));
    expect(TOKEN_COMMA);
    if (current_token.type != TOKEN_IDENT) error("foreach_field(T, X): X is a macro's name, X(field)");
    char macro[64];
    snprintf(macro, sizeof macro, "%s", current_token.text);
    next_token();
    if (current_token.type != TOKEN_RPAREN) error("foreach_field(T, X): ')' expected");
    next_token();
    if (current_token.type == TOKEN_SEMI) next_token();
    Buf text = {0};
    buf_emit(&text, "{");
    field_names(t.sidx, &text, macro);
    buf_emit(&text, " }");
    LexState after = save_lex();
    PPTok *tk = expand_all(pp_tokenize(text.p, 0));
    Buf b = {0};
    for (; tk->kind != PK_EOF; tk = tk->next) { buf_append(&b, " ", 1); buf_append(&b, tk->s, tk->len); }
    buf_append(&b, "", 0);
    prescan_addr_taken_ex(b.p, 0);
    int line = cur_line;
    src = b.p;
    next_token();
    cur_line = line;
    parse_statement();
    if (current_token.type != TOKEN_EOF) error("foreach_field(%s, %s): unexpected '%s' in the expansion", type_name(t), macro, current_token.text);
    restore_lex(after);
    cur_line = line;
}

static void parse_statement(void) {
    if (dbg_slot >= 0) dbg_line();
    if (c2_ext && current_token.type == TOKEN_IDENT && !strcmp(current_token.text, "foreach_field") &&
        peek_token().type == TOKEN_LPAREN && !find_func("foreach_field") && !find_var("foreach_field")) {
        parse_foreach_field();
        return;
    }
    int direct = stmt_direct;                          /* a direct statement of a { block } */
    stmt_direct = 0;
    int unroll = current_token.a_unroll;
    if (unroll && current_token.type != TOKEN_FOR && current_token.type != TOKEN_WHILE && current_token.type != TOKEN_DO)
        error("[[unroll]] goes before a for, while or do loop");
    switch (current_token.type) {
    case TOKEN_LBRACE:
        next_token();
        enter_scope();
        while (current_token.type != TOKEN_RBRACE && current_token.type != TOKEN_EOF) { stmt_direct = 1; parse_statement(); }
        expect(TOKEN_RBRACE);
        leave_block_defers();
        leave_scope();
        return;
    case TOKEN_DEFER:
        parse_defer(direct);
        return;
    case TOKEN_SEMI:
        next_token();
        return;
    case TOKEN_BREAK:
        next_token();
        expect(TOKEN_SEMI);
        if (loop_depth == 0) error("break outside loop/switch");
        if (in_defer && loop_depth <= defer_loop_base) error("break out of a defer");
        run_defers(loop_scope[loop_depth - 1]);
        EMIT(JMP, LBL(break_stack[loop_depth - 1]));
        return;
    case TOKEN_CONTINUE: {
        next_token();
        expect(TOKEN_SEMI);
        int d = loop_depth - 1;
        while (d >= 0 && continue_stack[d] < 0) d--;
        if (d < 0) error("continue outside loop");
        if (in_defer && d < defer_loop_base) error("continue out of a defer");
        run_defers(loop_scope[d]);
        EMIT(JMP, LBL(continue_stack[d]));
        return;
    }
    case TOKEN_GOTO: {
        next_token();
        if (current_token.type == TOKEN_ASTERISK) {    /* GNU computed goto: goto *p; (p from &&label) */
            next_token();
            if (fn_used_defer || in_defer) error("goto in a function that uses defer is not supported");
            fn_used_goto = 1;
            Val v = rvalue(parse_expr());
            if (!is_pointer(v.ty)) error("goto * needs a pointer (a label address, &&label), not '%s'", type_name(v.ty));
            expect(TOKEN_SEMI);
            EMIT(JMPI, R(to_reg(v)));
            return;
        }
        if (current_token.type != TOKEN_IDENT) error("label name expected after goto");
        if (fn_used_defer || in_defer) error("goto in a function that uses defer is not supported");
        fn_used_goto = 1;
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
        int n = unroll ? unroll_count(unroll, NULL, NULL, NULL) : 1;
        LexState body = save_lex();
        for (int k = 0; k < n; k++) {
            if (k > 0) { restore_lex(body); l_cont = new_label(); }
            unroll_body(k, l_brk, l_cont, unroll != 0);
            place_label(l_cont);
            LexState after = save_lex();
            restore_lex(cond);
            if (k < n - 1) cond_jump(0, l_brk);    /* between the copies */
            else cond_jump(1, l_top);              /* the condition again, compiled at the bottom */
            restore_lex(after);
        }
        place_label(l_brk);
        return;
    }
    case TOKEN_DO: {
        next_token();
        int l_top = new_label(), l_cont = new_label(), l_brk = new_label();
        place_label(l_top);
        int n = unroll ? unroll_count(unroll, NULL, NULL, NULL) : 1;
        LexState body = save_lex();
        for (int k = 0; k < n; k++) {
            if (k > 0) { restore_lex(body); l_cont = new_label(); }
            unroll_body(k, l_brk, l_cont, unroll != 0);
            place_label(l_cont);
            expect(TOKEN_WHILE);
            expect(TOKEN_LPAREN);
            if (k < n - 1) cond_jump(0, l_brk);
            else cond_jump(1, l_top);
            expect(TOKEN_RPAREN);
            expect(TOKEN_SEMI);
        }
        place_label(l_brk);
        return;
    }
    case TOKEN_FOR: {
        next_token();
        LexState fe_resume;
        int fe_decls = c2_ext && current_token.type == TOKEN_LPAREN ? foreach_header(&fe_resume) : 0;
        expect(TOKEN_LPAREN);
        enter_scope();                                 /* for (int i ...) is scoped to the loop */
        LexState init = save_lex();
        if (fe_decls) for (int k = 0; k < fe_decls; k++) parse_local_decl();   /* for (T x : things) */
        else if (is_type_start(current_token) && !ctor_ahead()) parse_local_decl();
        else {
            int tm = ntemps;
            if (current_token.type != TOKEN_SEMI) rvalue(parse_expr());
            temps_flush(tm);
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
        if (fe_decls) restore_lex(fe_resume);          /* the body, in the written source */
        place_label(l_top);
        int n = unroll ? unroll_count(unroll, &init, has_cond ? &cond : NULL, &inc) : 1;
        LexState body = save_lex();
        for (int k = 0; k < n; k++) {
            if (k > 0) { restore_lex(body); l_cont = new_label(); }
            unroll_body(k, l_brk, l_cont, unroll != 0);
            place_label(l_cont);
            LexState after = save_lex();
            restore_lex(inc);
            int tm = ntemps;
            if (current_token.type != TOKEN_RPAREN) rvalue(parse_expr());
            temps_flush(tm);
            if (has_cond) {
                restore_lex(cond);
                if (k < n - 1) cond_jump(0, l_brk);   /* between the copies */
                else cond_jump(1, l_top);
            } else if (k == n - 1) {
                EMIT(JMP, LBL(l_top));
            }
            restore_lex(after);
        }
        place_label(l_brk);
        leave_scope();
        return;
    }
    case TOKEN_SWITCH: {
        /* body is any statement; case/default labels may sit anywhere inside it (Duff's device).
           Dispatch code goes after the body: a chain of beq to the case labels. */
        next_token();
        expect(TOKEN_LPAREN);
        int tm = ntemps;
        Val sv = rvalue(parse_expr());
        if (!is_integer(sv.ty)) error("switch on '%s', which is not an integer", type_name(sv.ty));
        Type st = promote(sv.ty);
        sv = clean32(convert(sv, st));                 /* beq compares 64 bits */
        expect(TOKEN_RPAREN);
        int sw = new_temp(RK_I);                       /* evaluate once */
        EMIT(MOV, R(sw), sv.op);
        temps_flush(tm);
        int l_disp = new_label(), l_brk = new_label();
        if (sw_depth >= 64) error("switch nesting too deep");
        SwitchCtx *c = calloc(1, sizeof *c);
        c->def = -1;
        c->ty = st;
        c->depth = scope_depth;
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
        if (unrolling && sw_depth <= unroll_sw_base) error("[[unroll]]: the loop body has a '%s' of a switch around the loop", is_default ? "default" : "case");
        SwitchCtx *c = sw_stack[sw_depth - 1];
        if (ndefers > 0 && defers[ndefers - 1].depth > c->depth) error("case label after a defer in the same switch");
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
        if (in_defer) error("return inside a defer");
        ret_musttail = current_token.a_musttail;
        hook_line = cur_line;                          /* __on_leave reports the return's line */
        next_token();
        must_tail = ret_musttail;
        int tc = !ret_musttail && current_token.type != TOKEN_SEMI && expr_makes_temps() ? 0 : try_tail_call();
        must_tail = 0;
        if (tc) { hook_line = 0; return; }
        if (current_token.type == TOKEN_SEMI) {
            next_token();
            if (!is_void(cur_func->ret)) error("return without a value in a function returning '%s'", type_name(cur_func->ret));
            emit_return(NULL);
        } else {
            int m = ntemps;
            Val v = parse_expr();
            expect(TOKEN_SEMI);
            if (ncopies && !is_own_local(v)) v = copy_if_named(v);   /* a member, a global...: a copy */
            if (ntemps) temp_moved(v);                 /* returned: the caller owns it */
            emit_return(&v);
            ntemps = m;                                /* (destroyed by emit_return, on this path) */
        }
        return;
    case TOKEN_IDENT:
        if (peek_token().type == TOKEN_COLON && !is_type_start(current_token)) {   /* label: */
            if (fn_used_defer || in_defer) error("labels in a function that uses defer are not supported");
            fn_used_goto = 1;
            place_label(goto_label(current_token.text, 1));
            next_token();
            next_token();
            if (current_token.type != TOKEN_RBRACE) parse_statement();
            return;
        }
        /* fall through */
    default:
        if (current_token.type == TOKEN_STATIC_ASSERT) { parse_static_assert(); return; }
        if ((is_type_start(current_token) && !ctor_ahead()) || current_token.type == TOKEN_TYPEDEF) { parse_local_decl(); return; }
        int whole_call = stmt_is_call(), tm = ntemps;
        last_called = NULL;
        rvalue(parse_expr());
        temps_flush(tm);
        if (whole_call && last_called && last_called->nodiscard && !no_uninit_warn && !in_defer)
            diag(last_called->nodiscard == 2 ? "%s:%d: warning: the result of '%s' is discarded ([[pure]]: the call does nothing else)\n"
                                             : "%s:%d: warning: the result of '%s' is discarded ([[nodiscard]]; (void) to ignore it)\n",
                 cur_file, cur_line, last_called->name);
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

/* receiver overloading (.c2): the prescan notes each function name's first-parameter types (as words:
   "vec2", "unsigned int", "char"); a name declared with two or more of them is overloaded */
typedef struct { char name[32]; char recv[8][48]; int n; } OvlName;
static OvlName ovl_names[256];
static int novl_names;
static void ovl_note(const char *name, const char *recv) {
    OvlName *o = NULL;
    for (int i = 0; i < novl_names && !o; i++) if (!strcmp(ovl_names[i].name, name)) o = &ovl_names[i];
    if (!o) {
        if (novl_names >= 256) return;
        o = &ovl_names[novl_names++];
        snprintf(o->name, sizeof o->name, "%s", name);
        o->n = 0;
    }
    for (int i = 0; i < o->n; i++) if (!strcmp(o->recv[i], recv)) return;
    if (o->n < 8) snprintf(o->recv[o->n++], sizeof o->recv[0], "%s", recv);
}
static int host_typed(const char *name);
static int is_overloaded(const char *name) {
    for (int i = 0; i < novl_names; i++) if (!strcmp(ovl_names[i].name, name)) return ovl_names[i].n >= 2;
    return 0;
}

/* try/throw: which calls need the check after them.  The prescan notes, per function defined in the
   file, the names it calls and whether it calls throw() or through a pointer; and which functions are
   used as values (their address taken: callable from anywhere).  A function may throw when it calls
   throw(), or a function that may throw, or (when a function that may throw escaped as a value, other
   than as try()'s argument) a C function or a pointer, since those may call it back. */
typedef struct { char name[32]; int first, n; unsigned char throws, indirect, may; } ThrowFn;
static ThrowFn tfns[2048];
static int ntfns, ntcallee, nvalue_names, t_escaped;
static char (*tcallee)[33];                            /* name, with '.' first for a method call */
static char (*value_names)[32];
static int tfn_find(const char *name) {
    for (int i = 0; i < ntfns; i++) if (!strcmp(tfns[i].name, name)) return i;
    return -1;
}
static int tcallee_may(const char *c) {                /* does a call by this name possibly throw? */
    if (c[0] == '.') {                                 /* x.name(): name() or any T_name() */
        const char *m = c + 1;
        size_t ml = strlen(m);
        int found = 0, may = 0;
        for (int i = 0; i < ntfns; i++) {
            size_t nl = strlen(tfns[i].name);
            if (!strcmp(tfns[i].name, m) || (nl > ml + 1 && tfns[i].name[nl - ml - 1] == '_' && !strcmp(tfns[i].name + nl - ml, m))) {
                found = 1;
                may |= tfns[i].may;
            }
        }
        return found ? may : t_escaped;                /* (a member that is a function pointer) */
    }
    int i = tfn_find(c);
    return i >= 0 ? tfns[i].may : t_escaped;           /* not defined here: C, the host, a pointer */
}
static void throw_graph_solve(void) {
    for (int changed = 1; changed;) {
        changed = 0;
        for (int i = 0; i < ntfns; i++) {
            ThrowFn *f = &tfns[i];
            if (f->may) continue;
            int may = f->throws || (f->indirect && t_escaped);
            for (int k = 0; k < f->n && !may; k++) may = tcallee_may(tcallee[f->first + k]);
            if (may) { f->may = 1; changed = 1; }
        }
        if (!t_escaped)
            for (int v = 0; v < nvalue_names && !t_escaped; v++) {
                int i = tfn_find(value_names[v]);
                if (i >= 0 && tfns[i].may) { t_escaped = 1; changed = 1; }
            }
    }
}
static int call_may_throw(const char *fname, int is_import) {
    if (!fname || !strcmp(fname, "(function pointer)")) return t_escaped;
    const char *n = demangle(fname);
    if (!strcmp(n, "__c2_throw") || !strcmp(n, "throw") || sandbox_unwinds(n)) return 1;
    int i = tfn_find(n);
    if (i >= 0) return tfns[i].may;
    return is_import ? t_escaped : 1;                  /* a function the prescan didn't see: to be safe */
}

static void prescan_definitions(void) {   /* call with the first token loaded; lexer state is restored */
    LexState saved = save_lex();
    int depth = 0, pdepth = 0, skip_decl = 0, in_init = 0;   /* skip_decl: extern/typedef statement */
    TokenType last_tt = TOKEN_EOF;
    int cur_tfn = -1, try_arg = 0;
    ntfns = ntcallee = nvalue_names = t_escaped = 0;
    if (!tcallee) tcallee = (char (*)[33])(malloc)(16384 * sizeof *tcallee);
    if (!value_names) value_names = (char (*)[32])(malloc)(16384 * sizeof *value_names);
    while (current_token.type != TOKEN_EOF) {
        TokenType tt = current_token.type;
        if (tt == TOKEN_IDENT && last_tt != TOKEN_VOID && ((c2_ext && !strcmp(current_token.text, "throw")) ||
            (sandbox_on && sandbox_unwinds(current_token.text))) &&
            peek_token().type == TOKEN_LPAREN) unwind_on = 1;    /* a throw(...) call: calls check for it */
        if (depth > 0 && cur_tfn >= 0 && c2_ext) {     /* in a function's body: the call graph */
            TokenType nt = peek_token().type;
            ThrowFn *f = &tfns[cur_tfn];
            if (tt == TOKEN_IDENT && nt == TOKEN_LPAREN) {
                const char *c = current_token.text;
                if (!strcmp(c, "throw") || !strcmp(c, "__c2_throw") || sandbox_unwinds(c)) f->throws = 1;
                else if (tcallee && ntcallee < 16384) {
                    snprintf(tcallee[ntcallee++], 33, "%s%.31s", last_tt == TOKEN_DOT || last_tt == TOKEN_ARROW ? "." : "", c);
                    f->n++;
                }
                try_arg = !strcmp(c, "try") ? 1 : 0;
            } else if ((tt == TOKEN_RPAREN || tt == TOKEN_RBRACKET) && nt == TOKEN_LPAREN) {
                f->indirect = 1;                       /* (f)(...), a[i](...): through a pointer */
                try_arg = 0;
            } else if (tt == TOKEN_IDENT && last_tt != TOKEN_DOT && last_tt != TOKEN_ARROW) {
                if (try_arg != 2 && value_names && nvalue_names < 16384)   /* a name used as a value */
                    snprintf(value_names[nvalue_names++], 32, "%.31s", current_token.text);
                try_arg = 0;
            } else if (tt == TOKEN_LPAREN && try_arg == 1) try_arg = 2;   /* try(fn, ...): not an escape */
            else try_arg = 0;
        }
        last_tt = tt;
        if (depth == 0 && pdepth == 0) {
            if (tt == TOKEN_SEMI) { skip_decl = in_init = 0; next_token(); continue; }
            if (tt == TOKEN_EXTERN || tt == TOKEN_TYPEDEF) skip_decl = 1;
            else if (tt == TOKEN_ASSIGN) in_init = 1;
            else if (tt == TOKEN_COMMA) in_init = 0;
        }
        if (tt == TOKEN_LPAREN) pdepth++;
        else if (tt == TOKEN_RPAREN) pdepth--;
        if (current_token.type == TOKEN_LBRACE) depth++;
        else if (current_token.type == TOKEN_RBRACE) { if (--depth == 0 && !in_init) skip_decl = 0; if (!depth) cur_tfn = -1; }
        else if (depth == 0 && pdepth == 0 && current_token.type == TOKEN_IDENT && !skip_decl && !in_init) {
            Token nx = peek_token();
            if ((nx.type == TOKEN_ASSIGN || nx.type == TOKEN_SEMI || nx.type == TOKEN_COMMA || nx.type == TOKEN_LBRACKET)
                && !is_type_start(current_token) && ndefined_vars < 1024)
                snprintf(defined_vars[ndefined_vars++], 32, "%.31s", current_token.text);
        }
        if (depth == 0 && pdepth == 0 && current_token.type == TOKEN_IDENT) {
            char name[32];
            snprintf(name, sizeof name, "%.31s", current_token.text);
            int at_init = in_init;
            next_token();
            if (current_token.type != TOKEN_LPAREN) continue;
            char recv[48] = "";                        /* the first parameter's type, as words */
            int first = 1, words = 0, last_ident = 0;
            size_t last_at = 0;
            for (int pd = 0; current_token.type != TOKEN_EOF; ) {   /* skip the parameter list */
                TokenType pt = current_token.type;
                if (pt == TOKEN_LPAREN) pd++;
                else if (pt == TOKEN_RPAREN && --pd == 0) break;
                else if (pd == 1 && pt == TOKEN_COMMA) first = 0;
                else if (pd == 1 && first) {           /* type words (keywords keep no text: spelled here) */
                    const char *w = pt == TOKEN_IDENT ? current_token.text : pt == TOKEN_INT ? "int" :
                        pt == TOKEN_CHAR ? "char" : pt == TOKEN_SHORT ? "short" : pt == TOKEN_LONG ? "long" :
                        pt == TOKEN_UNSIGNED ? "unsigned" : pt == TOKEN_SIGNED ? "signed" : pt == TOKEN_FLOAT ? "float" :
                        pt == TOKEN_DOUBLE ? "double" : pt == TOKEN_VOID ? "void" : pt == TOKEN_BOOL ? "bool" :
                        pt == TOKEN_ELLIPSIS ? "..." : NULL;
                    if (w) {
                        size_t len = strlen(recv);
                        last_at = len;
                        fmt_to(recv + len, sizeof recv - len, "%s%s", len ? " " : "", w);
                        words++;
                        last_ident = pt == TOKEN_IDENT;
                    }
                }
                next_token();
            }
            if (words >= 2 && last_ident) recv[last_at] = '\0';   /* drop the parameter's name */
            if (!at_init && recv[0] && strcmp(recv, "void") && strcmp(recv, "...")) ovl_note(name, recv);
            next_token();
            if (current_token.type == TOKEN_LBRACE && ndefined < 1024) strcpy(defined_names[ndefined++], name);
            if (current_token.type == TOKEN_LBRACE && ntfns < 2048) {   /* its body follows */
                cur_tfn = ntfns++;
                memset(&tfns[cur_tfn], 0, sizeof tfns[cur_tfn]);
                snprintf(tfns[cur_tfn].name, 32, "%s", name);
                tfns[cur_tfn].first = ntcallee;
            }
            continue;
        }
        next_token();
    }
    restore_lex(saved);
    if (unwind_on) throw_graph_solve();
}

/* declaration of a function (prototype, definition header, or local prototype) */
static int same_text(const char *a, const char *b) {   /* equal but for spaces */
    for (;;) {
        while (isspace((unsigned char)*a)) a++;
        while (isspace((unsigned char)*b)) b++;
        if (*a != *b) return 0;
        if (!*a) return 1;
        a++, b++;
    }
}

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
        f->is_import = !is_defined_in_file(name) && !is_defined_in_file(demangle(name));   /* (a module's private __mN_f) */
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
        if (c2_ext && !f->unprototyped && !f->defined)   /* named arguments use the latest names */
            for (int i = 0; i < g->np; i++)
                if (f->pn[i][0] && g->pn[i][0] && strcmp(f->pn[i], g->pn[i]))
                    diag("%s:%d: warning: parameter %d of '%s' is named '%s' here but '%s' before; named arguments use '%s'\n",
                         cur_file, cur_line, i + 1, name, g->pn[i], f->pn[i], g->pn[i]);
        for (int i = 0; f->pdef && g->pdef && i < g->np; i++)   /* a default may be repeated, not changed */
            if (f->pdef[i] && g->pdef[i] && !same_text(f->pdef[i], g->pdef[i]))
                error("default of parameter %d of '%s' changed: '%s' before, '%s' now", i + 1, name, f->pdef[i], g->pdef[i]);
        f->unprototyped = 0;
        f->np = g->np;
        f->vararg = g->vararg;
        memcpy(f->pt, g->pt, sizeof f->pt);
        memcpy(f->pn, g->pn, sizeof f->pn);   /* the latest declaration's names (the definition's) win */
        if (g->pdef || f->pdef) {              /* defaults: given by any declaration, merged */
            char **pd = cc_calloc(MAX_PARAMS, sizeof *pd);
            for (int i = 0; i < g->np; i++) pd[i] = g->pdef && g->pdef[i] ? g->pdef[i] : f->pdef ? f->pdef[i] : NULL;
            f->pdef = pd;
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
    f->in_module = cur_module() + 1;
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
    pure_on = f->nodiscard == 2;                       /* [[pure]]: checked while its body compiles */
    npure_regs = npure_warned = 0;
    nvars = 0;
    nglabels = 0;
    scope_depth = 1;
    loop_depth = 0;
    sw_depth = 0;
    ndefers = in_defer = fn_used_defer = fn_used_goto = 0;
    dbg_slot = -1;                                     /* (the parameters join the frame once it exists) */
    MemT rq[2];
    cur_sret_reg = is_struct_val(f->ret) && abi_ret_regs(f->ret, rq) == 0 ? new_reg("_ret", RK_I, 1) : -1;
    for (int i = 0; i < f->np; i++) {
        int preg = new_reg(f->pn[i], reg_kind(f->pt[i]), 1);
        tail_param_reg[i] = preg;
        tail_param_var[i] = -1;
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
        } else if (is_addr_taken(user_pn[i]) || dbg_keep_vars()) {   /* spill an address-taken parameter (or -g) */
            v = declare_var(user_pn[i], f->pt[i]);
            tail_param_var[i] = (int)(v - vars);
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
    dbg_slot = -1;
    nfresh = 0;
    emit_hook("__on_enter");
    tail_label = new_label();                          /* self tail calls jump here */
    place_label(tail_label);
    expect(TOKEN_LBRACE);
    while (current_token.type != TOKEN_RBRACE && current_token.type != TOKEN_EOF) { stmt_direct = 1; parse_statement(); }
    expect(TOKEN_RBRACE);
    for (int i = 0; i < nglabels; i++)
        if (!glabels[i].defined) { cur_line = glabels[i].line; error("label '%s' used but not defined", glabels[i].name); }
    emit_return(NULL);                                 /* falling off the end (runs the body's defers) */
    ndefers = 0;
    tail_label = -1;
    be_func_end();
    hide_structs(1);                                   /* struct tags declared in the body */
    while (nfunc_aliases > 0 && func_aliases[nfunc_aliases - 1].depth >= 1) nfunc_aliases--;
    scope_depth = 0;
    cur_func = NULL;
    pure_on = 0;
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
    int nodiscard = current_token.a_nodiscard;         /* [[...]] before the declaration */
    const char *deprecated = current_token.a_deprecated;
    decl_exported = current_token.a_export;
    int gskip = current_token.a_skip;                   /* [[skip]] int secret: not in reflect_globals() */
    int persist = current_token.a_persist;
    int oper = current_token.a_operator, dtor_attr = current_token.a_destructor, copy_attr = current_token.a_copy;
    int noncomm = current_token.a_noncomm;
    char oper_text[8];
    snprintf(oper_text, sizeof oper_text, "%s", attr_operator_text);
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
    int cx = spec_constexpr, ext = spec_extern;
    for (;;) {
        char name[32];
        Type t = parse_declarator(base, name, 0);
        spec_constexpr = cx;
        spec_extern = ext;                             /* (a parameter list in the declarator resets it: extern int (*fp)(int);) */
        int may_be_private = is_func(t) ? is_defined_in_file(name) : !spec_extern;   /* not C's functions */
        char set_name[40] = "";
        if (is_func(t) && c2_ext && is_overloaded(name) && host_typed(name) && strcmp(cur_file, "<built-in>"))
            error("'%s' is declared by the host (C2_add_function): a script can't declare it with other parameters", name);
        if (is_func(t) && c2_ext && is_overloaded(name)) {
            int exp = decl_exported || current_token.a_export, m = cur_module();
            if (may_be_private && m >= 0 && scope_depth == 0) {   /* a module's overloads: all exported or none */
                char other[48];
                snprintf(other, sizeof other, "__m%d_%s", m, name);
                OvlSet *o = exp ? ovl_set(other, 0) : ovl_set(name, 0);
                if (o && o->n) error("overloads of '%s' must all be [[export]]ed, or none (a module's name is public or private as a whole)", name);
            }
            if (may_be_private) module_decl_name(name, sizeof name, exp);
            overload_symbol(t, name, sizeof name, set_name, sizeof set_name);
        } else if (!find_func(name) && !find_global(name)) {
            if (may_be_private) module_decl_name(name, sizeof name, decl_exported || current_token.a_export);
        } else if (decl_exported) module_decl_name(name, sizeof name, 1);
        if (constexpr_decl(t, name)) {
        } else if (is_func(t)) {
            if (persist) error("[[persist]] is for globals, not functions ('%s')", name);
            if (sigs[func_sig(t)].knr) knr_param_decls(&sigs[func_sig(t)]);
            Func *f = declare_function(t, name);
            if (set_name[0]) overload_join(set_name, f);
            if (current_token.a_destructor || dtor_attr) register_destructor(f);
            if (current_token.a_copy || copy_attr) register_copy(f);
            if (set_name[0] && !strcmp(name, set_name) && current_token.type != TOKEN_LBRACE && !f->defined)
                f->is_import = 1;                      /* C's abs(int) next to a .c2 abs(vec2): from the C library */
            if (oper) register_operator(f, oper, oper_text);
            if ((noncomm || current_token.a_noncomm) && oper) oper_noncomm_set(f);
            if (nodiscard) f->nodiscard = nodiscard;
            if (deprecated && !f->deprecated) f->deprecated = str_dup(deprecated);
            if (current_token.type == TOKEN_LBRACE) { define_function(f); flush_nested(); return; }
        } else {
            if (oper) error("[[operator%s]] is for functions, not variables ('%s')", oper_text, name);
            if (is_void(t)) error("variable '%s' declared void", name);
            if (find_func(name)) error("'%s' redeclared as a variable", name);
            int has_init = current_token.type == TOKEN_ASSIGN;
            Global *g = find_global(name);
            if (is_array(t) && arr_len(t) < 0 && !has_init) {   /* int a[]; extern int a[]; — the size comes later */
                if (g && is_array(g->ty) && same_type(elem_of(g->ty), elem_of(t))) {
                    if (persist) g->persist = 1;
                    if (current_token.type != TOKEN_COMMA) break;
                    next_token();
                    continue;
                }
                if (!g && !(spec_extern && !is_var_defined_in_file(name))) {
                    g = add_global(name, t);
                    g->deferred = 1;
                    if (persist) g->persist = 1;
                    if (current_token.type != TOKEN_COMMA) break;
                    next_token();
                    continue;
                }
            }
            if (is_array(t) && arr_len(t) < 0 && has_init) t = set_arr_len(t, count_initializer(elem_of(t)));
            if (g && g->deferred && is_array(g->ty) && arr_len(g->ty) < 0) {   /* the tentative array gets its size */
                if (!is_array(t) || !same_type(elem_of(g->ty), elem_of(t))) error("conflicting types for '%s'", name);
                g->ty = t;
                g->deferred = 0;
                define_global_storage(g, 0);
                if (has_init) g->has_init = 1;
                if (persist) g->persist = 1;
                if (current_token.type != TOKEN_COMMA) break;
                next_token();
                continue;
            }
            if (is_struct_val(t) && !structs[t.sidx].complete && !(spec_extern && !is_var_defined_in_file(name))) {
                if (has_init) error("variable '%s' has incomplete type", name);
                if (!g) { g = add_global(name, t); g->deferred = 1; }   /* tentative: wait for the struct */
                if (persist) g->persist = 1;
                if (current_token.type != TOKEN_COMMA) break;
                next_token();
                continue;
            }
            if (!g && spec_extern && !has_init && !is_var_defined_in_file(name)) {
                if (persist) error("[[persist]] '%s': only the script's own globals can persist, not extern ones", name);
                g = add_global(name, t);               /* defined by the host or a library: import */
                g->has_init = 1;
                g->imported = 1;
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
                g = add_global(name, t);
                define_global_storage(g, 0);
            }
            if (persist && g) g->persist = 1;
            if (gskip && g) g->skip = 1;
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
"static int64_t c2_w_lrintf(float x) { return (int64_t)llrintf(x); }\n"
"static int64_t c2_w_lroundf(float x) { return (int64_t)llroundf(x); }\n"
"static float c2_w_scalblnf(float x, int64_t n) { return scalbnf(x, n > 2147483647 ? 2147483647 : n < -2147483647 ? -2147483647 : (int)n); }\n"
"static float c2_w_nexttowardf(float x, double y) { return nextafterf(x, (float)y); }\n"
"static int64_t c2_w_lround(double x) { return (int64_t)llround(x); }\n"
"static double c2_w_nexttoward(double x, double y) { return nextafter(x, y); }\n"
"static int c2_w___c2_signbit(double x) { return signbit(x) != 0; }\n"
"static int c2_w___c2_mb_cur_max(void) { return (int)MB_CUR_MAX; }\n"
C2_STR(C2_ARITH_SRC(c2_w___c2_arith_overflow)) "\n"
"#if defined(__has_include)\n"
"#if __has_include(\"minicoro.h\")\n"
"#define C2_HAVE_MINICORO\n"
"#endif\n"
"#endif\n"
"static void *c2_w___c2_dbg_top;\n"
"#ifdef C2_HAVE_MINICORO\n"                         /* coroutines: the same glue as c2 has (runtime/coro.c2) */
"static void *c2_w___c2_resume_from[64];\n"
"static int c2_w___c2_resume_depth;\n"
"#define MINICORO_IMPL\n"

"#include \"minicoro.h\"\n"
C2_STR(C2_CORO_SRC(c2_w___c2_)) "\n"
"#else\n"
"static void c2_no_coro(void) { fputs(\"coroutines need minicoro.h on the include path (c2's 3rd/)\\n\", stderr); abort(); }\n"
"static void *c2_w___c2_co_create(void (*f)(void *), void *a, size_t s) { (void)f; (void)a; (void)s; c2_no_coro(); return 0; }\n"
"static int c2_w___c2_co_resume(void *h) { (void)h; c2_no_coro(); return 0; }\n"
"static void c2_w___c2_co_yield(void) { c2_no_coro(); }\n"
"static int c2_w___c2_co_status(void *h) { (void)h; return 0; }\n"
"static void *c2_w___c2_co_running(void) { return 0; }\n"
"static void c2_w___c2_co_destroy(void *h) { (void)h; }\n"
"#endif\n"
C2_STR(C2_TRY_SRC(c2_w___c2_)) "\n"
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
"        {\"sinhf\", (void *)sinhf},\n"
"        {\"coshf\", (void *)coshf},\n"
"        {\"tanhf\", (void *)tanhf},\n"
"        {\"asinhf\", (void *)asinhf},\n"
"        {\"acoshf\", (void *)acoshf},\n"
"        {\"atanhf\", (void *)atanhf},\n"
"        {\"expm1f\", (void *)expm1f},\n"
"        {\"log1pf\", (void *)log1pf},\n"
"        {\"logbf\", (void *)logbf},\n"
"        {\"erff\", (void *)erff},\n"
"        {\"erfcf\", (void *)erfcf},\n"
"        {\"lgammaf\", (void *)lgammaf},\n"
"        {\"tgammaf\", (void *)tgammaf},\n"
"        {\"nearbyintf\", (void *)nearbyintf},\n"
"        {\"rintf\", (void *)rintf},\n"
"        {\"remainderf\", (void *)remainderf},\n"
"        {\"nextafterf\", (void *)nextafterf},\n"
"        {\"fdimf\", (void *)fdimf},\n"
"        {\"ilogbf\", (void *)ilogbf},\n"
"        {\"frexpf\", (void *)frexpf},\n"
"        {\"ldexpf\", (void *)ldexpf},\n"
"        {\"modff\", (void *)modff},\n"
"        {\"scalbnf\", (void *)scalbnf},\n"
"        {\"llrintf\", (void *)llrintf},\n"
"        {\"llroundf\", (void *)llroundf},\n"
"        {\"remquof\", (void *)remquof},\n"
"        {\"fmaf\", (void *)fmaf},\n"
"        {\"lrintf\", (void *)c2_w_lrintf},\n"
"        {\"lroundf\", (void *)c2_w_lroundf},\n"
"        {\"scalblnf\", (void *)c2_w_scalblnf},\n"
"        {\"nexttowardf\", (void *)c2_w_nexttowardf},\n"
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
"        {\"__c2_co_create\", (void *)c2_w___c2_co_create},\n"
"        {\"__c2_co_resume\", (void *)c2_w___c2_co_resume},\n"
"        {\"__c2_co_yield\", (void *)c2_w___c2_co_yield},\n"
"        {\"__c2_co_status\", (void *)c2_w___c2_co_status},\n"
"        {\"__c2_co_running\", (void *)c2_w___c2_co_running},\n"
"        {\"__c2_co_destroy\", (void *)c2_w___c2_co_destroy},\n"
"        {\"__c2_try\", (void *)c2_w___c2_try},\n"
"        {\"__c2_throw\", (void *)c2_w___c2_throw},\n"
"        {\"__c2_throwing\", (void *)&c2_w___c2_throwing},\n"
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
"        {\"__c2_arith_overflow\", (void *)c2_w___c2_arith_overflow},\n"
"    };\n"
"    for (size_t i = 0; i < sizeof tab / sizeof tab[0]; i++) if (!strcmp(tab[i].n, name)) return tab[i].a;\n"
"    fprintf(stderr, \"c2 runtime: unknown function '%s'\\n\", name);\n"
"    abort();\n"
"}\n";
static const char *c_wrapped[] = {"__c2_assert_fail", "__c2_mb_cur_max", "__c2_arith_overflow", "__c2_clocks_per_sec",
    "__c2_co_create", "__c2_co_resume", "__c2_co_yield", "__c2_co_status", "__c2_co_running", "__c2_co_destroy", "__c2_try", "__c2_throw", "__c2_throwing", "__c2_errno", "__c2_huge_val", "__c2_huge_valf", "__c2_memset", "__c2_nanf", "__c2_signbit", "__c2_stderr", "__c2_stdin", "__c2_stdout", "atol", "clock", "dlclose", "dlerror", "dlopen", "dlsym", "fseek", "ftell", "getc", "imaxabs", "labs", "ldiv", "lrint", "lrintf", "lround", "lroundf", "nexttoward", "nexttowardf", "putc", "scalbln", "scalblnf", "strdup", "strtoimax", "strtol", "strtold", "strtoul", "strtoumax", "time", "timespec_get"};
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
    SrcLoc *last_loc = NULL;
    for (MIR_insn_t in = DLIST_HEAD(MIR_insn_t, fn->insns); in; in = DLIST_NEXT(MIR_insn_t, in)) {
        MIR_op_t *o = in->ops;
        MIR_insn_code_t c = in->code;
        const char *cast, *op = c_binop(c, &cast);
        if (c == MIR_LABEL) { buf_emit(b, "L%d:;\n", (int)(intptr_t)in->data); continue; }
        SrcLoc *loc = c_lines_on ? (SrcLoc *)in->data : NULL;
        if (loc && loc != last_loc && (!last_loc || loc->line != last_loc->line || loc->file != last_loc->file)) {
            buf_emit(b, "#line %d \"", loc->line);           /* debuggers show the .c2 source */
            for (const char *p = loc->file; *p; p++) buf_emit(b, *p == '\\' || *p == '"' ? "\\%c" : "%c", *p);
            buf_emit(b, "\"\n");
            last_loc = loc;
        }
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
        case MIR_LADDR: c_val(b, fn, &o[0]); buf_emit(b, " = (int64_t)(intptr_t)&&"); c_label(b, &o[1]); break;   /* GNU C */
        case MIR_JMPI: buf_emit(b, "goto *(void *)(intptr_t)"); c_val(b, fn, &o[0]); break;
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
                } else if (ai >= np && o[i].mode == MIR_OP_MEM && MIR_blk_type_p(o[i].u.mem.type)) {   /* a struct to `...` */
                    buf_emit(b, "*(%s *)(intptr_t)", c_blk(o[i].u.mem.type, (size_t)o[i].u.mem.disp));   /* (disp: the size) */
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
    buf_emit(b, "}\n");
    if (last_loc) buf_emit(b, "#line 1 \"(c2 -C output)\"\n");   /* what follows is generated, not from the .c2 */
    buf_emit(b, "\n");
}

static void c_print_runtime(FILE *f) { fputs(c_runtime, f); fputs(c_runtime_table, f); }

/* c2 -C -prefix ui_: every function and global the output defines is renamed ui_name (by #defines at its
   top), c2_init is ui_init and the script's main is ui_main, with no C main(): several scripts, each with
   its own copy of what it uses from the runtime, link into one program without clashing */
static char c_prefix[32];

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
    if (c_is_main_renamed && !c_prefix[0]) {
        Func *mf = find_func("main");
        int np = mf && mf->np >= 2;
        buf_emit(&c_out, "\nint main(int argc, char **argv) {\n    c2_init();\n    return (int)__c2_main(%s);\n}\n",
                 np ? "argc, (int64_t)(intptr_t)argv" : "");
        if (!np) buf_emit(&c_out, "/* (argc, argv unused) */\n");
    }
    if (c_prefix[0]) {                                 /* the renames, before any use */
        fprintf(f, "/* -prefix %s: the names this file defines, renamed */\n", c_prefix);
        fprintf(f, "#define c2_init %sinit\n", c_prefix);
        for (MIR_item_t it = DLIST_HEAD(MIR_item_t, m->items); it; it = DLIST_NEXT(MIR_item_t, it)) {
            if (it->item_type != MIR_func_item && it->item_type != MIR_data_item && it->item_type != MIR_bss_item &&
                it->item_type != MIR_ref_data_item) continue;
            const char *n = MIR_item_name(ctx, it);
            if (it->item_type == MIR_func_item && !strcmp(n, "main")) fprintf(f, "#define __c2_main %smain\n", c_prefix);
            else fprintf(f, "#define %s %s%s\n", n, c_prefix, n);
        }
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
static int64_t host_lrintf(float x) { return (int64_t)llrintf(x); }   /* (long: 64-bit in C², not in MSVC) */
static int64_t host_lroundf(float x) { return (int64_t)llroundf(x); }
static float host_scalblnf(float x, int64_t n) { return scalbnf(x, n > INT_MAX ? INT_MAX : n < INT_MIN ? INT_MIN : (int)n); }
static float host_nexttowardf(float x, double y) { return nextafterf(x, (float)y); }
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
        W(__c2_dbg_top, &host_dbg_top),
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
#ifndef C2_NO_CORO
        W(__c2_co_create, host_co_create), W(__c2_co_resume, host_co_resume), W(__c2_co_yield, host_co_yield),
        W(__c2_co_status, host_co_status), W(__c2_co_running, host_co_running), W(__c2_co_destroy, host_co_destroy),
#endif
        W(__c2_try, host_try), W(__c2_throw, host_throw), W(__c2_throwing, &host_throwing),
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
        F(sinhf), F(coshf), F(tanhf), F(asinhf), F(acoshf), F(atanhf), F(expm1f), F(log1pf), F(logbf), F(erff), F(erfcf), F(lgammaf), F(tgammaf), F(nearbyintf), F(rintf), F(remainderf), F(nextafterf), F(fdimf), F(ilogbf), F(frexpf), F(ldexpf), F(modff), F(scalbnf), F(llrintf), F(llroundf), F(remquof), F(fmaf),
        W(lrintf, host_lrintf), W(lroundf, host_lroundf), W(scalblnf, host_scalblnf), W(nexttowardf, host_nexttowardf),
        F(isblank), F(iscntrl), F(isgraph), F(isprint),
        F(difftime), F(mktime), F(asctime), F(ctime), F(gmtime), F(localtime), F(strftime),
        W(timespec_get, host_timespec_get), W(__c2_errno, host_errno), F(signal), F(raise), F(setlocale),
        W(imaxabs, llabs), W(strtoimax, strtoll), W(strtoumax, host_strtoul),
        F(mblen), F(mbtowc), F(wctomb), F(mbstowcs), F(wcstombs), W(__c2_mb_cur_max, host_mb_cur_max),
        W(__c2_arith_overflow, host_arith_overflow),
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
    int debug;                  /* C2_set_debug: keep a call stack for C2_backtrace */
    int opt_level;              /* MIR generator: 0..3 (default 2) */
    int lazy;                   /* generate each function's code on its first call */
    int sandbox;                /* C2_sandbox: no deleting files, starting programs, loading libraries */
    int safe_reload;            /* C2_safe_reload: functions count themselves in and out; no reload while one runs */
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
    /* hot reload: the loaded program's globals (to carry [[persist]] ones into the next program), and
       the previous program while the host still needs it (C2_keep_previous) */
    struct C2Global { char *key, *item, *type; size_t size; int fnptr; } *globals;
    int nglobals;
    int keep_prev;
    char *decls;                /* C2_declare / C2_add_function / C2_add_variable: seen by every compile */
    char **typed;               /* the names C2_add_function declared (a script can't overload them) */
    int ntyped;
    size_t decls_len;
#ifndef NO_MIR
    MIR_context_t prev_ctx;
#endif
    int prev_gen_live;
};

static void free_globals(struct C2Global *g, int n) {
    for (int i = 0; i < n; i++) { free(g[i].key); free(g[i].item); free(g[i].type); }
    free(g);
}

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

/* declarations every later compile sees, before the script (types for C2_add_function, say) */
void C2_declare(C2 *cc, const char *text) {
    size_t n = strlen(text);
    char *p = (char *)realloc(cc->decls, cc->decls_len + n + 2);
    if (!p) return;
    memcpy(p + cc->decls_len, text, n);
    p[cc->decls_len + n] = '\n';
    p[cc->decls_len + n + 1] = '\0';
    cc->decls = p;
    cc->decls_len += n + 1;
}

static int is_ident_char(char c) { return isalnum((unsigned char)c) || c == '_'; }

/* the name a declaration declares: for a function the identifier before its parameter list, for a
   variable the last identifier before '[', '=', ';' or a ')' (int (*fp)(void)); "" if none */
static void decl_name(const char *d, int func, char *name, size_t size) {
    const char *end = NULL;
    for (const char *p = d; *p && !end; p++) {
        if (func ? *p == '(' : (*p == '[' || *p == '=' || *p == ';' || *p == ')')) {
            const char *q = p;
            while (q > d && isspace((unsigned char)q[-1])) q--;
            if (q > d && is_ident_char(q[-1])) end = q;
            else if (func) break;                       /* (*f)(...) or a cast: not a plain function */
        }
    }
    if (!end && !func) {                               /* "int score" without ';' */
        end = d + strlen(d);
        while (end > d && isspace((unsigned char)end[-1])) end--;
        if (end == d || !is_ident_char(end[-1])) end = NULL;
    }
    name[0] = '\0';
    if (!end) return;
    const char *b = end;
    while (b > d && is_ident_char(b[-1])) b--;
    if (isdigit((unsigned char)*b)) return;
    snprintf(name, size, "%.*s", (int)(end - b), b);
}

static int host_typed_in(C2 *cc, const char *name);
static const char *add_typed(C2 *cc, const char *decl, void *addr, int func) {
    static char msg[300];
    char name[128];
    decl_name(decl, func, name, sizeof name);
    if (!name[0] || !strcmp(name, "extern") || !strcmp(name, "void") || !strcmp(name, "int")) {
        snprintf(msg, sizeof msg, "%s: can't find the %s's name in '%.120s'", func ? "C2_add_function" : "C2_add_variable",
                 func ? "function" : "variable", decl);
        return msg;
    }
    if (!addr) { snprintf(msg, sizeof msg, "%s: '%s' has a NULL address", func ? "C2_add_function" : "C2_add_variable", name); return msg; }
    const char *p = decl;
    while (isspace((unsigned char)*p)) p++;
    size_t n = strlen(decl);
    while (n && isspace((unsigned char)decl[n - 1])) n--;
    char *text = (char *)malloc(n + 16);
    if (!text) return "out of memory";
    snprintf(text, n + 16, "%s%.*s%s", !func && strncmp(p, "extern", 6) ? "extern " : "", (int)(n - (p - decl)), p,
             n && decl[n - 1] == ';' ? "" : ";");
    C2_declare(cc, text);
    free(text);
    C2_add_symbol(cc, name, addr);
    if (func && !host_typed_in(cc, name)) {
        char **t = (char **)realloc(cc->typed, (cc->ntyped + 1) * sizeof *t);
        if (!t) return "out of memory";
        cc->typed = t;
        cc->typed[cc->ntyped++] = str_dup(name);
    }
    return "";
}

static int host_typed_in(C2 *cc, const char *name) {
    for (int i = 0; cc && i < cc->ntyped; i++) if (!strcmp(cc->typed[i], name)) return 1;
    return 0;
}
static int host_typed(const char *name) { return host_typed_in(compiling, name); }

/* a host function or variable with its declaration, so scripts needn't (and can't wrongly) repeat it:
   C2_add_function(c2, "Vec3 vec3_add(Vec3 a, Vec3 b);", (void *)vec3_add).  "" or an error message. */
const char *C2_add_function(C2 *cc, const char *declaration, void *addr) { return add_typed(cc, declaration, addr, 1); }
const char *C2_add_variable(C2 *cc, const char *declaration, void *addr) { return add_typed(cc, declaration, addr, 0); }

const char *C2_errors(C2 *cc) { return cc->errors ? cc->errors : ""; }

void C2_use_interpreter(C2 *cc, int on) { cc->interp = on; }
void C2_set_opt_level(C2 *cc, int level) { cc->opt_level = level < 0 ? 0 : level > 3 ? 3 : level; }
void C2_set_lazy(C2 *cc, int on) { cc->lazy = on; }
void C2_sandbox(C2 *cc, int on) { cc->sandbox = on; }
void C2_safe_reload(C2 *cc, int on) { cc->safe_reload = on; }
void C2_set_debug(C2 *cc, int on) { cc->debug = on; }

typedef struct { const char *name; int64_t kind; const void *at; } C2FrameVar;   /* kind: category << 8 | size */
typedef struct C2Frame { struct C2Frame *prev; const char *func, *file; int64_t line, live; C2FrameVar var[]; } C2Frame;

static int frame_value(char *out, size_t n, const C2FrameVar *v) {   /* "name = value" */
    int cat = (int)(v->kind >> 8 & 0xff), size = (int)(v->kind & 0xff);
    const unsigned char *p = (const unsigned char *)v->at;
    int64_t i = 0;
    uint64_t u = 0;
    if ((cat == 1 || cat == 2 || cat == 5 || cat == 6) && size >= 1 && size <= 8) {
        memcpy(&u, p, (size_t)size);
        if (cat == 1 || cat == 6) { int sh = 64 - 8 * size; i = (int64_t)(u << sh) >> sh; } else i = (int64_t)u;
    }
    switch (cat) {
    case 1: return snprintf(out, n, "%s = %lld", v->name, (long long)i);
    case 2: return snprintf(out, n, "%s = %llu", v->name, (unsigned long long)u);
    case 5: return snprintf(out, n, "%s = %s", v->name, u ? "true" : "false");
    case 6: return i >= 32 && i < 127 ? snprintf(out, n, "%s = '%c'", v->name, (int)i) : snprintf(out, n, "%s = %lld", v->name, (long long)i);
    case 3: {
        double d;
        if (size == 4) { float f; memcpy(&f, p, 4); d = f; } else memcpy(&d, p, 8);
        return snprintf(out, n, "%s = %g", v->name, d);
    }
    case 4: {
        void *q;
        memcpy(&q, p, sizeof q);
        return q ? snprintf(out, n, "%s = %p", v->name, q) : snprintf(out, n, "%s = NULL", v->name);
    }
    case 7: return snprintf(out, n, "%s = {...}", v->name);
    default: return snprintf(out, n, "%s = [...]", v->name);
    }
}

/* the script call stack, innermost first, one frame per line: "  update() at game.c2:42" */
const char *C2_backtrace(void) {
    static char out[16384];
    size_t n = 0;
    out[0] = '\0';
    int depth = 0, co = host_resume_depth < 64 ? host_resume_depth : 64;
    for (C2Frame *f = (C2Frame *)host_dbg_top; n < sizeof out - 1; f = f->prev, depth++) {
        const char *note = NULL;
        while (!f && co > 0) { f = (C2Frame *)host_resume_from[--co]; note = "  (a coroutine, resumed from:)\n"; }
        if (!f) break;
        if (depth == 64) { snprintf(out + n, sizeof out - n, "  ...\n"); break; }
        char args[512] = "", locals[512] = "";
        size_t na = 0, nl = 0;
        for (int v = 0; v < f->live && v < 24; v++) {   /* (-g frames: their variables, as they are now) */
            char one[160];
            frame_value(one, sizeof one, &f->var[v]);
            int param = (int)(f->var[v].kind >> 16 & 1);
            char *b = param ? args : locals;
            size_t *bn = param ? &na : &nl;
            int k = snprintf(b + *bn, sizeof args - *bn, "%s%s", *bn ? ", " : "", one);
            if (k > 0) *bn += (size_t)k < sizeof args - *bn ? (size_t)k : sizeof args - *bn - 1;
        }
        int k = snprintf(out + n, sizeof out - n, "%s  %s(%s) at %s:%lld\n%s%s%s", note ? note : "", f->func, args, f->file,
                         (long long)f->line, nl ? "      " : "", locals, nl ? "\n" : "");
        if (k < 0) break;
        n += (size_t)k < sizeof out - n ? (size_t)k : sizeof out - n - 1;
    }
    return out;
}

/* C2_crash_handler(1): on a crash (SIGSEGV, SIGFPE, SIGILL, SIGBUS, SIGABRT), print the script call stack
   to stderr, then crash as before.  Needs scripts compiled with C2_set_debug. */
static const char *crash_name(int sig) {
    return sig == SIGSEGV ? "SIGSEGV, a bad memory access" : sig == SIGFPE ? "SIGFPE, an arithmetic error" :
           sig == SIGILL ? "SIGILL" : sig == SIGABRT ? "SIGABRT, abort()" :
#ifdef SIGBUS
           sig == SIGBUS ? "SIGBUS" :
#endif
           "a signal";
}
static void crash_print(const char *what) {
    static int reported;                               /* once (Windows may see it twice) */
    if (reported++) return;
    fprintf(stderr, "script crashed (%s); call stack, innermost first:\n%s", what,
            host_dbg_top ? C2_backtrace() : "  (none: no script running, or not compiled with -g / C2_set_debug)\n");
    fflush(stderr);
}
static void crash_report(int sig) {
    signal(sig, SIG_DFL);
    crash_print(crash_name(sig));
    raise(sig);
}
#ifdef _WIN32
/* Windows: a hardware exception in JIT code never reaches the C runtime's SIGSEGV (finding the handler
   means walking frames that have no unwind data), so a vectored handler, which Windows calls first,
   reports it, and lets it go on as before.  Only while a script runs, and only for fatal ones. */
static long __stdcall crash_veh(c2_exc_pointers *e) {
    unsigned long c = e->record->code;
    const char *what = c == 0xC0000005u ? crash_name(SIGSEGV) : c == 0xC00000FDu ? "a stack overflow" :
                       c == 0xC0000094u || c == 0xC0000095u ? crash_name(SIGFPE) :
                       c == 0xC000001Du || c == 0xC0000096u ? crash_name(SIGILL) : NULL;
    if (what && host_dbg_top) crash_print(what);
    return 0;                                          /* EXCEPTION_CONTINUE_SEARCH */
}
#endif
void C2_crash_handler(int on) {
    static const int sigs[] = {SIGSEGV, SIGFPE, SIGILL, SIGABRT,
#ifdef SIGBUS
        SIGBUS,
#endif
    };
#if defined(__unix__) || defined(__APPLE__)
    static char *alt;                                  /* a stack overflow needs another stack to report on */
    if (on && !alt && (alt = (char *)malloc(65536))) {
        stack_t ss;
        memset(&ss, 0, sizeof ss);
        ss.ss_sp = alt;
        ss.ss_size = 65536;
        sigaltstack(&ss, NULL);
    }
    for (size_t i = 0; i < sizeof sigs / sizeof sigs[0]; i++) {
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_handler = on ? crash_report : SIG_DFL;
        sa.sa_flags = SA_ONSTACK;
        sigaction(sigs[i], &sa, NULL);
    }
#else
    for (size_t i = 0; i < sizeof sigs / sizeof sigs[0]; i++) signal(sigs[i], on ? crash_report : SIG_DFL);
#endif
#ifdef _WIN32
    static void *veh;
    if (on && !veh) veh = AddVectoredExceptionHandler(1, crash_veh);
    if (!on && veh) { RemoveVectoredExceptionHandler(veh); veh = NULL; }
#endif
}

#ifndef NO_MIR
static void release_program(C2 *cc) {   /* the loaded program's context (and generator, if lazy) */
    if (!cc->ctx) return;
    if (cc->gen_live) MIR_gen_finish(cc->ctx);
    MIR_finish(cc->ctx);
    cc->ctx = NULL;
    cc->gen_live = 0;
}

static void release_previous(C2 *cc) {
    if (!cc->prev_ctx) return;
    if (cc->prev_gen_live) MIR_gen_finish(cc->prev_ctx);
    MIR_finish(cc->prev_ctx);
    cc->prev_ctx = NULL;
    cc->prev_gen_live = 0;
}

/* a function's or global's address in a loaded program; *is_func tells which */
static void *ctx_get(MIR_context_t c, const char *name, int *is_func) {
    if (!c) return NULL;
    for (MIR_module_t m = DLIST_HEAD(MIR_module_t, *MIR_get_module_list(c)); m; m = DLIST_NEXT(MIR_module_t, m))
        for (MIR_item_t it = DLIST_HEAD(MIR_item_t, m->items); it; it = DLIST_NEXT(MIR_item_t, it)) {
            if (it->item_type != MIR_func_item && it->item_type != MIR_data_item && it->item_type != MIR_bss_item
                && it->item_type != MIR_ref_data_item && it->item_type != MIR_expr_data_item) continue;
            if (strcmp(MIR_item_name(c, it), name) == 0) {
                if (is_func) *is_func = it->item_type == MIR_func_item;
                return it->addr;                       /* (a bss item has no u.data: an uninitialized global) */
            }
        }
    return NULL;
}

static const char *ctx_func_at(MIR_context_t c, void *addr) {   /* the name of the function at addr */
    for (MIR_module_t m = DLIST_HEAD(MIR_module_t, *MIR_get_module_list(c)); m; m = DLIST_NEXT(MIR_module_t, m))
        for (MIR_item_t it = DLIST_HEAD(MIR_item_t, m->items); it; it = DLIST_NEXT(MIR_item_t, it))
            if (it->item_type == MIR_func_item && it->addr == addr) return MIR_item_name(c, it);
    return NULL;
}

/* the new program's globals, for the reload after it */
static struct C2Global *record_globals(int *n) {
    struct C2Global *r = (struct C2Global *)calloc(nglobals + 1, sizeof *r);
    *n = 0;
    for (int i = 0; r && i < nglobals; i++) {
        Global *g = &globals[i];
        if (g->imported || g->deferred) continue;
        struct C2Global *e = &r[(*n)++];
        e->key = str_dup(demangle(g->name));
        e->item = str_dup(g->name);
        e->type = str_dup(type_name(g->ty));
        e->size = (size_t)size_of(g->ty);
        e->fnptr = g->persist * 2 + (is_pointer(g->ty) && is_func(elem_of(g->ty)));   /* bit 1: [[persist]] */
    }
    return r;
}

/* ---- a [[persist]] global whose type changed, moved field by field through the reflection tables
   (both programs import the runtime's reflection: they list their [[persist]] globals in __c2_persisted).
   Fields match by name (or [[rename]]); numbers convert, an enum keeps its value's name, arrays keep their
   items, owned values (string, array, map) move when their type is the same; what can't move starts
   again from the new initializer, with a warning.  Fields the new type dropped are freed. */
typedef struct { const char *path, *gname; MIR_context_t oldc, newc; char at[256]; } Migration;

static int shape_same_(const c2type *a, const c2type *b, const c2type **sa, const c2type **sb, int depth) {
    if (a == b) return 1;
    if (!a || !b || a->kind != b->kind || a->size != b->size || a->count != b->count || strcmp(a->name, b->name)) return 0;
    for (int i = 0; i < depth; i++) if (sa[i] == a && sb[i] == b) return 1;   /* (a type inside itself: array(Node) in Node) */
    if (depth >= 64) return 0;
    sa[depth] = a; sb[depth] = b;
    if (a->kind == C2_PTR) return 1;                                      /* by name */
    if (!shape_same_(a->of, b->of, sa, sb, depth + 1) || !shape_same_(a->val, b->val, sa, sb, depth + 1)) return 0;
    for (int i = 0; a->fields && i < a->count; i++) {
        const c2field *f = &a->fields[i], *g = &b->fields[i];
        if (strcmp(f->name, g->name) || f->offset != g->offset || f->bits != g->bits || f->bitpos != g->bitpos ||
            !shape_same_(f->type, g->type, sa, sb, depth + 1)) return 0;
    }
    for (int i = 0; a->values && i < a->count; i++)
        if (strcmp(a->values[i].name, b->values[i].name) || a->values[i].value != b->values[i].value) return 0;
    return 1;
}
static int shape_same(const c2type *a, const c2type *b) {
    const c2type *sa[64], *sb[64];
    return shape_same_(a, b, sa, sb, 0);
}
static long long mig_int(const c2type *t, const void *p) {
    int k = t->kind == C2_ENUM && t->of ? t->of->kind : t->kind, sz = t->kind == C2_ENUM && t->of ? t->of->size : t->size;
    switch (sz) {
    case 1: return k == C2_UINT || k == C2_BOOL ? *(const unsigned char *)p : *(const signed char *)p;
    case 2: return k == C2_UINT ? *(const unsigned short *)p : *(const short *)p;
    case 4: return k == C2_UINT ? *(const unsigned *)p : *(const int *)p;
    default: return *(const long long *)p;
    }
}
static void mig_put(int size, void *p, long long v) {
    if (size == 1) *(unsigned char *)p = (unsigned char)v;
    else if (size == 2) *(unsigned short *)p = (unsigned short)v;
    else if (size == 4) *(unsigned *)p = (unsigned)v;
    else *(long long *)p = v;
}
static int mig_number(const c2type *t) {
    return t && (t->kind == C2_BOOL || t->kind == C2_CHAR || t->kind == C2_INT || t->kind == C2_UINT || t->kind == C2_FLOAT || t->kind == C2_ENUM);
}

static void mig_drop(const c2type *t, void *p) {          /* an old value nothing took: free what it owns */
    if (!t) return;
    if (t->free) { t->free(p); return; }                  /* string, a [[destructor]] */
    if (t->kind == C2_STRUCT) {
        for (int i = 0; i < t->count; i++) if (!t->fields[i].bits) mig_drop(t->fields[i].type, (char *)p + t->fields[i].offset);
    } else if (t->kind == C2_ARRAY && t->of) {
        for (int i = 0; i < t->count; i++) mig_drop(t->of, (char *)p + (size_t)i * t->of->size);
    } else if (t->kind == C2_DYNARRAY && t->of) {
        char *a = *(char **)p;
        if (!a) return;
        size_t n = ((size_t *)a)[-2];
        for (size_t i = 0; i < n; i++) mig_drop(t->of, a + i * t->of->size);
        free(a - 2 * sizeof(size_t));
        *(char **)p = NULL;
    }                                                     /* (a map or set: left as it is) */
}

static void mig_restart(Migration *m, const c2type *ot, const c2type *nt) {
    diag("%s: warning: [[persist]] '%s%s' starts again: its type changed from '%s' to '%s'\n",
         m->path, m->gname, m->at, ot ? ot->name : "?", nt ? nt->name : "?");
}
static void mig_value(Migration *m, const c2type *ot, void *op, const c2type *nt, void *np, const c2field *nf, int obits, int obitpos) {
    int nbits = nf ? nf->bits : 0, nbitpos = nf ? nf->bitpos : 0;
    if (!ot || !nt) return;
    if (mig_number(ot) && mig_number(nt)) {               /* numbers, bools, enums, bit-fields */
        long long iv;
        double dv;
        int isf = ot->kind == C2_FLOAT && !obits;
        if (isf) { dv = ot->size == 4 ? *(const float *)op : *(const double *)op; iv = (long long)dv; }
        else {
            iv = mig_int(ot, op);
            if (obits) {
                iv = (long long)(((unsigned long long)iv >> obitpos) & ((1ULL << obits) - 1));
                if (ot->kind == C2_INT && (iv >> (obits - 1)) & 1) iv -= 1LL << obits;
            }
            dv = (double)iv;
        }
        if (ot->kind == C2_ENUM && nt->kind == C2_ENUM) { /* by the value's name */
            const char *name = NULL;
            for (int i = 0; i < ot->count && !name; i++) if (ot->values[i].value == iv) name = ot->values[i].name;
            int found = 0;
            for (int i = 0; name && i < nt->count && !found; i++)
                if (!strcmp(nt->values[i].name, name)) { iv = nt->values[i].value; found = 1; }
            if (!found) {
                diag("%s: warning: [[persist]] '%s%s' starts again: %s has no %s\n", m->path, m->gname, m->at, nt->name, name ? name : "such value");
                return;
            }
            dv = (double)iv;
        }
        if (nf && (nf->flags & C2_CLAMP)) {               /* the new [[clamp]] */
            if (dv < nf->min) dv = nf->min;
            if (dv > nf->max) dv = nf->max;
            if (!isf) iv = (long long)dv;
        }
        if (nt->kind == C2_FLOAT && !nbits) { if (nt->size == 4) *(float *)np = (float)dv; else *(double *)np = dv; return; }
        if (isf) iv = (long long)dv;
        if (nt->kind == C2_BOOL) iv = isf ? dv != 0 : iv != 0;
        if (nbits) {
            unsigned long long u = (unsigned long long)mig_int(nt, np), mask = ((1ULL << nbits) - 1) << nbitpos;
            u = (u & ~mask) | (((unsigned long long)iv << nbitpos) & mask);
            mig_put(nt->size, np, (long long)u);
        } else mig_put(nt->kind == C2_ENUM && nt->of ? nt->of->size : nt->size, np, iv);
        return;
    }
    if (obits || nbits) { mig_restart(m, ot, nt); return; }
    if (shape_same(ot, nt)) {                             /* the same: moved whole (owned memory included) */
        if (nt->kind == C2_PTR && !nt->of) {              /* a function pointer: to the new program's function */
            void *fp = *(void *const *)op;
            const char *fname = fp ? ctx_func_at(m->oldc, fp) : NULL;
            int isf = 0;
            void *nfp = fname ? ctx_get(m->newc, fname, &isf) : NULL;
            if (fname && !isf) diag("%s: warning: [[persist]] '%s%s' pointed to '%s', which the new program doesn't define: it is NULL now\n",
                                    m->path, m->gname, m->at, demangle(fname));
            *(void **)np = fname ? (isf ? nfp : NULL) : fp;
            return;
        }
        mig_drop(nt, np);                                 /* (what the new initializer made) */
        memcpy(np, op, (size_t)nt->size);
        return;
    }
    size_t mark = strlen(m->at);
    if (ot->kind == C2_STRUCT && nt->kind == C2_STRUCT) {
        char *used = (char *)calloc((size_t)ot->count + 1, 1);
        for (int i = 0; used && i < nt->count; i++) {
            const c2field *g = &nt->fields[i];
            if (!g->type || (g->flags & C2_SKIP) || !g->name[0]) continue;
            const char *gk = g->key ? g->key : g->name;
            for (int j = 0; j < ot->count; j++) {
                const c2field *f = &ot->fields[j];
                const char *fk = f->key ? f->key : f->name;
                if (used[j] || !f->type || !f->name[0] || (strcmp(f->name, g->name) && strcmp(fk, gk))) continue;
                used[j] = 1;
                snprintf(m->at + mark, sizeof m->at - mark, ".%s", g->name);
                mig_value(m, f->type, (char *)op + f->offset, g->type, (char *)np + g->offset, g, f->bits, f->bitpos);
                m->at[mark] = '\0';
                break;
            }
        }
        for (int j = 0; used && j < ot->count; j++)      /* the fields it dropped */
            if (!used[j] && ot->fields[j].type && !ot->fields[j].bits) mig_drop(ot->fields[j].type, (char *)op + ot->fields[j].offset);
        free(used);
        return;
    }
    if (ot->kind == C2_ARRAY && nt->kind == C2_ARRAY && ot->of && nt->of) {
        for (int i = 0; i < nt->count && i < ot->count; i++) {
            snprintf(m->at + mark, sizeof m->at - mark, "[%d]", i);
            mig_value(m, ot->of, (char *)op + (size_t)i * ot->of->size, nt->of, (char *)np + (size_t)i * nt->of->size, nf, 0, 0);
        }
        m->at[mark] = '\0';
        for (int i = nt->count; i < ot->count; i++) mig_drop(ot->of, (char *)op + (size_t)i * ot->of->size);
        return;
    }
    if (ot->kind == C2_DYNARRAY && nt->kind == C2_DYNARRAY && ot->of && nt->of) {   /* items moved one by one */
        char *a = *(char **)op;
        size_t n = a ? ((size_t *)a)[-2] : 0;
        mig_drop(nt, np);                                 /* (what the new initializer made) */
        if (!n) { if (a) free(a - 2 * sizeof(size_t)); return; }
        size_t *h = (size_t *)calloc(1, 2 * sizeof(size_t) + n * (size_t)nt->of->size);
        if (!h) { mig_restart(m, ot, nt); return; }
        h[0] = h[1] = n;
        char *b = (char *)(h + 2);
        for (size_t i = 0; i < n; i++) {
            snprintf(m->at + mark, sizeof m->at - mark, "[%zu]", i);
            mig_value(m, ot->of, a + i * ot->of->size, nt->of, b + i * nt->of->size, nf, 0, 0);
        }
        m->at[mark] = '\0';
        free(a - 2 * sizeof(size_t));
        *(char **)np = b;
        return;
    }
    if ((ot->kind == C2_MAP && nt->kind == C2_MAP) || (ot->kind == C2_SET && nt->kind == C2_SET)) {
        /* entries moved one by one into a map of the new types, through the new program's map_put_ */
        typedef void *(*Put)(void *, const void *, size_t, size_t, int, size_t *, int *, void *const *);
        int isf = 0;
        Put put = (Put)ctx_get(m->newc, "map_put_", &isf);
        void *const *ops = nt->of ? nt->of->keyops : NULL;   /* a string key: the new program's key operations */
        int ismap = ot->kind == C2_MAP;
        if (put && isf && ot->of && nt->of && (!ismap || (ot->val && nt->val))) {
            struct mhdr { size_t count, cap; unsigned *slots; size_t nslots; };
            size_t ova = ismap ? (size_t)ot->val->align : 1, nva = ismap ? (size_t)nt->val->align : 1;
            size_t oka = (size_t)ot->of->align, nka = (size_t)nt->of->align;
            size_t ovoff = ((size_t)ot->of->size + ova - 1) / ova * ova, nvoff = ((size_t)nt->of->size + nva - 1) / nva * nva;
            size_t oal = oka > ova ? oka : ova, nal = nka > nva ? nka : nva;
            size_t oes = ((ovoff + (ismap ? (size_t)ot->val->size : 0)) + oal - 1) / oal * oal;
            size_t nes = ((nvoff + (ismap ? (size_t)nt->val->size : 0)) + nal - 1) / nal * nal;
            int okind = ot->of->kind == C2_PTR && ot->of->of && ot->of->of->kind == C2_CHAR;
            int nkind = nt->of->kind == C2_PTR && nt->of->of && nt->of->of->kind == C2_CHAR;
            char *a = *(char **)op;
            size_t n = a ? ((struct mhdr *)a - 1)->count : 0;
            void *nm = *(void **)np;
            char *kb = (char *)calloc(1, (size_t)nt->of->size + 16);
            for (size_t i = 0; kb && i < n; i++) {
                size_t at = 0;
                int added = 0;
                memset(kb, 0, (size_t)nt->of->size);
                snprintf(m->at + mark, sizeof m->at - mark, "[%zu].key", i);
                mig_value(m, ot->of, a + i * oes, nt->of, kb, NULL, 0, 0);
                nm = put(nm, kb, nes, (size_t)nt->of->size, nkind, &at, &added, ops);
                if (ops && ops[3]) ((void (*)(void *))ops[3])(kb);   /* the map made its own copy */
                if (ismap) {
                    snprintf(m->at + mark, sizeof m->at - mark, "[%zu].value", i);
                    mig_value(m, ot->val, a + i * oes + ovoff, nt->val, (char *)nm + at * nes + nvoff, nf, 0, 0);
                }
            }
            m->at[mark] = '\0';
            free(kb);
            if (a) {                                      /* the old map's memory (its values moved) */
                struct mhdr *h = (struct mhdr *)a - 1;
                if (okind) for (size_t i = 0; i < n; i++) free(*(char **)(a + i * oes));
                free(h->slots);
                free(h);
            }
            *(void **)np = nm;
            return;
        }
    }
    mig_restart(m, ot, nt);                               /* a string to a number...: */
    mig_drop(ot, op);                                     /* the new initializer stays */
}

static const c2type *persisted_type(MIR_context_t c, const char *key) {   /* from the program's __c2_persisted */
    const c2global *const *tab = (const c2global *const *)ctx_get(c, "__c2_persisted", NULL);
    for (const c2global *g = tab ? *tab : NULL; g && g->name; g++)
        if (!strcmp(g->name, key)) return g->type;
    return NULL;
}

/* after the new program's initializers: its [[persist]] globals take the old program's values */
static void carry_persisted(C2 *cc, MIR_context_t newctx, struct C2Global *ng, int n, const char *path) {
    for (int i = 0; i < n; i++) {
        struct C2Global *e = &ng[i];
        if (!(e->fnptr & 2)) continue;
        struct C2Global *o = NULL;
        for (int k = 0; k < cc->nglobals && !o; k++) if (!strcmp(cc->globals[k].key, e->key)) o = &cc->globals[k];
        if (!o) continue;                              /* new: keeps its initializer */
        const c2type *ot = persisted_type(cc->ctx, e->key), *nt = persisted_type(newctx, e->key);
        if (ot && nt && !shape_same(ot, nt)) {        /* changed, and both have tables: field by field */
            void *from = ctx_get(cc->ctx, o->item, NULL), *to = ctx_get(newctx, e->item, NULL);
            Migration m = {path, e->key, cc->ctx, newctx, ""};
            if (from && to) mig_value(&m, ot, from, nt, to, NULL, 0, 0);
            continue;
        }
        if (o->size != e->size || strcmp(o->type, e->type)) {
            diag("%s: warning: [[persist]] '%s' starts again: its type changed from '%s' to '%s'\n",
                 path, e->key, o->type, e->type);
            continue;
        }
        void *from = ctx_get(cc->ctx, o->item, NULL), *to = ctx_get(newctx, e->item, NULL);
        if (!from || !to) continue;
        memcpy(to, from, e->size);
        if (e->fnptr & 1) {                            /* a function pointer: to the new program's function */
            void *fp = *(void **)to;
            const char *fname = fp ? ctx_func_at(cc->ctx, fp) : NULL;
            if (fname) {
                int isf = 0;
                void *nf = ctx_get(newctx, fname, &isf);
                if (!nf || !isf) diag("%s: warning: [[persist]] '%s' pointed to '%s', which the new program doesn't define: it is NULL now\n",
                                      path, e->key, demangle(fname));
                *(void **)to = isf ? nf : NULL;
            }
        }
    }
}
#endif

/* every global the compiler uses, back to its initial state (the compiler's heap is freed) */
static void reset_compiler_state(void) {
    memset(args_chunks, 0, sizeof args_chunks); args_depth = 0;   /* (freed with the rest of the compile) */
    cc_free_all();
    /* preprocessor */
    macros = NULL; nmacros = cap_macros = 0;
    memset(pp_files, 0, sizeof pp_files); npp_files = 0; pp_ctx = NULL; pp_main_file = -1;
    nmodules = nprivates = mod_sp = nmod_aliases = 0; memset(module_alias, 0, sizeof module_alias);
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
    memset(items, 0, sizeof items[0] * nitems); nitems = 0; memset(item_hash, 0, sizeof item_hash);
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
    refl_reset();                                      /* (an earlier compile's tables and enums) */
    attr_warned = NULL;
    nenum_infos = nenum_vals = 0;
    memset(funcs, 0, sizeof funcs[0] * nfuncs); nfuncs = 0;
    noper = 0;
    novl_names = novl_sets = 0;
    ntemps = ndtors = ncopies = 0;
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

/* C2_sandbox: what a sandboxed script gets instead of the C library functions that delete, rename or
   write files, start programs or load libraries: they fail (-1, NULL or 0, errno EPERM) when called, so
   a script using them still compiles, and the runtime's saves just fail. */
static long sb_fail(void) { errno = EPERM; return -1; }
static void *sb_null(void) { errno = EPERM; return NULL; }
static int sb_false(void) { errno = EPERM; return 0; }
/* exit() and abort() in the sandbox: the program leaves every function, as for a throw no try catches,
   back to the host (C2_exited tells it) */
static void sb_exit(int code) { host_exiting = 1; host_exit_code = code; host_throwing = INT_MIN; }
static void sb_abort(void) { host_exiting = 2; host_exit_code = 3; host_throwing = INT_MIN; }
static FILE *sb_fopen(const char *path, const char *mode) {   /* reading is fine */
    if (strpbrk(mode, "wa+")) { errno = EPERM; return NULL; }
    return fopen(path, mode);
}
static void *sandbox_symbol(const char *name) {
    static const char *fail[] = {"remove", "unlink", "_unlink", "rename", "rmdir", "_rmdir", "mkdir", "_mkdir", "system", "_wsystem",
        "chmod", "_chmod", "chown", "link", "symlink", "truncate", "ftruncate", "chdir", "_chdir", "kill", "fork", "vfork",
        "execv", "execve", "execvp", "execl", "execlp", "execle", "_execv", "_execvp", "_spawnv", "_spawnvp",
        "posix_spawn", "posix_spawnp", "open", "_open", "creat", "_creat", "atexit", "at_quick_exit", NULL};
    static const char *null[] = {"popen", "_popen", "dlopen", "dlsym", "freopen", "_wfopen", "LoadLibraryA", "LoadLibraryW",
        "LoadLibraryExA", "LoadLibraryExW", "GetProcAddress", "ShellExecuteA", "ShellExecuteW", NULL};
    static const char *zero[] = {"DeleteFileA", "DeleteFileW", "MoveFileA", "MoveFileW", "MoveFileExA", "MoveFileExW", "CopyFileA",
        "CopyFileW", "CreateProcessA", "CreateProcessW", "RemoveDirectoryA", "RemoveDirectoryW", "CreateDirectoryA",
        "CreateDirectoryW", "WinExec", NULL};
    if (!strcmp(name, "fopen")) return (void *)sb_fopen;
    if (!strcmp(name, "exit") || !strcmp(name, "_exit") || !strcmp(name, "_Exit") || !strcmp(name, "quick_exit")) return (void *)sb_exit;
    if (!strcmp(name, "abort")) return (void *)sb_abort;
    for (int i = 0; fail[i]; i++) if (!strcmp(fail[i], name)) return (void *)sb_fail;
    for (int i = 0; null[i]; i++) if (!strcmp(null[i], name)) return (void *)sb_null;
    for (int i = 0; zero[i]; i++) if (!strcmp(zero[i], name)) return (void *)sb_false;
    return NULL;
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
    if (linking && linking->sandbox) {                /* its own list, then C²'s table: nothing found by name */
        void *s = sandbox_symbol(name);
        return s ? s : builtin_symbol(name);
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

/* `c2 -C` output asks c2_rt("printf") for each C library function it calls.  c2_rt.c (c2 -Crt) is one
   way to have it; this is the other: link the program with c2 built as a library (C2_NO_MAIN), as a
   game that embeds C² already is, and its scripts compiled ahead of time get the same C library,
   coroutines, try/throw and -g state the JIT gives them. */
void *c2_rt(const char *name) { return lookup_symbol(name); }

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

/* structs that own things (.c2): a struct with members that have a [[destructor]] (a string, an array of
   them, a struct of its own like that) gets S_free, a [[destructor]] freeing them last to first, and,
   when they all have a [[copy]], S_copy, a [[copy]] copying them; unless the file defines those names
   itself.  So temporaries, copies and explicit p.free() work for game objects as for strings. */
static int owned_checked;
static void gen_owned(int si) {
    Struct *st = &structs[si];
    const char *tn = struct_name(si);
    if (!tn || st->is_union || !st->complete || st->depth != 0 || st->hidden) return;
    Type sty = mk_type(TY_STRUCT);
    sty.sidx = si;
    if (dtor_for(sty)) return;
    int own[64], n = 0, all_copy = 1;
    for (int i = 0; i < st->n; i++) {
        Type t = st->m[i].ty;
        if (!st->m[i].name[0]) continue;
        if (is_array(t) && t.nd == 1) t = elem_of(t);
        else if (is_array(t)) continue;
        if (!is_struct_val(t) || !dtor_for(t)) continue;
        own[n++] = i;
        if (!copy_for(t)) all_copy = 0;
    }
    if (!n) return;
    char fname[80], cname[80];
    snprintf(fname, sizeof fname, "%s_free", tn);
    snprintf(cname, sizeof cname, "%s_copy", tn);
    int do_free = !is_defined_in_file(fname) && !find_func(fname);
    int do_copy = all_copy && !is_defined_in_file(cname) && !find_func(cname);
    if (!do_free) return;                              /* the file frees it its own way */
    Buf b = {0};
    buf_emit(&b, "[[export, destructor]] void %s(%s *p) {\n", fname, tn);
    for (int k = n - 1; k >= 0; k--) {
        Member *m = &st->m[own[k]];
        Type t = m->ty;
        Func *d = dtor_for(is_array(t) ? elem_of(t) : t);
        if (is_array(t)) buf_emit(&b, "    for (size_t i_ = %d; i_-- > 0;) %s(&p->%s[i_]);\n", arr_len(t), d->name, m->name);
        else buf_emit(&b, "    %s(&p->%s);\n", d->name, m->name);
    }
    buf_emit(&b, "}\n");
    if (do_copy) {
        buf_emit(&b, "[[export, copy]] %s %s(const %s *p) {\n    %s r;\n    memcpy(&r, p, sizeof r);\n", tn, cname, tn, tn);
        for (int k = 0; k < n; k++) {
            Member *m = &st->m[own[k]];
            Type t = m->ty;
            Func *c = copy_for(is_array(t) ? elem_of(t) : t);
            if (is_array(t))
                buf_emit(&b, "    for (size_t i_ = 0; i_ < %d; i_++) { typeof_unqual(p->%s[0]) t_ = %s((typeof_unqual(p->%s[0]) *)&p->%s[i_]);"
                             " memcpy(&r.%s[i_], &t_, sizeof t_); }\n", arr_len(t), m->name, c->name, m->name, m->name, m->name);
            else
                buf_emit(&b, "    { typeof_unqual(p->%s) t_ = %s((typeof_unqual(p->%s) *)&p->%s); memcpy(&r.%s, &t_, sizeof t_); }\n",
                         m->name, c->name, m->name, m->name, m->name);
        }
        buf_emit(&b, "    return r;\n}\n");
    }
    if (ndefined < 1024) snprintf(defined_names[ndefined++], 32, "%.31s", fname);
    if (do_copy && ndefined < 1024) snprintf(defined_names[ndefined++], 32, "%.31s", cname);
    LexState ls = save_lex();
    int line = cur_line;
    src = b.p;
    next_token();
    while (current_token.type != TOKEN_EOF) parse_toplevel();
    restore_lex(ls);
    cur_line = line;
    cc_free(b.p);
}

static void gen_owned_all(void) {
    if (!c2_ext || !ndtors) { owned_checked = nstructs; return; }
    while (owned_checked < nstructs) gen_owned(owned_checked++);
}

/* ============================================================ reflection (runtime/reflect.c2)
 * reflect(T) is the address of a global __c2_ty_N, a const c2type.  The types reflect() reached, and
 * all they lead to (fields, elements, pointees), get their tables at the end of the file, as C²
 * text parsed like the program's own (their storage exists from the first use; the values are stored
 * before main). */
static Type rtypes[2048];
static int nrtypes, rtypes_done;
static int refl_want_globals;
static int nkeyops, keyops_done, keyops_unit;          /* (a unit's tables have names of their own) */
static void refl_reset(void) { nrtypes = rtypes_done = refl_want_globals = nkeyops = keyops_done = 0; keyops_unit++; }
enum { RK_VOID, RK_BOOL, RK_CHAR, RK_INT, RK_UINT, RK_FLOAT, RK_ENUM, RK_PTR, RK_ARRAY, RK_FUNC, RK_STRUCT,
       RK_UNION, RK_STRING, RK_DYNARRAY, RK_MAP, RK_SET };   /* runtime/reflect.c2's c2kind, in order */

static Type refl_norm(Type t) {                    /* qualifiers and bit-field widths aren't part of it */
    t.bcq = 0; t.bfw = t.bfp = 0;
    for (int i = 0; i < t.nd; i++) t.d[i].cq = 0;
    return t;
}
static int refl_same(Type a, Type b) {
    if (!same_type(a, b) || a.eid != b.eid) return 0;
    for (int i = 0; i < a.nd; i++)
        if (a.d[i].ck != b.d[i].ck || (a.d[i].kind == D_ARR && a.d[i].n != b.d[i].n)) return 0;
    return 1;
}
static int refl_skipped(Type t) {                  /* [[skip]]: reflection knows nothing about it */
    if (t.nd) return 0;
    if (t.base == TY_STRUCT && t.sidx >= 0) return structs[t.sidx].skip;
    return t.eid && enum_infos[t.eid - 1].skip;
}
static int refl_string(Type t) {                   /* the runtime's string */
    if (t.nd || t.base != TY_STRUCT || t.sidx < 0 || !structs[t.sidx].is_union) return 0;
    const char *n = struct_name(t.sidx);
    return n && !strcmp(n, "string") && dtor_for(t);
}
static int refl_id(Type t) {                       /* -1: [[skip]] */
    t = refl_norm(t);
    if (refl_skipped(t)) return -1;
    for (int i = 0; i < nrtypes; i++) if (refl_same(rtypes[i], t)) return i;
    if (nrtypes >= 2048) error("too many reflected types");
    rtypes[nrtypes] = t;
    return nrtypes++;
}
static int refl_kind(Type t) {
    if (is_pointer(t)) {
        int ck = t.d[t.nd - 1].ck;
        return ck == C2_CK_ARRAY ? RK_DYNARRAY : ck == C2_CK_MAP ? RK_MAP : ck == C2_CK_SET ? RK_SET : RK_PTR;
    }
    if (is_array(t)) return RK_ARRAY;
    if (is_func(t)) return RK_FUNC;
    if (t.base == TY_STRUCT) return refl_string(t) ? RK_STRING : structs[t.sidx].is_union ? RK_UNION : RK_STRUCT;
    if (t.eid) return RK_ENUM;
    if (is_void(t)) return RK_VOID;
    if (is_bool(t)) return RK_BOOL;
    if (is_fp(t)) return RK_FLOAT;
    if (t.base == TY_CHAR && !t.uns) return RK_CHAR;
    return t.uns ? RK_UINT : RK_INT;
}
static Type refl_container_part(Type t, const char *member) {   /* a map's key or value: its pointee's member */
    Type e = elem_of(t);
    Struct *st = &structs[e.sidx];
    for (int i = 0; i < st->n; i++) if (!strcmp(st->m[i].name, member)) return st->m[i].ty;
    return mk_type(TY_VOID);
}
static void refl_name(Type t, char *out, size_t n) {   /* "Player", "float *", "char [8]", "array(int)" */
    char in[96];
    int k = refl_kind(t);
    if (k == RK_DYNARRAY || k == RK_SET) { refl_name(k == RK_SET ? refl_container_part(t, "key") : elem_of(t), in, sizeof in);
                                          snprintf(out, n, "%s(%s)", k == RK_SET ? "set" : "array", in); return; }
    if (k == RK_MAP) {
        char v[96];
        refl_name(refl_container_part(t, "key"), in, sizeof in);
        refl_name(refl_container_part(t, "value"), v, sizeof v);
        snprintf(out, n, "map(%s, %s)", in, v);
        return;
    }
    if (k == RK_PTR) { refl_name(elem_of(t), in, sizeof in); snprintf(out, n, "%s *", in); return; }
    if (k == RK_ARRAY) { refl_name(elem_of(t), in, sizeof in); snprintf(out, n, "%s [%d]", in, arr_len(t)); return; }
    if (k == RK_FUNC) { snprintf(out, n, "function"); return; }
    if (t.base == TY_STRUCT) { const char *s = struct_name(t.sidx); snprintf(out, n, "%s", s ? s : "<anonymous>"); return; }
    if (t.eid) {
        const char *s = enum_infos[t.eid - 1].name;
        for (int i = 0; !s[0] && i < ntypedefs; i++)                 /* typedef enum { ... } Dir; */
            if (typedefs[i].ty.eid == t.eid && !typedefs[i].ty.nd) s = typedefs[i].name;
        snprintf(out, n, "%s", s[0] ? demangle(s) : "<anonymous enum>");
        return;
    }
    if (k == RK_VOID) { snprintf(out, n, "void"); return; }
    if (k == RK_BOOL) { snprintf(out, n, "bool"); return; }
    if (k == RK_CHAR) { snprintf(out, n, "char"); return; }
    static const char *ints[] = {"", "char", "short", "int", "long long"};
    if (k == RK_INT || k == RK_UINT) {
        snprintf(out, n, "%s%s", t.uns ? "unsigned " : (t.base == TY_CHAR ? "signed " : ""), ints[t.base]);
        return;
    }
    snprintf(out, n, "%s", is_float(t) ? "float" : "double");
}

/* reflect(T) / reflect(expr): the address of T's table */
static Val reflect_of(Type t) {
    int si = find_struct("c2type");
    if (!c2_ext) error("reflect() needs a .c2 file");
    if (si < 0 || !structs[si].complete) error("reflect() needs #import \"runtime\" (or \"runtime/reflect\")");
    t = refl_norm(t);
    char tn[200];
    if (refl_skipped(t)) { refl_name(t, tn, sizeof tn); error("reflect(%s): the type is [[skip]], so reflection knows nothing about it", tn); }
    int id = refl_id(t);
    char name[40];
    snprintf(name, sizeof name, "__c2_ty_%d", id);
    Type ct = mk_type(TY_STRUCT);
    ct.sidx = si;
    ct.bcq = 1;
    Global *g = find_global(name);
    if (!g) { g = add_global(name, ct); g->deferred = 1; }
    ensure_global_storage(g);
    int a = new_temp(RK_I);
    EMIT(MOV, R(a), REF(g->name));
    return rv_reg(a, ptr_to(ct));
}

/* reflect_globals(): a const c2global * to the program's globals ({NULL} at the end), for consoles and
   editors: every global of the script's own files that isn't private to a module or [[skip]] */
static Val reflect_globals_of(void) {
    int si = find_struct("c2global");
    if (!c2_ext) error("reflect_globals() needs a .c2 file");
    if (si < 0 || !structs[si].complete) error("reflect_globals() needs #import \"runtime\" (or \"runtime/reflect\")");
    Type ct = mk_type(TY_STRUCT);
    ct.sidx = si;
    ct.bcq = 1;
    Type pt = ptr_to(ct);
    Global *g = find_global("__c2_globals");
    if (!g) { g = add_global("__c2_globals", pt); be_bss("__c2_globals", size_of(pt)); }
    refl_want_globals = 1;
    int a = new_temp(RK_I);
    EMIT(MOV, R(a), REF(g->name));
    return rvalue(lv_mem(a, 0, pt));
}
static Val reflect_functions_of(void) {               /* a const c2func * ({NULL} at the end) */
    int si = find_struct("c2func");
    if (si < 0 || !structs[si].complete) error("reflect_functions() needs #import \"runtime\" (or \"runtime/reflect\")");
    Type ct = mk_type(TY_STRUCT);
    ct.sidx = si;
    ct.bcq = 1;
    Type pt = ptr_to(ct);
    Global *g = find_global("__c2_functions");
    if (!g) { g = add_global("__c2_functions", pt); be_bss("__c2_functions", size_of(pt)); }
    refl_want_globals = 1;
    int a = new_temp(RK_I);
    EMIT(MOV, R(a), REF(g->name));
    return rvalue(lv_mem(a, 0, pt));
}
/* a type as C² source, for a declaration "T name" (NULL: it can't be written that way) */
static const char *refl_spell(Type t) {
    if (is_func(t) || is_array(t)) return NULL;
    for (int i = 0; i < t.nd; i++) if (t.d[i].kind != D_PTR) return NULL;
    const char *n = type_name(t);
    if (strchr(n, '(') || strchr(n, '<') || strstr(n, "...")) return NULL;
    if (t.base == TY_STRUCT && !structs[t.sidx].name[0]) return NULL;
    return n;
}
/* the program's own functions that a console can call by name: defined outside the #import'ed modules,
   not variadic, every parameter and the result reflected and writable as a declaration */
static int refl_callable(Func *f) {
    if (!f->defined || f->vararg || f->is_import || f->in_module || f->unprototyped || f->np > 16) return 0;
    if (!strncmp(f->name, "__", 2) || !strcmp(f->name, "main") || strstr(f->name, "__") || strcmp(demangle(f->name), f->name)) return 0;
    int fi = (int)(f - funcs);                         /* destructors and copies: called for you, not by name */
    for (int i = 0; i < ndtors; i++) if (dtor_fi[i] == fi) return 0;
    for (int i = 0; i < ncopies; i++) if (copy_fi[i] == fi) return 0;
    if (!is_void(f->ret) && (!refl_spell(f->ret) || refl_id(f->ret) < 0)) return 0;
    for (int i = 0; i < f->np; i++) if (!refl_spell(f->pt[i]) || refl_id(f->pt[i]) < 0) return 0;
    return 1;
}
static int refl_listed_global(Global *g) {
    if (g->imported || g->skip || !strncmp(g->name, "__", 2) || strcmp(demangle(g->name), g->name)) return 0;
    if (is_func(g->ty) || (is_array(g->ty) && arr_len(g->ty) < 0)) return 0;
    return refl_id(g->ty) >= 0;
}

/* the tables of every type reached, as C² text: parsed at the end of the file */
static int keyops_want(Type t, char *name, size_t n);
static void keyops_generate(void);
static void refl_generate(void) {
    int nlisted = nglobals;                           /* (the tables add globals of their own) */
    int gsi = find_struct("c2global");                /* the runtime's reflection is in: the host may ask (C2_reflected) */
    if (c2_ext && gsi >= 0 && structs[gsi].complete) refl_want_globals = 1;
    if (refl_want_globals) for (int i = 0; i < nlisted; i++) refl_listed_global(&globals[i]);
    int nfuncs_listed = nfuncs, fsi = find_struct("c2func");   /* functions by name: their types reached first */
    int want_funcs = refl_want_globals && fsi >= 0 && structs[fsi].complete && rtypes_done == 0;
    if (want_funcs) for (int i = 0; i < nfuncs_listed; i++) refl_callable(&funcs[i]);
    int npersist = 0;                                 /* [[persist]] globals: a hot reload moves them field by field */
    if (refl_want_globals) for (int i = 0; i < nlisted; i++)
        if (globals[i].persist && !globals[i].imported && !is_func(globals[i].ty) && refl_id(globals[i].ty) >= 0) npersist++;
    if (!nrtypes) return;
    int si = find_struct("c2type");
    Type ct = mk_type(TY_STRUCT);
    ct.sidx = si;
    ct.bcq = 1;
    for (int i = 0; i < nrtypes; i++) {               /* what they lead to (refl_id appends) */
        Type t = rtypes[i];
        int k = refl_kind(t);
        if (k == RK_PTR || k == RK_ARRAY || k == RK_DYNARRAY) { if (!is_func(elem_of(t))) refl_id(elem_of(t)); }
        else if (k == RK_MAP) { refl_id(refl_container_part(t, "key")); refl_id(refl_container_part(t, "value")); }
        else if (k == RK_SET) refl_id(refl_container_part(t, "key"));
        else if (k == RK_ENUM) { Type b = t; b.eid = 0; refl_id(b); }
        else if (k == RK_STRUCT || k == RK_UNION)
            for (int f = 0; f < structs[t.sidx].n; f++) refl_id(structs[t.sidx].m[f].ty);
    }
    Buf b = {0};
    for (int i = rtypes_done; i < nrtypes; i++) {      /* every table has storage before any refers to it */
        char name[40];
        snprintf(name, sizeof name, "__c2_ty_%d", i);
        Global *g = find_global(name);
        if (!g) { g = add_global(name, ct); g->deferred = 1; }
        ensure_global_storage(g);
    }
    for (int i = rtypes_done; i < nrtypes; i++) {
        Type t = rtypes[i];
        int k = refl_kind(t);
        char tn[200], of[40] = "NULL", val[40] = "NULL", fields[40] = "NULL", values[40] = "NULL", fr[120] = "NULL", cp[40] = "NULL";
        int count = 0;
        refl_name(t, tn, sizeof tn);
        int sz = k == RK_FUNC || k == RK_VOID ? 0 : size_of(t), al = k == RK_FUNC || k == RK_VOID ? 1 : align_of(t);
        Type o = mk_type(TY_VOID), v = mk_type(TY_VOID);
        int has_of = 0, has_val = 0;
        if (k == RK_PTR || k == RK_ARRAY || k == RK_DYNARRAY) { o = elem_of(t); has_of = !is_func(o); }
        if (k == RK_MAP || k == RK_SET) { o = refl_container_part(t, "key"); has_of = 1; }
        if (k == RK_MAP) { v = refl_container_part(t, "value"); has_val = 1; }
        if (k == RK_ENUM) { o = t; o.eid = 0; has_of = 1; }
        if (k == RK_ARRAY) count = arr_len(t);
        if (has_of && refl_id(o) >= 0) snprintf(of, sizeof of, "&__c2_ty_%d", refl_id(o));
        if (has_val && refl_id(v) >= 0) snprintf(val, sizeof val, "&__c2_ty_%d", refl_id(v));
        if (k == RK_STRUCT || k == RK_UNION) {
            Struct *st = &structs[t.sidx];
            count = st->n;
            if (count) {
                snprintf(fields, sizeof fields, "__c2_tyf_%d", i);
                buf_emit(&b, "static const c2field __c2_tyf_%d[] = {\n", i);
                for (int f = 0; f < st->n; f++) {
                    Member *m = &st->m[f];
                    int fid = refl_id(m->ty);
                    char ft[40] = "NULL";
                    if (fid >= 0) snprintf(ft, sizeof ft, "&__c2_ty_%d", fid);
                    char key[48] = "NULL";
                    if (m->rename) snprintf(key, sizeof key, "\"%s\"", m->rename);
                    const FieldMeta *fm = m->meta;
                    int flags = ((m->skip || fid < 0) ? 1 : 0) | (fm && fm->has_clamp ? 2 : 0);
                    buf_emit(&b, "    {\"%s\", %s, %d, %d, %d, %d, %s, ", m->name, ft, m->off, m->ty.bfw, m->ty.bfp, flags, key);
                    if (fm && fm->comment) buf_emit(&b, "\"%s\", ", fm->comment); else buf_emit(&b, "NULL, ");
                    buf_emit(&b, "%.17g, %.17g, %d},\n", fm && fm->has_clamp ? fm->lo : 0.0, fm && fm->has_clamp ? fm->hi : 0.0,
                             fm ? fm->maxlen : 0);
                }
                buf_emit(&b, "};\n");
            }
        }
        if (k == RK_ENUM) {
            EnumInfo *ei = &enum_infos[t.eid - 1];
            count = ei->n;
            if (count) {
                snprintf(values, sizeof values, "__c2_tye_%d", i);
                buf_emit(&b, "static const c2enum __c2_tye_%d[] = {\n", i);
                for (int e = 0; e < ei->n; e++)
                    buf_emit(&b, "    {\"%s\", %lldLL},\n", demangle(enum_vals[ei->first + e].name), enum_vals[ei->first + e].val);
                buf_emit(&b, "};\n");
            }
        }
        char ko[120] = "NULL";
        if (!t.nd && t.base == TY_STRUCT) {
            Func *d = dtor_for(t), *c = copy_for(t);
            char kn[96];
            if (keyops_want(t, kn, sizeof kn)) {          /* as a map key: its table (keyops_generate) */
                fmt_to(ko, sizeof ko, "(void *const *)%s", kn);
            }
            if (d) snprintf(fr, sizeof fr, "(void (*)(void *))%s", d->name);
            if (c) {
                snprintf(cp, sizeof cp, "__c2_tyc_%d", i);
                buf_emit(&b, "static void __c2_tyc_%d(void *d, const void *s) { typeof(%s((void *)s)) r = %s((void *)s); memcpy(d, &r, sizeof r); }\n",
                         i, c->name, c->name);
            }
        }
        buf_emit(&b, "const c2type __c2_ty_%d = {\"%s\", %d, %d, %d, %s, %s, %d, %s, %s, %s, %s, %s};\n",
                 i, tn, k, sz, al, of, val, count, fields, values, fr, cp, ko);
    }
    if (refl_want_globals && rtypes_done == 0) {
        buf_emit(&b, "static const c2global __c2_gtab[] = {\n");
        for (int i = 0; i < nlisted; i++)
            if (refl_listed_global(&globals[i]))
                buf_emit(&b, "    {\"%s\", &__c2_ty_%d, (void *)&%s},\n", globals[i].name, refl_id(globals[i].ty), globals[i].name);
        buf_emit(&b, "    {NULL, NULL, NULL}};\nconst c2global *__c2_globals = __c2_gtab;\n");
        if (npersist) {
            buf_emit(&b, "static const c2global __c2_ptab[] = {\n");
            for (int i = 0; i < nlisted; i++)
                if (globals[i].persist && !globals[i].imported && !is_func(globals[i].ty) && refl_id(globals[i].ty) >= 0)
                    buf_emit(&b, "    {\"%s\", &__c2_ty_%d, (void *)&%s},\n", demangle(globals[i].name), refl_id(globals[i].ty), globals[i].name);
            buf_emit(&b, "    {NULL, NULL, NULL}};\nconst c2global *__c2_persisted = __c2_ptab;\n");
        }
    }
    if (want_funcs) {                                 /* a thunk per function: call(args[], result) */
        Buf tab = {0};
        buf_emit(&tab, "static const c2func __c2_ftab[] = {\n");
        for (int i = 0; i < nfuncs_listed; i++) {
            Func *f = &funcs[i];
            if (!refl_callable(f)) continue;
            buf_emit(&b, "static void __c2_fn%d(void **a_, void *r_) {\n", i);
            for (int k = 0; k < f->np; k++) {
                const char *pn = f->un ? f->un[k] : f->pn[k];
                buf_emit(&b, "    %s %s; ", refl_spell(f->pt[k]), pn);
                if (f->pdef && f->pdef[k]) buf_emit(&b, "if (a_[%d]) memcpy(&%s, a_[%d], sizeof %s); else %s = (%s);\n", k, pn, k, pn, pn, f->pdef[k]);
                else buf_emit(&b, "memcpy(&%s, a_[%d], sizeof %s);\n", pn, k, pn);
            }
            if (is_void(f->ret)) buf_emit(&b, "    (void)r_; %s(", f->name);
            else buf_emit(&b, "    %s res_ = %s(", refl_spell(f->ret), f->name);
            for (int k = 0; k < f->np; k++) buf_emit(&b, "%s%s", k ? ", " : "", f->un ? f->un[k] : f->pn[k]);
            buf_emit(&b, ");\n%s}\n", is_void(f->ret) ? "" : "    memcpy(r_, &res_, sizeof res_);\n");
            if (f->np) {
                buf_emit(&b, "static const c2param __c2_fp%d[] = {", i);
                for (int k = 0; k < f->np; k++)
                    buf_emit(&b, "{\"%s\", &__c2_ty_%d, %d}, ", f->un ? f->un[k] : f->pn[k], refl_id(f->pt[k]), f->pdef && f->pdef[k] ? 1 : 0);
                buf_emit(&b, "};\n");
            }
            char ret[32] = "NULL";
            if (!is_void(f->ret)) snprintf(ret, sizeof ret, "&__c2_ty_%d", refl_id(f->ret));
            char ps[32] = "NULL";
            if (f->np) snprintf(ps, sizeof ps, "__c2_fp%d", i);
            buf_emit(&tab, "    {\"%s\", %s, %d, %s, __c2_fn%d},\n", f->name, ret, f->np, ps, i);
        }
        buf_emit(&tab, "    {NULL, NULL, 0, NULL, NULL}};\nconst c2func *__c2_functions = __c2_ftab;\n");
        buf_append(&b, tab.p, tab.len);                  /* (tab.p: the compile's own memory, freed with it) */
    }
    rtypes_done = nrtypes;
    keyops_generate();                                /* the key tables the types name, defined first */
    LexState ls = save_lex();
    int line = cur_line;
    src = b.p;
    next_token();
    while (current_token.type != TOKEN_EOF) parse_toplevel();
    restore_lex(ls);
    cur_line = line;
    cc_free(b.p);
}

/* map and set keys of a type that owns memory or compares its own way (a string): how to compare, hash,
   copy and free one.  __c2_keyops(key) is a void *[4] {equal, hash, copy, free} the compiler writes for
   the key's type (NULL: compared and hashed by its bytes).  equal: [[operator==]]; hash: T_hash(T *)
   (or (T)); copy: [[copy]]; free: [[destructor]]. */
static Type keyops_types[256];
static void keyops_name(Type t, char *out, size_t n) {
    char tn[200];
    refl_name(t, tn, sizeof tn);
    int k = fmt_to(out, n, "__c2_ko%d_", keyops_unit);
    for (const char *q = tn; *q && k < (int)n - 1; q++) out[k++] = isalnum((unsigned char)*q) ? *q : '_';
    out[k] = '\0';
}
/* t's table {equal, hash, copy, free} is generated (its name in name): when t has [[operator==]] and T_hash */
static int keyops_want(Type t, char *name, size_t n) {
    t.bcq = 0;
    const char *sn = is_struct_val(t) ? struct_name(t.sidx) : NULL;
    char hn[96];
    fmt_to(hn, sizeof hn, "%s_hash", sn ? sn : "?");
    if (!sn || !find_operator(OPR_EQ, t, t) || !find_func(hn)) return 0;
    int i = 0;
    while (i < nkeyops && !(keyops_types[i].sidx == t.sidx)) i++;
    if (i == nkeyops) { if (nkeyops >= 256) error("too many map key types"); keyops_types[nkeyops++] = t; }
    keyops_name(t, name, n);
    return 1;
}
static Val keyops_of(Type t) {
    t.bcq = 0;
    if (!is_struct_val(t) || (!dtor_for(t) && !find_operator(OPR_EQ, t, t))) return rv_op(IMM(0), ptr_to(mk_type(TY_VOID)));
    const char *sn = struct_name(t.sidx);
    char hn[96];
    fmt_to(hn, sizeof hn, "%s_hash", sn ? sn : "?");
    if (!find_operator(OPR_EQ, t, t)) error("a %s as a map or set key needs [[operator==]] (and %s)", type_name(t), hn);
    if (!find_func(hn)) error("a %s as a map or set key needs %s(%s *): its hash, as [[operator==]] compares", type_name(t), hn, sn ? sn : "T");
    char name[96];
    keyops_want(t, name, sizeof name);
    Type at = array_of(ptr_to(mk_type(TY_VOID)), 4);
    Global *g = find_global(name);
    if (!g) { g = add_global(name, at); be_bss(name, size_of(at)); }
    int a = new_temp(RK_I);
    EMIT(MOV, R(a), REF(g->name));
    return rv_reg(a, ptr_to(mk_type(TY_VOID)));
}
static void keyops_generate(void) {
    if (keyops_done >= nkeyops) return;
    Buf b = {0};
    for (int i = keyops_done; i < nkeyops; i++) {
        Type t = keyops_types[i];
        const char *ts = refl_spell(t), *sn = struct_name(t.sidx);
        char name[96];
        keyops_name(t, name, sizeof name);
        Func *h = NULL;
        char hn[96];
        fmt_to(hn, sizeof hn, "%s_hash", sn);
        h = find_func(hn);
        int hptr = h && h->np >= 1 && is_pointer(h->pt[0]);
        buf_emit(&b, "static bool %s_eq(const void *a, const void *b) { return *(%s *)a == *(%s *)b; }\n", name, ts, ts);
        buf_emit(&b, "static unsigned long long %s_h(const void *p) { return (unsigned long long)%s(%s(%s *)p); }\n", name, hn, hptr ? "" : "*", ts);
        if (copy_for(t)) buf_emit(&b, "static void %s_cp(void *d, const void *s) { %s c = %s((%s *)s); memcpy(d, &c, sizeof c); }\n", name, ts, copy_for(t)->name, ts);
        else buf_emit(&b, "static void %s_cp(void *d, const void *s) { memcpy(d, s, sizeof(%s)); }\n", name, ts);
        if (dtor_for(t)) buf_emit(&b, "static void %s_fr(void *p) { %s((%s *)p); }\n", name, dtor_for(t)->name, ts);
        buf_emit(&b, "void *%s[4] = {(void *)%s_eq, (void *)%s_h, (void *)%s_cp, %s};\n", name, name, name, name,
                 dtor_for(t) ? "" : "NULL");
        if (dtor_for(t)) {                            /* (the last initializer: written apart, to keep the line short) */
            b.len -= 3;
            buf_emit(&b, "(void *)%s_fr};\n", name);
        }
    }
    keyops_done = nkeyops;
    if (!b.len) return;
    LexState ls = save_lex();
    int line = cur_line;
    src = b.p;
    next_token();
    while (current_token.type != TOKEN_EOF) parse_toplevel();
    restore_lex(ls);
    cur_line = line;
}

static void parse_all(void) {
    owned_checked = 0;
    while (current_token.type != TOKEN_EOF) { gen_owned_all(); parse_toplevel(); }
    gen_owned_all();
    keyops_generate();
    refl_generate();
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
    dbg_on = cc->debug && mode != 'C';
    sandbox_on = cc->sandbox && mode != 'C';
    running_on = cc->safe_reload && mode != 'C';
    dbg_slot = -1;
    unwind_on = no_unwind_check = 0;
    src_locs_free();
    c_lines_on = mode == 'C';
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
#if !defined(NO_MIR) && !defined(C2_NO_CORO)
    if ((mode == 0 || mode == 'T') && cc->ctx && host_co_suspended()) {   /* their stacks return into this program */
        int n = host_co_suspended();
        cur_line = 0;
        error("can't reload while %d coroutine%s %s suspended (or running): let %s finish, or co_free() %s first",
              n, n == 1 ? "" : "s", n == 1 ? "is" : "are", n == 1 ? "it" : "them", n == 1 ? "it" : "them");
    }
#endif
#ifndef NO_MIR
    if ((mode == 0 || mode == 'T') && cc->ctx) {       /* C2_safe_reload: a script function is still running */
        const long long *running = (const long long *)ctx_get(cc->ctx, "__c2_running", NULL);
        if (running && *running > 0) {
            cur_line = 0;
            error("can't reload while %lld script function%s running (a call from the script into the host that "
                  "compiles?): compile after it returns", *running, *running == 1 ? " is" : "s are");
        }
    }
#endif
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
                       "#include <string.h>\n#include <stdlib.h>\n#include <math.h>\n"
                       "typedef int8_t i8, int8; typedef int16_t i16, int16; typedef int32_t i32, int32; typedef int64_t i64, int64;\n"
                       "typedef uint8_t u8, uint8; typedef uint16_t u16, uint16; typedef uint32_t u32, uint32; typedef uint64_t u64, uint64;\n"
                       "typedef float f32, flt32; typedef double f64, flt64;\n");
    for (int i = 0; i < cc->nlibs; i++)                /* (Windows: MSVC-built libraries, MSVC bit-fields) */
        if (!cc->libs[i].unloaded && cc->libs[i].header)   /* (#pragma comment(lib): no header) */
#ifdef _WIN32
            buf_emit(&pre, "#pragma ms_struct on\n#include \"%s\"\n#pragma ms_struct off\n", cc->libs[i].header);
#else
            buf_emit(&pre, "#include \"%s\"\n", cc->libs[i].header);
#endif
    if (cc->debug && mode != 'C') buf_emit(&pre, "extern void *__c2_dbg_top;\n");   /* C2_set_debug */
    if (sandbox_on) buf_emit(&pre, "extern int __c2_throwing;\n");   /* C2_sandbox: exit() sets it */
    if (running_on) buf_emit(&pre, "long long __c2_running;\n");     /* C2_safe_reload: script functions running */
    if (path && ends_with(path, ".c2")) buf_emit(&pre, "extern int __c2_throwing;\n");   /* try/throw */
    if (path && ends_with(path, ".c2"))                /* for (u32 c : text): one UTF-8 character */
        buf_emit(&pre, "static unsigned __c2_utf8(const char **p, const char *e) { const unsigned char *s = (const unsigned char *)*p; "
                       "unsigned c = *s++; int n = c < 0xC0 ? 0 : c < 0xE0 ? 1 : c < 0xF0 ? 2 : 3; if (n) { c &= 0x3Fu >> n; "
                       "for (int i = 0; i < n && (!e || (const char *)s < e) && (*s & 0xC0) == 0x80; i++) c = c << 6 | (*s++ & 0x3F); } "
                       "*p = (const char *)s; return c; }\n");
    if (cc->decls) buf_append(&pre, cc->decls, cc->decls_len);   /* C2_declare, C2_add_function, C2_add_variable */
    ms_struct_pragma = 0;
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
        globals_final = 1;
        for (int i = 0; i < nglobals; i++) ensure_global_storage(&globals[i]);
        globals_final = 0;
        be_module_end();
        if (mode == 'S') fputs(out_text.p, stdout);
#ifndef NO_MIR
        else if (mode == 'A') MIR_output(ctx, stdout);
        else if (mode == 'C') { c_module(mir_module, stdout); src_locs_free(); }
        else {
            MIR_module_t m = mir_module;
            if (mode == 'T') {
                MIR_scan_string(ctx, out_text.p);
                m = DLIST_TAIL(MIR_module_t, *MIR_get_module_list(ctx));
            }
            linking = cc;
            for (MIR_item_t it = DLIST_HEAD(MIR_item_t, m->items); it; it = DLIST_NEXT(MIR_item_t, it))
                if (it->item_type == MIR_import_item && !lookup_symbol(it->u.import_id)) {
                    if (cc->sandbox) { linking = NULL; int found = lookup_symbol(it->u.import_id) != NULL; linking = cc;
                        if (found) error("'%s' isn't available in the sandbox (C2_sandbox): a sandboxed script reaches C²'s own C library, the host's symbols and the libraries it loaded", it->u.import_id); }
                    error("cannot resolve external symbol '%s'", it->u.import_id);
                }
            MIR_load_module(ctx, m);
            if (cc->interp) MIR_link(ctx, MIR_set_interp_interface, import_resolver);
            else {
                MIR_gen_init(ctx);
                /* -g: at most -O1, whose code keeps every store to the call stack's frames (-O2 drops or
                   moves the ones nothing reads back, and a crash report would show garbage) */
                MIR_gen_set_optimize_level(ctx, (unsigned)(cc->debug && cc->opt_level > 1 ? 1 : cc->opt_level));
                gen_on = 1;
                MIR_link(ctx, cc->lazy ? MIR_set_lazy_gen_interface : MIR_set_gen_interface, import_resolver);
            }
            host_exiting = host_exit_code = 0;          /* a new program: an earlier sandboxed exit() is over */
            if (cc->sandbox) host_throwing = 0;
            for (MIR_item_t it = DLIST_HEAD(MIR_item_t, m->items); it; it = DLIST_NEXT(MIR_item_t, it))
                if (it->item_type == MIR_func_item && strncmp(it->u.func->name, "__c2_ginit", 10) == 0)
                    ((void (*)(void))it->addr)();     /* global initializers */
            int keep_gen = gen_on && cc->lazy;             /* lazy: functions are generated on their first call */
            if (gen_on && !keep_gen) MIR_gen_finish(ctx);
            gen_on = 0;
            linking = NULL;
            Func *mf = find_func("main");
            cc->main_np = mf && mf->defined ? mf->np : 0;
            int nng = 0;
            struct C2Global *ng = record_globals(&nng);
            if (cc->ctx) {                             /* a reload: carry state over, then tell the script */
                carry_persisted(cc, newctx, ng, nng, path);
                Func *rf = find_func("__on_reload");
                if (rf && rf->defined && rf->np == 0) {
                    int isf = 0;
                    void *fn = ctx_get(newctx, rf->name, &isf);
                    if (fn && isf) ((void (*)(void))fn)();
                }
            }
            free_globals(cc->globals, cc->nglobals);
            cc->globals = ng;
            cc->nglobals = nng;
            if (cc->keep_prev) {                       /* the host releases it (or the next compile does) */
                release_previous(cc);
                cc->prev_ctx = cc->ctx;
                cc->prev_gen_live = cc->gen_live;
                cc->ctx = NULL;
            } else release_program(cc);                /* the previous program */
            if (!cc->prev_ctx) close_unloaded_libs(cc);   /* C2_unload: nothing uses them any more */
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
    return ctx_get(cc->ctx, name, NULL);
#else
    (void)cc; (void)name;
    return NULL;
#endif
}

c2ref C2_reflected(C2 *cc, const char *path) {   /* through the script's own reflect_global() */
    c2ref none = {0};
    const void *const *globals = (const void *const *)C2_get(cc, "__c2_globals");
    c2ref (*find)(const void *, const char *) = (c2ref (*)(const void *, const char *))C2_get(cc, "reflect_global");
    if (!globals || !find || !path) return none;
    return find(*globals, path);
}

void C2_keep_previous(C2 *cc, int on) { cc->keep_prev = on; }

void *C2_get_previous(C2 *cc, const char *name) {
#ifndef NO_MIR
    return ctx_get(cc->prev_ctx, name, NULL);
#else
    (void)cc; (void)name;
    return NULL;
#endif
}

void C2_release_previous(C2 *cc) {
#ifndef NO_MIR
    release_previous(cc);
    close_unloaded_libs(cc);
#else
    (void)cc;
#endif
}

int C2_run_main(C2 *cc, int argc, char **argv) {
    void *m = C2_get(cc, "main");
    if (!m) return -1;
    int ret = cc->main_np >= 2 ? (int)((int64_t (*)(int64_t, char **))m)(argc, argv)
                               : (int)((int64_t (*)(void))m)();
    fflush(stdout);
    return host_exiting ? host_exit_code : ret;       /* a sandboxed exit(n): n */
}

int C2_exited(C2 *cc, int *code) {
    (void)cc;
    int how = host_exiting;
    if (code) *code = how ? host_exit_code : 0;
    host_exiting = host_exit_code = 0;
    if (how) host_throwing = 0;
    return how;
}

void C2_delete(C2 *cc) {
    if (!cc) return;
#ifndef NO_MIR
    release_program(cc);
    release_previous(cc);
#endif
    free_globals(cc->globals, cc->nglobals);
    free(cc->decls);
    for (int i = 0; i < cc->ntyped; i++) free(cc->typed[i]);
    free(cc->typed);
    for (int i = 0; i < cc->nlibs; i++) cc->libs[i].unloaded = 1;
    close_unloaded_libs(cc);
    for (int i = 0; i < cc->ndefs; i++) { free(cc->defs[i][0]); free(cc->defs[i][1]); }
    for (int i = 0; i < cc->nincs; i++) free(cc->incs[i]);
    for (int i = 0; i < cc->nsyms; i++) free(cc->syms[i].name);
    free(cc->syms);
    free(cc->errors);
    free(cc);
}

/* a library by name: SDL3 is SDL3.dll on Windows, libSDL3.dylib / SDL3.dylib on macOS, libSDL3.so /
   SDL3.so elsewhere, from the system's search, then the current folder; path gets the last one tried */
static void *lib_open(const char *library, char *path, size_t path_size) {
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
    for (int here = 0; !h && here < 2 && strchr(base, '.'); here++) {   /* a file name already: libSDL3.so.0 */
        if (here && base != library) break;
        snprintf(path, path_size, "%s%s", here ? "./" : "", library);
#ifdef _WIN32
        h = LoadLibraryA(path);
#else
        h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
#endif
    }
    for (int here = 0; !h && here < 2; here++) {       /* the system's search, then the current directory */
      if (here && base != library) break;
      for (size_t e = 0; !h && e < sizeof exts / sizeof exts[0]; e++) {
        for (int prefix = 0; !h && prefix < 2; prefix++) {
#ifdef _WIN32
            if (prefix) break;
#endif
            snprintf(path, path_size, "%s%.*s%s%s%s", here ? "./" : "", (int)(base - library), library,
                     prefix ? "lib" : "", base, exts[e]);
#ifdef _WIN32
            h = LoadLibraryA(path);
#else
            h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
#endif
        }
      }
    }
    return h;
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
    char path[1100];
    void *h = lib_open(library, path, sizeof path);
    if (!h) { snprintf(msg, sizeof msg, "C2_load: cannot load library '%s' (tried %s and the like)", library, path); return msg; }
    for (int i = 0; i < cc->nlibs; i++)                /* loading a header again replaces it */
        if (!cc->libs[i].unloaded && cc->libs[i].header && strcmp(cc->libs[i].header, full) == 0) cc->libs[i].unloaded = 1;
    cc->libs[cc->nlibs].header = str_dup(full);
    cc->libs[cc->nlibs].path = str_dup(path);
    cc->libs[cc->nlibs].handle = h;
    cc->libs[cc->nlibs++].unloaded = 0;
    return "";
}

/* #pragma comment(lib, "SDL3") in a script: SDL3's functions resolve for it (and later compiles), as with
   C2_load but without a header: the script #includes the declarations it wants (LuaJIT's ffi style) */
static const char *script_library(const char *name) {
    static char msg[1300];
    C2 *cc = compiling;
    if (!cc) return "";
    char path[1100];
    void *h = lib_open(name, path, sizeof path);
    if (!h) { snprintf(msg, sizeof msg, "#pragma comment(lib): cannot load library '%s' (not found on the system's library path nor in the current folder)", name); return msg; }
    for (int i = 0; i < cc->nlibs; i++)
        if (!cc->libs[i].unloaded && !strcmp(cc->libs[i].path, path)) { lib_close(h); return ""; }   /* loaded already */
    if (cc->nlibs >= 64) { lib_close(h); return "#pragma comment(lib): too many libraries"; }
    cc->libs[cc->nlibs].header = NULL;
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
        if (!cc->libs[i].unloaded && cc->libs[i].header && strcmp(cc->libs[i].header, full) == 0) { cc->libs[i].unloaded = 1; return ""; }
    return "C2_unload: that header was not loaded";
}

/* ============================================================ command line */
#ifndef C2_NO_MAIN
/* the folder of the running executable: the CLI searches it for <runtime/...> (the C² runtime) */
static void exe_dir(char *out, size_t n, const char *argv0) {
    out[0] = '\0';
#if defined(_WIN32)
    unsigned long k = GetModuleFileNameA(NULL, out, (unsigned long)n);
    if (k == 0 || k >= n) out[0] = '\0';
#elif defined(__APPLE__)
    unsigned sz = (unsigned)n;
    if (_NSGetExecutablePath(out, &sz) != 0) out[0] = '\0';
#else
    long k = (long)readlink("/proc/self/exe", out, n - 1);
    out[k > 0 ? k : 0] = '\0';
#endif
    if (!out[0]) snprintf(out, n, "%s", argv0);
    char *slash = NULL;
    for (char *p = out; *p; p++) if (*p == '/' || *p == '\\') slash = p;
    if (slash) *slash = '\0';
    else snprintf(out, n, ".");
}

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
        else if (strcmp(a, "-prefix") == 0 && argi + 1 < argc) snprintf(c_prefix, sizeof c_prefix, "%s", argv[++argi]);
        else if (strncmp(a, "-prefix=", 8) == 0) snprintf(c_prefix, sizeof c_prefix, "%s", a + 8);
#endif
#ifndef NO_MIR
        else if (strcmp(a, "-Crt") == 0 || strcmp(a, "-Cruntime") == 0) { c_print_runtime(stdout); return 0; }   /* the same */
#endif
        else if (strcmp(a, "-i") == 0) C2_use_interpreter(cc, 1);
        else if (a[0] == '-' && a[1] == 'O' && a[2] >= '0' && a[2] <= '3' && !a[3]) C2_set_opt_level(cc, a[2] - '0');
        else if (strcmp(a, "-lazy") == 0) C2_set_lazy(cc, 1);
        else if (strcmp(a, "-sandbox") == 0) C2_sandbox(cc, 1);
        else if (strcmp(a, "-g") == 0) { C2_set_debug(cc, 1); C2_crash_handler(1); }
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
                        "  -C translate to C (compile it with the output of -Crt, or -Cruntime: c2_rt.c)\n"
                        "  -prefix ui_  with -C: name what the C defines ui_... (ui_init, ui_main, no main()), so scripts link together\n"
                        "  -O0..-O3 MIR optimization level (default -O2)   -lazy generate functions on first call\n"
                        "  -sandbox no deleting, renaming or writing files, starting programs or loading libraries (C2_sandbox)\n"
                        "  -g keep a script call stack: crashes print it (with -C: #line, for a C debugger)\n"
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
    char here[4096];
    exe_dir(here, sizeof here, argv[0]);
    C2_include_dir(cc, here);                          /* last: <runtime/string.h2>, <runtime/array.h2> */
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
