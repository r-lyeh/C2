## C²

I have always needed a C library that could compile C scripts on the fly; primarily for use within game scripts.

In this repository, I am experimenting with something similar to `stb_compiler.h`. It is capable of parsing a significant amount of C code, includes a preprocessor, is sufficiently fast and compact, and relies on external dependencies for code generation and JIT compilation; making its source code essentially platform-independent.

[TinyCC](https://bellard.org/tcc) is probably the closest existing solution, but it is not quite what I was looking for; hence the creation of this repository.

While I was at it, I also added another optional language on top of the C compiler. You can use C at any time, or use C², which stays closer to C than C++ while adding objects, reflection, serialization, function overloading, struct operators, and more. See **[LANGUAGE.md](LANGUAGE.md)** for details.

## Usage
```
c2 file.c [args...]       compile, JIT and run main(argc, argv); its result is the exit code
c2 -S file.c              print MIR text instead of running
c2 -E file.c              print the preprocessed source
c2 -i file.c [args...]    the same with MIR's interpreter (no machine-code generator)
c2 -C file.c > out.c      translate to C (no JIT at all); compile it with c2_rt.c (c2 -Crt, or -Cruntime)
c2 -C -prefix ui_ ui.c2   the same, with what it defines named ui_... (ui_init, ui_main): scripts link together
c2 -O0..-O3 file.c        MIR optimization level (default -O2; lower compiles faster)
c2 -lazy file.c           generate each function's code on its first call (faster start)
c2 -g file.c              keep a script call stack: a crash prints where each function was
c2 -sandbox file.c        no deleting, renaming or writing files, starting programs or loading libraries
c2 -T / -A file.c         debug: run through the MIR text path / print what the C API built
options: -Idir, -Dname[=value]
```

## Embedding (c2.h)
```c
#include "c2.h"
typedef struct { float x, y, z; } Vec3;
Vec3 vec3_add(Vec3 a, Vec3 b);                       /* a host function */

C2 *c2 = C2_new();
C2_declare(c2, "typedef struct { float x, y, z; } Vec3;");          /* types every script sees */
C2_add_function(c2, "Vec3 vec3_add(Vec3 a, Vec3 b);", (void *)vec3_add);   /* scripts just call it */
C2_add_variable(c2, "int score;", &score);                              /* ...and use score */
const char *err = C2_compile_file(c2, "game.c");  /* "" on success, else the errors */
if (*err) puts(err);
Vec3 (*update)(float) = (Vec3 (*)(float))C2_get(c2, "update");
update(dt);
C2_compile_file(c2, "game.c");                      /* hot reload; on error the old code stays */
C2_load(c2, "sdl.h", "SDL2");                       /* bind a C library: header + SDL2.dll/.so/.dylib */
C2_delete(c2);
```

- Compile `c2.c` with `C2_NO_MAIN` into your program and link `3rd/mir.c` and `3rd/mir-gen.c` (MIR).
- `C2_compile(c2, filename, source)` and `C2_compile_file()` return `""` on success, or the error messages. 
- `C2_errors()` has all diagnostics of the last compile, warnings included.
- `C2_add_function(c2, "prototype;", fn)` and `C2_add_variable(c2, "type name;", &var)` declare a host
  symbol once for every script, which then uses it without repeating the prototype (and gets an error if
  it declares it differently); `C2_declare` adds the types they need.  `C2_add_symbol(c2, name, addr)`
  is the untyped form: the script declares the symbol itself.
- `C2_use_interpreter(c2, 1)` runs scripts with MIR's interpreter instead
- `C2_set_opt_level()` tunes code generation
- `C2_set_lazy()`  has a faster boot time but there is a penalty for each function on its first call.
- `C2_sandbox(c2, 1)` (off by default) makes the next compiles' scripts unable to delete, rename or write
  files, start programs or load libraries: `remove`, `unlink`, `rename`, `fopen` for writing, `system`,
  `popen`, `exec*`, `dlopen`, `MoveFileExA`... fail when called (errno `EPERM`; a save just returns false),
  and nothing else is looked up by name in the C library or the OS (an error at compile time).  The
  host's own symbols and the libraries it loaded still work.  `exit()` and `abort()` don't end the host:
  the script returns from every function (defers run) and `C2_exited(c2, &code)` says it exited.  It guards against a script's mistakes, not
  against hostile code: C can still write anywhere in the process.
- Debugging: `C2_set_debug(c2, 1)` makes the next compiles keep a script call stack (a store per
  statement and a few per call, variables kept in memory, at most `-O1`: up to twice as slow on
  call-heavy code; see LANGUAGE.md); crash reports show each frame's parameters and variables.  `C2_backtrace()` returns it, innermost
  first (`  update() at game.c2:42`), say from a host function a script called; `C2_crash_handler(1)`
  prints it when the program crashes (a bad pointer, a division by zero, `abort()`...), then lets it crash
  as before.  `c2 -g` does both.  Coroutines keep their own stacks.  For breakpoints and stepping, `c2 -C`
  writes `#line` directives: gdb, lldb or Visual Studio then show the `.c2` lines of the generated C.
- Script functions use the platform C calling convention, structs by value included, so both sides call each other through plain function
pointers.
- `C2_load(c2, header, library)` binds a C library: scripts compiled afterwards see the header's
  declarations (as if they `#include`d it) and its functions resolve from the library.  `library` has no
  extension: `"SDL2"` loads `SDL2.dll` on Windows, `libSDL2.dylib` or `SDL2.dylib` on macOS, `libSDL2.so`
  or `SDL2.so` elsewhere, from the system's search path or the current directory (a path may come
  first: `"libs/SDL2"`).  Common header decorations (`__declspec`, `__attribute__`, `__cdecl`...) are
  ignored.  On Windows mind `long`: it is 64-bit in C², 32-bit in MSVC-built libraries.
  `C2_unload(c2, header)` drops it from the next compiles; the library is closed once a new program
  replaced the current one.  The command line has `-load header.h,library`.  A script can bind a library
  itself, LuaJIT's ffi style: `#include` the declarations, then `#pragma comment(lib, "SDL3")` (MSVC's
  spelling; the same search as `C2_load`, or an exact file name: `"libSDL3.so.0"`).  `c2 -C` output links
  it as any C program would.
