## C²

I have always needed a C library that could compile C scripts on the fly; primarily for use within game scripts.

In this repository, I am experimenting with something similar to `stb_compiler.h`. It is capable of parsing a significant amount of C code, includes a preprocessor, is sufficiently fast and compact, and relies on external dependencies for code generation and JIT compilation; making its source code essentially platform-independent.

[TinyCC](https://bellard.org/tcc) is probably the closest existing solution, but it is not quite what I was looking for; hence the creation of this repository.

## Usage
```
c2 file.c [args...]       compile, JIT and run main(argc, argv); its result is the exit code
c2 -S file.c              print MIR text instead of running
c2 -E file.c              print the preprocessed source
c2 -i file.c [args...]    the same with MIR's interpreter (no machine-code generator)
c2 -C file.c > out.c      translate to C (no JIT at all); compile it with c2_rt.c (c2 -Crt)
c2 -O0..-O3 file.c        MIR optimization level (default -O2; lower compiles faster)
c2 -lazy file.c           generate each function's code on its first call (faster start)
c2 -T / -A file.c         debug: run through the MIR text path / print what the C API built
options: -Idir, -Dname[=value]
```

## Embedding (c2.h)
```c
#include "c2.h"
typedef struct { float x, y, z; } Vec3;
Vec3 vec3_add(Vec3 a, Vec3 b);                       /* a host function */

C2 *c2 = C2_new();
C2_add_symbol(c2, "vec3_add", (void *)vec3_add);   /* scripts declare it and call it */
C2_add_symbol(c2, "score", &score);                 /* script: extern int score; */
const char *err = C2_compile_file(c2, "game.c");  /* "" on success, else the errors */
if (*err) puts(err);
Vec3 (*update)(float) = (Vec3 (*)(float))C2_get(c2, "update");
update(dt);
C2_compile_file(c2, "game.c");                      /* hot reload; on error the old code stays */
C2_load(c2, "sdl.h", "SDL2");                       /* bind a C library: header + SDL2.dll/.so/.dylib */
C2_delete(c2);
```

- Compile `c2.c` with `C2_NO_MAIN` into your program and link `MIR/mir.c` and `MIR/mir-gen.c` files.
- `C2_compile(c2, filename, source)` and `C2_compile_file()` return `""` on success, or the error messages. 
- `C2_errors()` has all diagnostics of the last compile, warnings included.
- `C2_use_interpreter(c2, 1)` runs scripts with MIR's interpreter instead
- `C2_set_opt_level()` tunes code generation
- `C2_set_lazy()`  has a faster boot time but there is a penalty for each function on its first call.
- Script functions use the platform C calling convention, structs by value included, so both sides call each other through plain function
pointers.
- `C2_load(c2, header, library)` binds a C library: scripts compiled afterwards see the header's
  declarations (as if they `#include`d it) and its functions resolve from the library.  `library` has no
  extension: `"SDL2"` loads `SDL2.dll` on Windows, `libSDL2.dylib` or `SDL2.dylib` on macOS, `libSDL2.so`
  or `SDL2.so` elsewhere, from the system's search path or the current directory (a path may come
  first: `"libs/SDL2"`).  Common header decorations (`__declspec`, `__attribute__`, `__cdecl`...) are
  ignored.  On Windows mind `long`: it is 64-bit in C², 32-bit in MSVC-built libraries.
  `C2_unload(c2, header)` drops it from the next compiles; the library is closed once a new program
  replaced the current one.  The command line has `-load header.h,library`.
- Compiler is currently not thread-safe, so do not share compiler contexts between threads.

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
- C99: headers: `stdbool stdint inttypes`; `snprintf`, `isnan`/`isinf`/`isfinite` and the common `math.h` float variants
- C11: `_Static_assert`, `_Generic`, `_Alignof`, `_Noreturn` (`stdnoreturn.h`), `stdalign.h`
- C11: `_Alignas` on struct members (accepted on variables, but ignored there)
- C11: anonymous `struct` / `union` members; `u8"..."` strings
- C11: `_Thread_local` is accepted, with one copy (scripts are single-threaded)
- C23: preprocessor: `#embed` (with `limit`, `prefix`, `suffix`, `if_empty`), `__has_include()`, `__has_embed()`, `#warning`
- C23: digit separators (`1'000'000`), binary literals (`0b101`)
- C23: `bool` / `true` / `false` as keywords; `#elifdef` / `#elifndef`; `__VA_OPT__`
- C23: enums with a fixed underlying type (`enum E : unsigned char`), and enums with values beyond `int` (they become `long long`)
- C23: `constexpr` (an integer one is a named constant, usable in array sizes and `case` labels); `[[attributes]]` (accepted, ignored); `alignas` / `alignof` / `thread_local` as keywords
- C23: `typeof` / `typeof_unqual` (also `__typeof__`), `auto x = expr;` type inference, `u8'x'` character literals (`unsigned char`)
- C23: `nullptr`, `static_assert` (message optional), empty initializers (`= {}`), unnamed parameters in definitions, labels before declarations

