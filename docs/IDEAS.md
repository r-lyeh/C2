## Ideas (not started)
Sorted by value for effort: the top ones are cheap and useful, the bottom ones are expensive, risky or
questionable.  Effort is a rough guess at the size of the change in `c2.c` (S: < 100 lines, M: a few
hundred, L: a large change or a design question first).

Done since the last sort: `.c2` locals start zeroed (`.c` follows C and warns on reads before
assignment), optional parameters that use other parameters and defaults on function pointers, the common
headers always included in `.c2`, self tail calls, and `.h` + library binding (`C2_load`), file-scoped
macros in `.h2` headers (`#export`), and the C gaps K&R definitions, `L"..."`/`wchar_t`, `_Pragma` and
digraphs. Still open from that idea: scoping the macros of plain `.h` headers too (`windows.h`), e.g. an
opt-in `#include "windows.h" private`.

### 1. Cheap and useful: do these first
- **`[[packed]] struct x {};`** (S) **on hold**.  Attributes are already parsed (and ignored); a packed
  layout is a small change in struct layout.  But unaligned access is only safe for plain loads and
  stores: on aarch64 (Apple Silicon included) atomics and some vector/pair instructions fault on
  unaligned addresses, and passing packed structs by value has to match what clang does on each ABI.
  Test on macOS/aarch64 before relying on it (the abi tests can cover the by-value part).

### 2. High value, moderate effort
- **Reflection and serialization** (M).  The most useful item for games: whole-struct printing,
  save/load, editor inspectors; JSON / JSON5 / msgpack / XML are library code on top.  Design (v1),
  merging Kit's `kit.reflect.h` API shape with exact layouts from the compiler:
  - `reflect(T)` / `reflect(expr)`, like `sizeof`: a `const c2type *` to a table built at compile time
    (exact sizes, offsets, bit-fields, arrays, nesting; no guessing, no `REFLECT(...)` wrapper, no
    run-time parser).  Works on any type; tables are emitted only for the types used (and what they reach).
  - `[[reflect]] struct T {...}` also registers T by name: `reflected("Player")`, and paths
    `reflected("Player.pos.x")` (Kit's separators `. / @ :: ->` accepted) for consoles, config files and
    editors that start from a string.  The host gets the same through `C2_reflected(c2, "Player.hp")`.
  - Each field's `meta` is the text of its `[[...]]` attributes (Kit's `META("...")`), parsed by tools.
  - `each_member(T, m)` loops over fields, as in Kit.  `dump(x)` and `enum_name(T, v)` are ~60 lines of
    C² in the prelude, on top of the tables.  `c2.h` exposes the same structs to the host.
  - Not in v1: listing functions or globals, building types at run time.

  ```c
  typedef struct c2field { const char *name, *meta; const struct c2type *type; int offset, bits, bitpos; } c2field;
  typedef struct c2enum  { const char *name; long long value; } c2enum;
  typedef struct c2type {
      const char *name;          // "Player", "int", "float *", "char [8]"
      int kind, size, align;     // C2_INT C2_UINT C2_FLOAT C2_BOOL C2_PTR C2_ARRAY C2_STRUCT C2_UNION C2_ENUM C2_FUNC
      const struct c2type *of;   // pointee, array element, enum's underlying type
      int count;                 // array length, fields, enum values
      const c2field *fields;     // struct / union
      const c2enum  *values;     // enum
  } c2type;

  struct Vec { float x, y; };
  enum Dir : unsigned char { NORTH, EAST, SOUTH, WEST };

  [[reflect]] struct Player {
      [[meta("display name")]]      const char *name;
      [[meta("range(0, 100)")]]     int hp;
                                    Vec pos;
                                    enum Dir facing;
      [[meta("hidden")]]            unsigned flags : 4;
  };

  int main() {
      Player hero = { "hero", 75, { 3, 4 }, EAST };

      dump(hero);   // Player { name = "hero", hp = 75, pos = Vec { x = 3, y = 4 }, facing = EAST, flags = 0 }

      for each_member(Player, m)                          // an inspector, in 2 lines
          printf("%-6s %-10s +%-2d %s\n", m.name, m.type->name, m.offset, m.meta ? m.meta : "");

      const c2field *f = reflected("Player.hp");          // from a string: console `set Player.hp 100`
      *(int *)((char *)&hero + f->offset) = 100;

      printf("%s\n", enum_name(reflect(enum Dir), hero.facing));   // "EAST"
  }
  ```
  Open questions: what `reflected()` returns for a type versus a field path (two functions, or a
  `c2field` for both); how `dump` prints pointers (address, or follow `char *` as a string only);
  `printf(1, 2.f, " oh my ", mystruct)` style printing still needs overloading or a `_Generic`-like
  built-in (see below).