- Compiler is currently not thread-safe, so do not share compiler contexts between threads.
- Hot reload: compiling again replaces the program.  A failed compile changes nothing.  A successful
  one invalidates every address the old program gave out (functions, globals, string literals, static
  data): `C2_get` again what you keep; memory the script `malloc`ed stays valid.  Don't recompile from
  inside a script call.  `C2_keep_previous(c2, 1)` keeps the old program until `C2_release_previous`, so
  the host can copy from it (`C2_get_previous`).  In the script, `[[persist]] int score;` keeps a global's
  value across reloads (a persisted function pointer moves to the new function of the same name), and
  `void __on_reload(void)`, if defined, runs after each reload.  When its type changes and the script
  imports the runtime, a persisted struct moves field by field: fields match by name, numbers convert,
  enums keep their value's name, arrays, maps and sets keep their items, strings move; a field that
  can't starts again from its initializer (with a warning), one the new type dropped is freed.
  `C2_safe_reload(c2, 1)` refuses a compile while a script function is running (a script that calls
  the host, which compiles).  Without
  the runtime, a changed type starts again.  Details in `c2.h`.

## Without JIT
Some platforms (iOS, game consoles) don't let a program generate machine code.  Two fallbacks:

- **Interpreter** (`c2 -i`, `C2_use_interpreter`): no optimizing generator, quicker to start,
  roughly 10x slower to run.  MIR still creates a few small call stubs, so this is for platforms where
  MIR's generator is missing or startup matters, not for ones that forbid generated code entirely.
- **C output** (`c2 -C script.c > script_gen.c`, plus `c2 -Crt > c2_rt.c` once): portable C to
  build with the platform's own compiler.  Script functions and globals keep their names; call
  `c2_init()` before using them (it runs global initializers); a script `main` becomes the program's
  `main`.  Run c2 on a machine with the target's ABI (same CPU family and OS conventions): struct
  passing and `va_list` follow the ABI c2 was built for.  Compiled with `gcc -O2`, the C output runs
  between 1.5x slower and 2x faster than the JIT, depending on the program.
  The runtime it needs is one function, `c2_rt(name)`, from either of two places: `c2_rt.c`
  (`c2 -Crt`, about 90 KB in the program) for a program without C² in it, or C² itself built as a
  library (`C2_NO_MAIN`): a game that embeds C² can compile its scripts ahead of time and link them
  in with nothing else (`tests/c2-abi/aot_lib.c` shows the build; the c2-abi suite runs it).
  `c2 -C -prefix ui_ ui.c2` renames what the file defines to `ui_...` (`ui_init`, `ui_main`, and no C
  `main`), so several scripts, each with its own copy of the runtime parts it uses, link into one
  program.

