# The C² tour

One feature per file, in a few lines each, in teaching order: every demo uses only what the earlier ones
showed, plus one new idea, and related ideas sit next to each other.  Read them in order.

- 01-04: C as it is now: no `#include`s, `f()` meaning `f(void)`, C23, the C23 preprocessor.
- 05-07: code in several files: headers that keep their macros, modules (`#import`, `[[export]]`) and
  their qualified and aliased names.
- 08-15: types and data: short type names, multi-character constants, zeroed locals and `[[unclear]]`,
  more attributes (`[[unroll]]` too), tail calls, exact array lengths and `countof`.
- 16-22: blocks and functions: `({ })`, nested functions, `defer`, overloading, default and named
  arguments (also on function pointers).
- 23-33: methods and objects: `21.twice()`, struct names as types, `.` on pointers, methods and type
  methods (`T_f()`), constructors, destructors and copies (and structs that own things), operators.
- 34-42: the runtime (`#import "runtime"`): string, its operators and f-strings (operators + destructors),
  array, map, set, one loop for all of them (`for (T x : things)`, ranges `0..n`), coroutines, and
  try/throw (which, like a yield, leaves code midway).
- 43-46: reflection: `reflect(T)`, `x.print()`, `f"{x}"` for a whole struct, `each_member`; saving and
  loading any value as JSON5 or XML; JSON5 and XML documents read by path; a console: globals found by
  name, fields with `[[clamp]]` and `[[comment]]`, functions called by name.
- 47: hooks: a function called at every function's entry (tracing, profiling).
- 48-49: a C library from a script, SDL3 opening a window: by hand with `dlopen`/`dlsym`, or through its
  header and `#pragma comment(lib, "SDL3")` (LuaJIT's ffi style).  They need SDL3, so `make test` skips them.

`c2 tour/demo_19_overloading.c2` runs one.  `make test` runs them all (suite `c2-tour`), so they keep
working; `make tour` runs them one after another.

Walk through all of them, each file shown and then run (from `tour\`):

    if exist ..\c2.exe for %i in (*.c2) do type %i && ..\c2 %i && pause

(In a `.bat` file, write `%%i`.)  On Linux and macOS: `for f in *.c2; do cat $f; ../c2 $f; read; done`