### New C² additions (.c2 files)
- `__C2_EXT__` is defined when compiling a `.c2` file, so shared headers can test for either C or C².
- Variables without an initializer start zeroed (`0`, `{0}`, NULL).
- `<stddef.h>`, `<stdint.h>`, `<stdio.h>`, `<string.h>`, `<stdlib.h>` and `<math.h>` are always included.
- Multi-character constants up to 8 characters (`'ABCDEFGH'`, type `long long`); `.c` files allow 4.
- Struct and union names are type names too, as in C++: `struct Vec { float x, y; };` then `Vec v;` (unless
  a variable, function or enumerator has the same name).
- `p.x` on a pointer to a struct means `p->x`.
- Method calls: `x.f(a, b)` calls `f(x, a, b)` when `x` has no member `f`.  If `f`'s first parameter is a
  pointer and `x` is not, `&x` is passed, so `c.add(5)` can change `c`; `p.add(5)` and `p->add(5)` pass `p`.
  Works on any value: `"hello".len()`, `21.twice()` (an `int`: after a number's `.`, a name that can't
  continue a float literal is a method), `21.0.twice()` and `21.f.twice()` (floats).
- Optional parameters: `int box(int w, int h = 10, int d = w * 2)`; the default is evaluated at each call
  that leaves it out, and can use the other parameters by name.  Parameters after one with a default need
  one too.  A function pointer type can have its own defaults: `int (*fp)(int a, int b = 7) = add;`.
- Named arguments: `box(.h = 5, .w = 2)`, after any positional ones.
- File-scoped macros: macros defined in an included `.h2` (or `.c2`) file are visible only in that file
  and in the expansions of its own macros, so a header can't leak its helpers (`MIN`, `MAX`...) into its
  includers, and an includer's macro of the same name doesn't break the header.  `#export NAME...` (or
  `#export define NAME ...`) makes a macro visible everywhere.  Include guards work as before: the guard
  stays visible to the header itself.  Plain `.h` files behave as in C.
- Nested functions: a function defined inside another one, visible in the enclosing block (recursion
  works).  They cannot use the enclosing function's variables yet.
- `int sum3(int v[3])` takes exactly an array of 3: passing `int a[4]` or a pointer is an error.
  (`int v[static 3]` keeps C's meaning: at least 3, not checked.)

All of them at work in one file: [demo_tour.c2](../demo_tour.c2).  [demo_sdl3.c2](../demo_sdl3.c2) opens an
SDL3 window, loading the library at run time with `dlopen` (no SDL headers needed).

See [IDEAS.md](IDEAS.md) for the plans.

### Differences from standard C
- Reading a local before setting it warns in `.c` files (`'var' is used uninitialized`).
- Multi-character constants pack their characters deterministically, in source order: `'12' == 0x3132`, `'1234' == 0x31323334`.
- Self tail calls (`return f(...);` inside `f`) become loops, so tail recursion of any depth runs in
  constant stack.  When that can't be done (a local's address is taken, a variable-length array is in
  scope, `...`, struct parameters or results) the call stays recursive and a warning says why.  With
  hooks (below) every call stays a call.
- `long` is 64-bit on every platform, Windows included (use `%lld` for `printf` there).
- Hooks: when a script defines `void __on_enter(const char *func, const char *file, int line)` and/or
  `__on_leave(...)` (or the host provides them with `C2_add_symbol`), every other function calls them on
  entry and before each return: profilers, script call stacks, tracing.  Without them nothing is emitted.
- Extra: `<dlfcn.h>` on Windows too (LoadLibrary/GetProcAddress); `strdup` in `<string.h>`.
- On Windows, by-value structs of 3, 5, 6 or 7 bytes to/from external functions are not supported
  (MIR passes them in a register, MSVC by reference).