## Features
- Compiler identifies as `__C2__`, which is always defined.
- `.c` files are compiled as C; `.c2` files also get the C² additions.
- C² compiles most of C89, C99 and C11, and a few C23 features; listed as follows.

### Supported
- C89: types: `char`, `short`, `int`, `long` (signed and unsigned), `float`, `double`, `void`, `enum`, pointers, function pointers, multi-dimensional arrays
- C89: `struct` / `union`: by value, nested, bit-fields; `typedef`; `const`, `volatile`; `static`, `extern`, `register`
- C89: every operator, with C precedence; `if`, `switch` (jump tables when dense), `for`, `while`, `do`, `break`, `continue`, `goto`, `return`; variadic functions; static locals; recursion
- C89: multi-character constants, up to 4 characters, packed in source order on every target: `'12'` == 0x3132, `'1234'` == 0x31323334
- C89: trigraphs (`??=` is `#`...) in `.c` files; `.c2` files follow C23, which removed them
- C89: K&R function definitions (`int max(a, b) int a, b; { ... }`; undeclared parameters are `int`)
- C89: wide literals `L"..."` / `L'x'` and `wchar_t` (as in the host C library: 32-bit elsewhere, 16-bit
  UTF-16 on Windows); `wchar.h`: the `wcs*` / `wmem*` string functions and `wcstod`/`wcstoll`/`wcstoull`
- C89: implicit `int` (`static x;`) and implicit function declarations (they return `int`: wrong for anything returning a pointer, `long` or `double`, so include the header)
- C89: preprocessor: `_Pragma("...")` (C99) and digraphs `<: :> <% %> %: %:%:` (C95); `#define` (object- and function-like, `#`, `##`), `#undef`, `#if` / `#ifdef` / `#ifndef` /
  `#elif` / `#else` / `#endif` with `defined()`, `#include` (`"file"` next to the includer, then `-I`
  dirs), `#error`, `#line`, `#pragma` (`once`; others ignored), `__FILE__`, `__LINE__`, `__DATE__`, `__TIME__`