- **Containers: vectors, hashmaps, interning, sorting, overridable allocators** (M), as a built-in
  `.c2` library header written in C², not compiler changes.  Base: r-lyeh/min's `array(t)` (a hidden
  size/capacity header before a plain `t*`), with out-of-memory checks, integer growth, no shared
  scratch globals and single evaluation of arguments (`typeof` helps); a map with open addressing that
  grows and deletes cleanly, with hash/compare picked from the key type.
- **`C2_load`, the rest**: headers that need more of the compiler extensions found in system and
  library headers (inline asm, `__builtin_*`, MSVC intrinsics), and `long` on Windows (64-bit in C²,
  32-bit in MSVC-built libraries: a per-library `long` size?).
- **Overloading by argument types** (M), then **operator overloading for math types** (M on top):
  `a + b * 2` on `vec3` is the biggest daily win for game code.  Needs name mangling, best-match rules
  through conversions, and a rule for which overload `C2_get` and function pointers mean.

### 3. Worth doing, more design or effort
- **Methods: `struct myclass { void func() const; };`** (M).  `x.f()` already works for free
  functions; declaring them inside the struct adds `this`, `const` methods and name scoping.  Mostly
  syntax over what exists.
- **Coroutines; `setjmp`/`longjmp`; a Lua-like API** (M-L).  `setjmp`/`longjmp` is a C gap (MIR has no
  direct support).  Coroutines need separate stacks (fibers on Windows, `ucontext` or a little assembly
  elsewhere) and can live in the C² library with no language change.
- **Allocation headers: `((void *)ptr)[-1]` holding size, capacity, type and a deallocator** (M), for
  strings and containers that know their size and a single routing `free`.  Safe only as C²'s own
  allocation API: memory from the host or plain `malloc` has no header, so replacing `free()` itself
  would crash on it.  Pairs with the containers item.
- **Heap allocations zeroed by default, and zeroed growth (realloc)** (S once the allocation headers
  exist; the old size is needed to zero only the new part).  Costs time on big buffers that are about to
  be overwritten anyway: better as C²'s allocator behaviour than as a change to `malloc`.
- **Native executables and DLLs** (M with libtcc): today through `c2 -C` and a C compiler.  MIR can't
  write object files.  [libtcc](https://bellard.org/tcc/) can compile the `-C` output to an exe or DLL
  in-process (Windows, Linux).  Other back ends that emit object code or assembly:
  [QBE](https://c9x.me/compile/), Cranelift, LLVM (each a large dependency).

### 4. Large or open design questions
- **Modules: `#import`/`#export`, no headers needed** (L): `#import "module"` (app.c2),
  `#public`/`#private` (module.c2).  Changes name lookup, symbol separation between scripts and
  preprocessor scoping, and needs several files compiled together (C² compiles one translation unit
  today).  Write the design first.
- **Nested functions that use the enclosing function's variables (captures), and lambdas** (L).
  Captured variables have to reach the inner function, which a plain C function pointer can't carry:
  copy-values into a hidden struct plus a "fat" function pointer breaks calls from C; trampolines need
  executable stacks.  Nested functions without captures are done.
- **Uniform call syntax, the rest: `a["b"]` as `a.b`** (M) works for constant strings (compile-time
  member lookup); with a string known only at run time it becomes reflection.  **`a[b]` as `a.b`**
  clashes with array indexing: drop it.
- **Local macros scoped to a loop or a function** (L for little gain): the preprocessor runs before
  scopes exist.  The file-scoped version above gets most of the benefit.

### 5. Questionable: probably not worth it
- **Units: `10Km 1024KiB 10cm 3ft`** (M).  Fun but niche; if wanted, make it general (user-defined
  literal suffixes that call a function), and watch for clashes with hex literals (`0x1F`).
- **Overloading by return type** (L).  Ambiguous whenever a result isn't assigned, costly to
  implement, rarely needed.
- **`0.0` as an untyped float literal (`0.0d`, `0.0l`) and `0` for any integer type (`0u`, `0s`)**.
  C already converts `0.0` to `float` and `0` to `unsigned` silently; the real problem is other
  compilers' warnings, not the language, and new suffixes make code less portable.
- **Tail calls to other functions** (`return g(x)`, mutual recursion): a new MIR instruction in every
  back end, a MIR patch per CPU.
- **MIR as a DLL**: build `mir.dll` (MIR's CMake has a `mir_shared` target; on Windows it needs
  `CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS`, since `mir.h` marks no exports).
  - Linked normally: no code changes; smaller `c2.exe`, MIR rebuilt separately, one more file to ship.
  - Loaded at run time (`LoadLibrary`/`dlopen` + `mir.h`): C² would start without MIR and still do
    `-C`/`-S`/`-E`. Costs ~100-150 lines of function-pointer glue for the 46 MIR calls, and the DLL must
    come from the same MIR version as the header (no stable ABI: C² reads MIR's structs directly).
  - Doesn't help iOS/consoles (no DLLs there either), and works against the drop-in library goal, which
    the current build (MIR's sources compiled with `c2.c`) already serves.
  - No effect on script speed: MIR is only called while compiling.
