/*
 * c2.h — compile C scripts at run time and call them (JIT through MIR).
 *
 *   C2 *c2 = C2_new();
 *   C2_add_symbol(c2, "draw_sprite", (void *)draw_sprite);    // script declares and calls it
 *   const char *err = C2_compile_file(c2, "game.c");
 *   if (*err) puts(err);
 *   void (*update)(float) = (void (*)(float))C2_get(c2, "update");
 *   update(dt);
 *   ...
 *   C2_compile_file(c2, "game.c");   // hot reload: on success the old code is released (see below)
 *   C2_delete(c2);
 *
 * Build: add c2.c to your program with C2_NO_MAIN defined and link MIR (see docs/CMakeLists.txt).
 * Script functions follow the platform C ABI, structs by value included, so the host calls them
 * through ordinary function pointers and scripts call host functions directly.
 * Not thread-safe: compile from one thread at a time (any number of C2 instances).
 */
#ifndef C2_H
#define C2_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct C2 C2;

C2 *C2_new(void);
void C2_delete(C2 *c2);

/* options for the following compiles: #define name value (value NULL = "1"), #include "..." dirs */
void C2_define(C2 *c2, const char *name, const char *value);
void C2_include_dir(C2 *c2, const char *dir);

/* a host function or variable the script can use by name (declare it in the script, e.g.
   `void draw_sprite(int id, float x, float y);` or `extern int score;`).  Host symbols are
   looked up before the built-in C library. */
void C2_add_symbol(C2 *c2, const char *name, void *addr);

/* the same, declared once by the host so scripts just use it (a script that declares it differently
   gets an error).  Returns "" or an error message.  C2_declare adds declarations (the types these
   use, say) that every later compile sees before the script.
       C2_declare(c2, "typedef struct { float x, y, z; } Vec3;");
       C2_add_function(c2, "Vec3 vec3_add(Vec3 a, Vec3 b);", (void *)vec3_add);
       C2_add_variable(c2, "int score;", &score);              // `extern` is implied */
void C2_declare(C2 *c2, const char *declarations);
const char *C2_add_function(C2 *c2, const char *declaration, void *addr);
const char *C2_add_variable(C2 *c2, const char *declaration, void *addr);

/* compile and link a script; its global initializers run before this returns.
   Returns "" on success, or the error messages (never NULL; valid until the next compile or
   C2_delete).  On success, pointers from C2_get for the previous program become invalid; on
   failure the previous program stays loaded.  filename is used in messages and for relative
   #includes; a name ending in .c2 turns the C² extensions on. */
const char *C2_compile(C2 *c2, const char *filename, const char *source);
const char *C2_compile_file(C2 *c2, const char *path);

/* Hot reload.  Compiling again while a program is loaded replaces it:
   - A failed compile changes nothing: the old program stays loaded and working.
   - A successful one releases the old program (unless C2_keep_previous): every address it gave out
     becomes invalid -- its functions, globals, string literals, static locals and compound literals.
     The host must call C2_get again for whatever it keeps (callbacks, globals).  Memory the script
     got from malloc is the process's, not the program's: it stays valid.
   - Don't compile while a script function is running (a callback from the script into the host
     that recompiles): its code would be released under it.
   - A global marked [[persist]] keeps its value: after the new program's initializers run, it gets
     the old value when the old program had a global of the same name and type (else it keeps its
     initializer, with a warning when only the type changed).  A persisted function pointer is moved
     to the new program's function of the same name (NULL, with a warning, when there is none).
     When its type changed and both programs import the runtime's reflection (#import "runtime"), it
     moves field by field instead: fields match by name (or [[rename]]), numbers convert (and meet a
     new [[clamp]]), an enum keeps its value's name, arrays keep their items (an array(T) is rebuilt
     when T changed), strings, maps and other values of an unchanged type move as they are, function
     pointers inside move as above.  What can't move (a string that became a number, a map whose types
     changed) keeps its initializer, with a warning naming the field ('hero.bag[2].name'); fields the
     new type dropped are freed.
   - Then, if the new program defines `void __on_reload(void)`, it runs (not on the first compile).
   - A compile is refused while a script coroutine (runtime/coro) is suspended: its stack returns into the
     loaded program.  Let it finish or co_free() it first. */