- One translation unit per compile: no object files, no linking of several `.c` files, no debug info, no
  inline assembly.  Native executables only through `c2 -C` and another C compiler.

### Missing from standard C
- C89: `long double` as its own type (it is `double`; `1.0L` is a `double`)
- C89: `#include` of system headers from disk: only the built-in ones exist (`<other.h>` is ignored with a note)
- C89: `setjmp.h`: `setjmp` / `longjmp`
- C89: `locale.h`: only `setlocale` and the `LC_*` constants (no `localeconv`)
- C95: wide character I/O and classification (`wprintf`, `fgetws`, `wctype.h`...); `printf("%ls")` works
- C99: `math.h`: `fpclassify` and the `FP_*` constants; most `float` variants (`sinhf`, `cbrtf`...)
- C99: `_Complex` / `_Imaginary`; headers `complex.h fenv.h tgmath.h wctype.h`
- C99: pointers to variable-length arrays (`int (*p)[n]`); only the outermost dimension may be variable
- C99: non-ASCII identifiers (UCNs or UTF-8 in names)
- C11: `aligned_alloc`
- C11: `_Atomic` (`stdatomic.h`), `threads.h`
- C11: `u"..."` (16-bit), `U"..."` (32-bit), `uchar.h`
- C23: `_BitInt`; taking the address of an integer `constexpr`
- C23: `char8_t`

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

Build C² with any of these options:

```
cl /O2 /DNDEBUG /MT c2.c mir\mir.c mir\mir-gen.c                                     # Windows (cl or clang-cl)
cc -O2 -DNDEBUG -fno-tree-sra c2.c mir/mir.c mir/mir-gen.c -lm -ldl -lpthread -o c2  # Linux, macOS
cmake -S . -B .build && cmake --build .build --config Release --parallel             # CMake (generic)
```

`-DNO_MIR=ON` would build `c2.c` alone, without the JIT (only `-S` and `-E` work)

Then `python tests/verify.py` runs the test-suites (`python tests/verify.py c2 lacc` or `make test c2`: only those):

| Suite | Coverage (passing) |
|---|---|
| `tests/c2` | 100% |
| `tests/andrewchambers_c` | 100% |
| `tests/lacc` | 96% |
| `tests/c2m` | 58% (many use c2m-only builtins and GNU extensions) |
| `tests/abi` | 100% (528 checks) |

`tests/abi` checks calls between C² scripts and a host program built by the platform's C compiler: structs
of 1 to 32 bytes (ints, floats, doubles, mixed) passed and returned both ways, through function pointers
and `.c2` method calls.  `verify.py` builds that host (`tests/abi/abi.c`, which includes `c2.c`) into
`.build/` with `--abi-cc` when it is missing or out of date.

`python tests/verify.py --modes api,text,interp,lazy,c` also runs every test through the interpreter,
with lazy generation and through the C output (`--cc` picks the C compiler).

And `python tests/bench.py` runs the benchmarks:

| Benchmarks (Linux x86-64, seconds) | C² | c2m -eg | gcc -O2 |
|---|---|---|---|
| 15 benchmarks, total | 59.7s | 59.2s | 45.2s |
| particles (struct locals) | 3.91s | 7.64s | 2.03s |
| fib(43) | 1.83s | 1.91s | 0.53s |

Alternatively, typing `make` on a terminal (Windows/Linux/macOS):

```
make                  # build C²
make clang            # build C² with clang / clang-cl
make test             # run tests/verify.py.  Windows: make test -v -k NAME;  Linux/macOS: make test ARGS="-v -k NAME"
make bench            # run tests/bench.py with C².  make bench -k fib (ARGS="-k fib")
make demo             # build and run the embedding demo (demo_embed.c)
make tour             # run demo_tour.c2, a tour of the C² additions
make tidy
```

## Links
- [MIR](https://github.com/vnmakarov/mir "MIT Licensed"), the agnostic code generator we use.

## License
Everything is put into the public domain, see [LICENSE.μ](../LICENSE.μ). `mir/` is [MIR](https://github.com/vnmakarov/mir) (MIT license, see `mir/LICENSE`). Tests in `tests/andrewchambers_c`, `tests/lacc`, `tests/c2m` and `tests/benchmarks` come from different places and keep their own licenses.