- C89: headers, built in and mapped directly onto the host C runtime: `assert ctype errno float limits locale math signal stdarg stddef stdio stdlib string time`, and the multibyte functions (`mblen mbtowc wctomb mbstowcs wcstombs`).  `malloc`/`free` are the host's own, so memory passes freely between host and scripts.
- C99: `long long`, `_Bool` (`stdbool.h`), `inline`, `restrict`, `//` comments, `__func__`
- C99: declarations anywhere, including in `for (int i = 0; ...)`
- C99: designated initializers, compound literals, flexible array members
- C99: variable-length arrays (outermost dimension); hex floats; `\u` / `\U` escapes in strings (UTF-8)
- C99: variadic macros (`__VA_ARGS__`)
- C99: headers: `stdbool stdint inttypes`; `snprintf`, `isnan`/`isinf`/`isfinite`/`fpclassify` and the `math.h` float variants (`sinhf`, `lrintf`, `fmaf`...)
- C11: `_Static_assert`, `_Generic`, `_Alignof`, `_Noreturn` (`stdnoreturn.h`), `stdalign.h`
- C11: `_Alignas` on struct members (accepted on variables, but ignored there)
- C11: anonymous `struct` / `union` members; `u8"..."` strings
- C11: `_Thread_local` is accepted, with one copy (scripts are single-threaded)
- C23: preprocessor: `#embed` (with `limit`, `prefix`, `suffix`, `if_empty`), `__has_include()`, `__has_embed()`, `#warning`
- C23: digit separators (`1'000'000`), binary literals (`0b101`)
- C23: `bool` / `true` / `false` as keywords; `#elifdef` / `#elifndef`; `__VA_OPT__`
- C23: enums with a fixed underlying type (`enum E : unsigned char`), and enums with values beyond `int` (their type is the first of `unsigned`, `long long` that fits them all)
- C23: `constexpr` (an integer one is a named constant, usable in array sizes and `case` labels); `[[attributes]]`: `[[nodiscard]]` (warns when a call's result is thrown away) and `[[deprecated("why")]]` (warns at each call) work; the others are accepted and ignored, since C² has no warnings for them to silence; several can share one list, as C23 allows: `[[export, operator+=]]`; `alignas` / `alignof` / `thread_local` as keywords
- C23: `typeof` / `typeof_unqual` (also `__typeof__`), `auto x = expr;` type inference, `u8'x'` character literals (`unsigned char`)
- C23: `nullptr`, `static_assert` (message optional), empty initializers (`= {}`), unnamed parameters in definitions, labels before declarations
- C2y: `_Countof(array)` (`countof` with `<stdcountof.h>`): the element count of an array (a pointer is an error; a variable-length array's is computed at run time)
- defer TS (ISO/IEC TS 25755): `_Defer stmt;` (`defer` with `<stddefer.h>`) runs `stmt` when its block is left (closing brace, `return`, `break`, `continue`), latest first; the return value is taken before.  Not supported in functions with `goto`/labels; no jumps out of a deferred statement.
- GNU: statement expressions `({ int t = f(); t * t; })`, the value of the last expression statement
- GNU/clang: `__builtin_add_overflow` / `_sub_` / `_mul_` (exact result, `1` if it doesn't fit the result's type); in `#if`, `__has_builtin`, `__has_attribute`, `__has_c_attribute` (what C² supports), `__has_feature` / `__has_extension` (0)

### Missing from standard C
- C89: `long double` as its own type (it is `double`; `1.0L` is a `double`)
- C89: `#include` of system headers from disk: only the built-in ones exist (`<other.h>` is ignored with a note)
- C89: `setjmp.h`: `setjmp` / `longjmp`
- C89: `locale.h`: only `setlocale` and the `LC_*` constants (no `localeconv`)
- C95: wide character I/O and classification (`wprintf`, `fgetws`, `wctype.h`...); `printf("%ls")` works
- C99: `_Complex` / `_Imaginary`; headers `complex.h fenv.h tgmath.h wctype.h`
- C99: pointers to variable-length arrays (`int (*p)[n]`); only the outermost dimension may be variable
- C99: non-ASCII identifiers (UCNs or UTF-8 in names)
- C11: `aligned_alloc`
- C11: `_Atomic` (`stdatomic.h`), `threads.h`
- C11: `u"..."` (16-bit), `U"..."` (32-bit), `uchar.h`
- C23: `_BitInt`; taking the address of an integer `constexpr`
- C23: `char8_t`

### Differences from standard C
- Reading a local before setting it warns in `.c` files (`'var' is used uninitialized`).
- Multi-character constants pack their characters deterministically, in source order: `'12' == 0x3132`, `'1234' == 0x31323334`.
- Self tail calls (`return f(...);` inside `f`) become loops, so tail recursion of any depth runs in
  constant stack.  When that can't be done (a local's address is taken, a variable-length array is in
  scope, `...`, struct parameters or results) the call stays recursive and a warning says why.  With
  hooks (below) every call stays a call.  `[[musttail]] return f(...);` turns that warning into an error.
- `long` is 64-bit on every platform, Windows included.  On Windows, literal formats given to the
  `printf` / `scanf` families are fixed while compiling (`%ld` becomes `%lld`), so `%ld` works everywhere.
- Bit-fields use gcc's layout on every platform.  After `#pragma ms_struct on` (as in gcc and clang),
  structs get MSVC's layout, for sharing them with MSVC-built code; headers mounted with `C2_load` get it
  automatically on Windows.
- Structs passed through `...` to C functions (`printf("%g", s)`) follow the platform ABI.
- Hooks: when a script defines `void __on_enter(const char *func, const char *file, int line)` and/or
  `__on_leave(...)` (or the host provides them with `C2_add_symbol`), every other function calls them on
  entry and before each return: profilers, script call stacks, tracing.  Without them nothing is emitted.
- Extra: `<dlfcn.h>` on Windows too (LoadLibrary/GetProcAddress); `strdup` in `<string.h>`.
- On Windows, by-value structs of 3, 5, 6 or 7 bytes to/from external functions are not supported
  (MIR passes them in a register, MSVC by reference).
- One translation unit per compile: no object files, no linking of several `.c` files, no debug info, no
  inline assembly.  Native executables only through `c2 -C` and another C compiler.

### New C² additions (.c2 files)
The C² language, with examples, is in **[LANGUAGE.md](LANGUAGE.md)**, and one feature per file in
[tour/](../tour/) (execute `make tour` to visualize all of them).  In short:

- C made convenient: the common headers always included, `f()` meaning `f(void)`, `i32`-style type
  names, struct names as types, `.` on pointers, zeroed locals (`[[unclear]]` to opt out),
  `[[unroll]]` loops, 8-character constants, `countof`, exact array parameters (`int v[3]`), `defer`, nested functions,
  one loop for everything: `for (int i : 0..n)`, `for (u32 c : text)`, `for (Item *it : bag)`, `for (auto *e : map)`.
- Functions and types: default and named arguments, methods (`x.f()`, `21.twice()`, `T_f()`),
  constructors (`vec3(1, z: 3)`), overloading by the first parameter, operators (`[[operator+]]`; `2 * v` finds `v * 2`).
- Owning values: `[[destructor]]`, `[[copy]]`, temporaries freed at the end of their statement, and
  structs that own things getting both member by member.
- Programs: modules (`#import`, `[[export]]`), macros that stay in their header, C libraries through
  `#pragma comment(lib, "...")`, `try` / `throw`, call stacks with `c2 -g`.
- The runtime (`#import "runtime"`): `string` with operators and f-strings (`f"{hp:.1f} hp"`), `array(T)`,
  `map(K, V)`, `set(K)`, coroutines, try/throw, and reflection: `reflect(T)` describes any type (fields,
  offsets, enum names), and every value has methods: `x.print()`, `x.text()`, `f"{hero}"` a whole
  struct, `x.save(path)` / `x.load(path)` / `x.json5()` / `x.xml()` (JSON5, or XML for `.xml`);
  `[[skip]]` leaves a field or a type out, `[[rename("hp")]]` saves a field under another name.  JSON5
  and XML documents, read by path (`doc.open(path)`).  `global("hero.hp")` reaches a global's part by
  name (consoles, editors; `C2_reflected` for the host), `x.find("pos.x")` a value's;
  `[[clamp(0, 100)]]`, `[[clamp(16)]]` and `[[comment("...")]]` say what a field accepts.
  `eval("heal(hero, 20)")` calls a function by name (defaults, named arguments, `hero.hit(30)`);
  `foreach_field(T, X)` is a compile-time loop over a struct's fields.

See [IDEAS.md](IDEAS.md) for the plans and [CHANGELOG.md](CHANGELOG.md) for what changed.

## Platforms
C² runs wherever MIR's JIT generates code. Currently:

| Target (MIR backend) | OS | Status |
|---|---|---|
| x86-64 | Windows | tested |
| x86-64 | Linux | tested |
| x86-64 | macOS | should work (same SysV ABI as Linux), untested |
| aarch64 | macOS (Apple Silicon) | tested |
| aarch64 | Linux | its `va_list` layout and `__aarch64__` are in, untested |
| ppc64le | Linux | needs its `va_list` layout and predefined macros |
| s390x | Linux | needs its `va_list` layout, predefined macros and big-endian `__BYTE_ORDER__` |
| riscv64 | Linux | needs its `va_list` layout and predefined macros |

Not MIR targets: 32-bit CPUs, Windows on ARM64, WebAssembly. (C² assumes 64-bit pointers.)

## Build, test, benchmark

Build C² by typing `make` on a terminal (Windows/Linux/macOS):

```
make                  # build C² for development: unoptimized + debug symbols
make release          # build C² for release: optimized without debug symbols
                      # (to develop, plain make: much faster to build, and the tests barely notice)
make clang            # build C² with clang / clang-cl: make clang release, or make release clang
make test [SUITE...]  # all test-suites, or only those: make test c2 c2-abi  (-v: why; -k NAME: matching tests)
make bench [NAME]     # the benchmarks, or the ones matching NAME: make bench fib
                      # (Linux/macOS: make test ARGS="c2 -v", make bench ARGS=fib)
make example          # build and run the embedding example (example.c)
make tour             # run every demo in tour/, one feature each
make tidy
```

Manually, with any of these options:

```
cl /O2 /DNDEBUG /MT c2.c 3rd\mir.c 3rd\mir-gen.c                                     # Windows (cl or clang-cl)
cc -O2 -DNDEBUG -fno-tree-sra c2.c 3rd/mir.c 3rd/mir-gen.c -lm -ldl -lpthread -o c2  # Linux, macOS
cmake -S docs -B .build-cmake && cmake --build .build-cmake --config Release         # CMake (generic)
```

`-DNO_MIR=ON` would build `c2.c` alone, without the JIT (only `-S` and `-E` work)

## Coverage

`make test [suite ...]` (or `python tests/test.py [suite ...]`) runs the test-suites.

Next to a test, `NAME.expect` holds its expected output, `NAME.expectrc` its exit code, and `NAME.expecterr`
lines that must appear in the compiler's messages, in order (`:3: error: ...`); a test whose `.expecterr`
has an error is meant not to compile (see `tests/c2-errors/`).

Our own suites are named `c2-*`; the others come from other C compilers' test-suites.

| Suite | Tests | Windows | Linux | Notes |
|---|---|---|---|---|
| `tests/c2` | 90 | 100% | 100% | C² features and runtime |
| `tests/c2-errors` | 70 | 100% | 100% | C² compile errors and warnings |
| `tests/andrewchambers_c` | 67 | 100% | 100% | |
| `tests/lacc` | 215 | 100% | 97% | Linux: 80-bit `long double` |
| `tests/c2m` | 105 of 124 | 100% | 100% | 19 skipped, each with a `NAME.skip` saying why: c2m-only builtins (`__builtin_prop_*`, `__builtin_jcall`), setjmp across JIT frames, two-file tests, `%Lf` |
| `tests/c2-abi` | 548 checks (460 on Windows) | 100% | 100% | host ⇄ script calls, typed host symbols, `C2_load`, a script compiled ahead of time |
| `tests/c2-reload` | 204 checks | 100% | 100% | hot reload, in JIT, interpreter and lazy modes |
| `tour` (suite `c2-tour`) | 47 | 100% | 100% | the feature demos: each must compile and exit with 0 (the 2 SDL3 ones are skipped) |
| `tests/c2-debug` | 4 | 100% | 100% | `-g` crash reports: the call stack printed must be the expected one |
| `docs/LANGUAGE.md` (suite `c2-book`) | 55 | 100% | 100% | the book's examples: each must print what the book shows under it |

Percentages are for `make test` (modes `api`, `text` and `debug`, which is `-g`), Nov/2026.
`--modes api,text,interp,lazy,c,debug` adds the interpreter, lazy generation and the C output; they fail
only where `api` does. <!-- Update this table
when tests are added. -->

`tests/c2-abi` checks calls between C² scripts and a host program built by the platform's C compiler: structs
of 1 to 32 bytes (ints, floats, doubles, mixed) passed and returned both ways, through function pointers
and `.c2` method calls.  `test.py` builds that host (`tests/c2-abi/abi.c`, which includes `c2.c`) into
`.build/` with `--abi-cc` when it is missing or out of date.  `tests/c2-reload` is a host program built the
same way: it recompiles scripts while running and checks the hot reload rules (`[[persist]]`,
`__on_reload`, `C2_keep_previous`, failed compiles, hundreds of reload cycles) with the JIT, the
interpreter and lazy generation.

`python tests/test.py --modes api,text,interp,lazy,c,debug` also runs every test through the
interpreter, with lazy generation, through the C output (`--cc` picks the C compiler) and with `-g`.

## Benchmarks

`make bench [name]` or `python tests/bench.py [name]` runs the benchmarks:

| Benchmarks (Linux x86-64, seconds) | C² | c2m -eg | gcc -O2 |
|---|---|---|---|
| 15 benchmarks, total | 59.7s | 59.2s | 45.2s |
| particles (struct locals) | 3.91s | 7.64s | 2.03s |
| fib(43) | 1.83s | 1.91s | 0.53s |

## License
Everything is put into the public domain, see [LICENSE.μ](../LICENSE.μ).

[MIR](https://github.com/vnmakarov/mir "MIT Licensed"), the agnostic code generator that we use, is MIT licensed.

[Tests](`tests/`) subfolders come from different places and keep their own licenses.

[minicoro](https://github.com/edubart/minicoro), the coroutines library that we use, is public domain or MIT No Attribution.