/* keep the previous program after a successful compile, until C2_release_previous (or the next
   successful compile, or C2_delete), so the host can copy what it needs out of it first */
void  C2_keep_previous(C2 *c2, int on);
void *C2_get_previous(C2 *c2, const char *name);   /* like C2_get, in the previous program; NULL if none */
void  C2_release_previous(C2 *c2);

/* all diagnostics of the last compile, warnings included (also after a success); "" if none */
const char *C2_errors(C2 *c2);

/* address of a function or global variable of the loaded program, NULL if there is none */
void *C2_get(C2 *c2, const char *name);

/* run the next compiles with MIR's interpreter instead of its machine-code generator: no
   optimizing compiler, faster to start, slower to run.  (It still needs a few small generated call
   stubs; for targets that allow no generated code at all, translate scripts to C: c2 -C.) */
void C2_use_interpreter(C2 *c2, int on);

/* code generation options for the next compiles: MIR's optimization level 0..3 (default 2; 0-1
   compile faster), and lazy generation (each function is compiled on its first call: a large
   script loads faster, the first call of each function pays for it) */
void C2_set_opt_level(C2 *c2, int level);
void C2_set_lazy(C2 *c2, int on);

/* sandbox the next compiles (off by default; c2 -sandbox): the C library functions that delete, rename or
   write files, start programs or load libraries (remove, unlink, rename, fopen for writing, system,
   popen, exec*, dlopen, dlsym, MoveFileExA...) fail when called (-1, NULL or 0, errno EPERM), and
   nothing else is found by name in the C library or the OS: a script reaches C²'s own C library (see
   README), the host's symbols (C2_add_symbol, C2_add_function: the host decides) and the libraries it
   loaded (C2_load).  A guard against a script's mistakes and casual misuse, not a security boundary:
   C code can still write anywhere in the process's memory.  -C output links through c2_rt(), which
   isn't sandboxed.
   exit(), _Exit(), quick_exit() and abort() don't end the host: the script leaves every function it is in,
   through their defers (as for a throw no try catches), and the host's call returns
   (its result means nothing then).  C2_exited() says so: 1 after exit(code), 2 after abort(), 0 if not;
   it clears that state.  Until then, or the next compile, a call into the script returns at its first
   call to another function.  C2_run_main returns exit's code.  atexit() is refused. */
void C2_sandbox(C2 *c2, int on);
int  C2_exited(C2 *c2, int *code);

/* refuse to compile while a script function is running (off by default): a script that calls the host,
   which compiles (a console command, an editor button), would return into released code.  With it on,
   the next compiles' functions count themselves in and out (a few instructions a call), and a compile
   while the count isn't 0 fails with an error, changing nothing.  longjmp out of script code skips the
   count down: don't, with this on. */
void C2_safe_reload(C2 *c2, int on);

/* debugging.  C2_set_debug(c2, 1): the next compiles keep a script call stack (one store per statement,
   a few per call).  C2_backtrace() returns it, innermost first ("  update() at game.c2:42\n" per frame;
   "" outside scripts), e.g. from a host function a script called.  C2_crash_handler(1) prints it to
   stderr when the program crashes (bad memory access, division by zero, abort...), then crashes as
   before.  For stepping and breakpoints, `c2 -C` output carries #line directives: a C debugger shows
   the .c2 source. */
void C2_set_debug(C2 *c2, int on);
const char *C2_backtrace(void);
void C2_crash_handler(int on);

/* ahead of time: `c2 -C game.c2 > game_c2.c` turns a script into C.  Compile that with your C compiler
   and link it here: it finds its C library, coroutines and try/throw through c2_rt(), which c2.c
   defines (no c2_rt.c needed; that one, from c2 -Crt, is for a program without C² in it).  Several
   scripts: `c2 -C -prefix ui_ ui.c2` names everything that file defines ui_... (call ui_init() first;
   the script's main is ui_main), so each can carry its own copy of the runtime without clashing. */
