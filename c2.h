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
 *   C2_compile_file(c2, "game.c");   // hot reload: on success the old code is released
 *   C2_delete(c2);
 *
 * Build: add c2.c to your program with C2_NO_MAIN defined and link MIR (see CMakeLists.txt).
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

/* compile and link a script; its global initializers run before this returns.
   Returns "" on success, or the error messages (never NULL; valid until the next compile or
   C2_delete).  On success, pointers from C2_get for the previous program become invalid; on
   failure the previous program stays loaded.  filename is used in messages and for relative
   #includes; a name ending in .c2 turns the C² extensions on. */
const char *C2_compile(C2 *c2, const char *filename, const char *source);
const char *C2_compile_file(C2 *c2, const char *path);

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