void *c2_rt(const char *name);

/* reflection: the tables a script's reflect(T) points to (runtime/reflect.c2), for a host handed one,
   e.g. by a script function that returns reflect(Player).  The same layout as the script's. */
typedef struct c2type c2type;
typedef struct c2field {
    const char *name;
    const c2type *type;        /* NULL: a [[skip]] type */
    int offset, bits, bitpos;  /* bytes; a bit-field's width and position in its unit (0 bits: none) */
    int flags;                 /* C2_SKIP, C2_CLAMP */
    const char *key;           /* [[rename("hp")]]: the name saves use; NULL: name */
    const char *comment;       /* [[comment("...")]], or NULL */
    double min, max;           /* [[clamp(min, max)]], when flags has C2_CLAMP */
    int maxlen;                /* [[clamp(N)]]: characters; 0: none */
} c2field;
typedef struct c2enum { const char *name; long long value; } c2enum;
struct c2type {
    const char *name;          /* "Player", "int", "float *", "char [8]", "array(int)" */
    int kind, size, align;     /* enum c2kind */
    const c2type *of, *val;    /* pointee / element / enum's type / key; a map's value */
    int count;                 /* array length, fields, enum values */
    const c2field *fields;
    const c2enum *values;
    void (*free)(void *);                       /* [[destructor]], or NULL */
    void (*copy)(void *dst, const void *src);   /* [[copy]], or NULL */
    void *const *keyops;       /* as a map key ([[operator==]] and T_hash): {equal, hash, copy, free}; or NULL */
};
enum c2kind { C2_VOID, C2_BOOL, C2_CHAR, C2_INT, C2_UINT, C2_FLOAT, C2_ENUM, C2_PTR, C2_ARRAY, C2_FUNC, C2_STRUCT,
              C2_UNION, C2_STRING, C2_DYNARRAY, C2_MAP, C2_SET };
enum { C2_SKIP = 1, C2_CLAMP = 2 };
/* a global's part, found by name: C2_reflected(c2, "player.pos.x"); type NULL: not found.  field: the
   field it is, or is an item of; bits, bitpos: a bit-field's (p is its storage unit).  Paths as the
   script's global(): "a.b", "/a/b", "bag[2]", "scores[Ada]" (a map's key); pointers to structs are
   followed.  Needs the script to #import "runtime" (or "runtime/reflect"): its globals are listed then. */
typedef struct c2ref { const c2type *type; void *p; const c2field *field; int bits, bitpos; } c2ref;   /* a script's any */
/* the script's globals, as reflect_globals() lists them: *(const c2global **)C2_get(c2, "__c2_globals"),
   {NULL} at the end */
typedef struct c2global { const char *name; const c2type *type; void *addr; } c2global;
/* the script's functions, as reflect_functions() lists them: *(const c2func **)C2_get(c2, "__c2_functions"),
   {NULL} at the end.  call(args, result): args[i] points to parameter i's value (NULL for an optional
   one: its default), result to room for the result (NULL if void) */
typedef struct c2param { const char *name; const c2type *type; int optional; } c2param;
typedef struct c2func { const char *name; const c2type *ret; int count; const c2param *params;
                        void (*call)(void **args, void *result); } c2func;
c2ref C2_reflected(C2 *c2, const char *path);

/* bind a C library: scripts compiled from now on see the header's declarations (as if every script
   #included it first) and its functions resolve from the library.  library has no extension:
   "SDL2" loads SDL2.dll on Windows, libSDL2.dylib / SDL2.dylib on macOS, libSDL2.so / SDL2.so
   elsewhere (a path may come first: "libs/SDL2").  Returns "" or an error message. */
const char *C2_load(C2 *c2, const char *header, const char *library);

/* stop offering a C2_load'ed header to the next compiles.  Its library is closed when a new
   program replaces the current one (which may still call into it), or by C2_delete. */
const char *C2_unload(C2 *c2, const char *header);

/* call the program's main(argc, argv); returns its result (-1 if there is no main) */
int  C2_run_main(C2 *c2, int argc, char **argv);

#ifdef __cplusplus
}
#endif
#endif
