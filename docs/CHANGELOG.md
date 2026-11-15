## Changelog

Newest first.  Proposals marked *(Committee)* came from the Committee.

### Nov/2026

**C² language (.c2 files)**
- `tests/verify.py` is now `tests/test.py` *(Committee)* (`make test` and `tests/test.bat` call it).
- The `sandbox_byname` test asks for `strnlen` (in glibc, macOS's libc and ucrtbase.dll alike, and not in
  C²'s table) instead of `getpid`, which Windows only has as `_getpid`: it failed there.
- Map and set keys that own memory *(Committee)*: a key type with `[[operator==]]` and `T_hash(T *)`
  compares and hashes through them, and the map keeps its own copy of each key (the type's `[[copy]]`,
  freed with its destructor).  `string` has `string_hash`, so `map(string, V)` and `set(string)` compare
  keys by their text: long keys no longer duplicate, and a key's temporary no longer dangles.  Missing
  `T_hash` is a compile error (and a key type with a destructor needs both).  Saving and loading use
  them too (`c2type.keyops`, new), and hot reload moves such maps entry by entry.
- `[]` on maps and sets, `+=` / `-=` on sets *(Committee)*: `ages["bo"] += 7` is `ages.at("bo")`,
  `seen[k]` is `seen.has(k)`, `seen += k` adds `k` once, `seen -= k` removes it (fine when missing).  In
  .c2, `m[i]` is now always by key (a loop walks the entries); C keeps `m[i]` as the i-th entry.
  Set theory between two sets (`A + B`, `A - B`, `A ^ B` and their `=` forms) is in IDEAS, not done.
- `string s = "text";` and `s = "text";` own the string the constructor made: a long text was freed
  twice before (the temporary was destroyed after the variable took it).
- One loop for everything *(Committee)*: `for (T x : things)`.  Ranges `a..b`,
  half-open both ways (`0..n` is 0 to n-1, `n..0` n-1 down to 0), with a signed constant step as a third
  part (`0..n..2`, `n..0..-2`); two variable ends need a step (the direction is never guessed at run
  time).  Text: `char` walks bytes, `u32` UTF-8 characters (`string`, `char [N]`, `const char *`).
  Arrays, `array(T)`, maps, sets: `T x` a copy that owns nothing, `T *x` (or `auto *x`) in place, `const
  T *x` to read; a map's entries, `m.keys()`, `m.values()`; a set's keys.  `reflect(T)` walks a type's
  fields, and a type with `bool T_next(T *it, X *out)` walks itself.  `0..10` lexes as a range in .c2.
  Tour demo 40; the demos after it move up one.
- Reflection and saving, one API: `x.verb(args)` *(Committee)*.  `any` is a value and its type
  (`c2ref` before); a parameter of type `any` takes any value (the compiler passes its type and
  address), and `x.f()` on a value with no `f` of its own calls `any_f(x)`, so the runtime's `any_*`
  functions are methods of every value.  Renamed: `dump(x)` → `x.print()` (`x.print(stderr)`),
  `value_text` → `x.text()`, `to_json5(x)` / `to_xml(x)` → `x.json5()` / `x.xml()`, `save_json5(path, x)`
  / `save_xml` → `x.save(path)` (XML for a `.xml` path), `load_json5(path, x, &errors)` / `load_xml` →
  `x.load(path, &errors)`, `from_json5(text, x)` / `from_xml` → `x.parse(text)` (XML when it starts
  with `<`), `reflected(path)` → `global(path)`, `reflected_in(x, path)` → `x.find(path)`,
  `ref_text(r)` / `ref_set(r, text)` → `r.text()` / `r.set(text)`, `reflect_globals()` /
  `reflect_functions()` → `globals()` / `functions()` (the old two remain), `invoke` → `eval`;
  documents: `doc.open(path)` / `doc.parse(text)` (by extension, by first character), `d.real()` →
  `d.number()`.  Loading's messages name the type (`Hero.bag[1].count: ...`).
- `T.f(...)` calls `T_f(...)`, a function that belongs to the type (`doc.open(path)`), also where a
  module of the same name is imported.
- Functions by name *(Committee)*: `eval("heal(hero, 20)", &result, &errors)` (runtime/serial) calls
  one of the program's functions from text: arguments by position or by name, defaults for the
  missing ones, globals (or their parts) passed as themselves or by address, other values read as
  loading reads them; `hero.hit(30)` calls `Hero_hit(&hero, 30)`.  `reflect_functions()` lists them
  (`c2func`: name, result, parameters with their names, types and whether optional, and a `call`).
- `foreach_field(T, X)` *(Committee)*: a compile-time loop, `X(name)` once per field of `T` (as
  reflection sees them), each expansion compiled where it stands.
- `[[pure]]` checked: a pure function that writes a global or calls one that isn't pure (C's math and
  string functions are; `string`'s getters are marked `[[pure]]`) gets a warning.
- Fixed: `&s.f` on a small struct local (`Vec a; memcmp(&a.x, ...)`) was "cannot take the address":
  its members lived in registers.  `typeof(x.bits)` of a bit-field is its type (so `h.lvl.print()` works).
  A program with more than 2048 MIR items (big ones under `-g`) failed with "too many items": 16384
  now, found by hash.
- `[[pure]]` *(Committee)*: C23's `[[reproducible]]` under a shorter name (gcc's `pure`): a call only
  computes its result, so throwing the result away is warned about; `[[reproducible]]` and
  `[[unsequenced]]` (gcc's `const`) do the same.
- An attribute C² doesn't know, in a `.c2` file, is a warning (`unknown attribute [[nodiscrad]],
  ignored`): a typo used to do nothing without a word.  Namespaced ones (`[[gnu::cold]]`) stay quiet.
  `__has_c_attribute` knows C²'s own attributes.
- The book has a chapter on attributes (18): all of them in one table, `[[pure]]`, and `[[persist]]`.
- By name *(Committee)*: `reflected("hero.pos.x")` finds a global's part by path (`hero/bag[2]`,
  `scores[Ada]`, through pointers to structs): a `c2ref` with its type and address.  `reflected_in(x,
  path)` starts from any value, `reflect_globals()` lists the globals (a module's private ones and
  `[[skip]]` ones left out), `ref_text(r)` shows one, `ref_set(r, "250")` sets one from text.  The host
  has `C2_reflected(c2, path)`.
- What a field accepts *(Committee)*: `[[clamp(min, max)]]` on numbers (and lists of them), `[[clamp(N)]]`
  on text (at most N characters, UTF-8; first named `[[range]]` and `[[maxlen]]`), `[[comment("...")]]`.  Loading and `ref_set` keep the
  nearest end of a range, or cut the text, and report it; saves write comments as `//` (JSON5) and
  `<!-- -->` (XML).  On the wrong kind of field they are errors.  `c2field` gains `comment`, `min`,
  `max`, `maxlen`, and `C2_CLAMP` in its flags.
- Reflection tables and enum names no longer carry over from one compile to the next in the same
  process (a host compiling several scripts, or reloading one, got stale tables).
- Tour demo 45, a console; hooks and SDL3 move to 46-48.
- Loading, and XML *(Committee)*: `load_json5(path, x)` / `from_json5(text, x)` read a value back through
  its reflection table; `to_xml`, `save_xml`, `load_xml`, `from_xml` do the same in XML.  Fields match
  by name (or `[[rename]]`); a missing one keeps its value, an unknown one is ignored; owned values are
  rebuilt and the old ones freed; what doesn't fit is reported with its path
  (`hero.bag[1].count: an integer expected, not text`) and the rest still loads.
- Documents *(Committee)*: `runtime/doc` parses JSON5 and XML into one tree of nodes, read by path
  (`d.integer("/window/width", 1280)`, `d.text("/button/@id")`, `d.count("levels")`), with a default
  for a missing value; a document that doesn't parse says where and why.  The parsers are rewritten
  from Kit's kit.data (JSON5 by Dominik Madarasz and r-lyeh, XML by Tor Andersson, public domain),
  fixed to return errors instead of stopping, to decode escapes, to read `1e5` as a float, to stop at
  the end of an unterminated string, to check that closing tags match, and to limit nesting.
- `runtime/serial` and `runtime/doc` are imported on their own (`#import "runtime/serial"`): in
  `#import "runtime"` they tripled the time every script took to compile.  `string_truncate(s, n)`.
- A C header included by a module (`<stdarg.h>`) declares its names for everyone, as in C: `va_list`
  was private to the first module that included it.
- Tour demos 43 (saving and loading) and 44 (documents); hooks and SDL3 move to 45-47.
- Saving, part C *(Committee)*: `to_json5(x)` and `save_json5(path, x)` (new `runtime/serial`) write any
  value as JSON5 through its reflection table: structs as objects, arrays and `array(T)` as lists, enums
  by name, maps with text or enum keys as objects (others as `[key, value]` pairs), floats with the
  fewest digits that read back the same, `NaN`/`Infinity`.  `[[rename("health")]]` saves a field under
  another name; `[[skip]]` fields, pointers (but `char *`) and unions are left out.  The file is
  written to `path.tmp` and then renamed over `path`: whole, or not at all.  Tour demo 43 (hooks and
  SDL3 move to 44-46).
- `dump`, `to_json5` and `save_json5` free the copy they make (a named value with owned members was
  copied and leaked).  `typeof(Vec(1, 2))` is the type of that value.  A module's private function can
  be declared before it is defined.
- An enum's tag is a type name, as a struct's or a union's: `Dir d;` for `enum Dir d;` *(Committee)*.
- Container methods *(Committee)*: `a.push(x)`, `a.count()`, `ages.set("ana", 31)`, `ages.at("bo") += 7`,
  `seen.add(3)`, `x.free()`: the runtime's array, map and set macros, called as methods (on locals,
  members, parameters).  In .c2 a function-like macro right after `.` or `->` isn't expanded, so
  `.set(` is the method even though `set(K)` is a macro.  `array_foreach` / `map_foreach` iterators
  are plain element pointers.
- Reflection, part A *(Committee)*: `reflect(T)` / `reflect(expr)` gives a `const c2type *` the compiler
  builds (name, kind, size, fields with exact offsets and bit-fields, enum names and values, element
  types, the `[[destructor]]` and `[[copy]]`); `string`, `array(T)`, `map(K, V)` and `set(K)` are kinds
  of their own.  `each_member`, `enum_name`, `dump(x)`, and f-strings print a whole struct, and an
  enum by name (`{e:d}` for its number).  `[[skip]]` on a field leaves it out of `each_member`, `dump`
  and saves; on a struct, union, enum or typedef, reflection knows nothing about the type.  `c2.h`
  declares the tables for hosts.  New `runtime/reflect`, tour demo 42 (hooks and SDL3 move to 43-45).
- `{"text"}` in a brace initializer builds a `string` member through its constructor, and in .c2 an
  array can be initialized from another of the same type (`int b[3] = a;`).
- `string_delete` is now `string_free` (`s.free()`) and `co_destroy` is `co_free` (`c.free()`), like every
  other deallocator *(Committee)*.
- The runtime's folder is `runtime/` (was `rt/`) *(Committee)*: `#import "runtime"` brings in all of it
  (`runtime/runtime.c2`), `#import "runtime/string"` one module.  `c2 -Cruntime` is another name for
  `c2 -Crt`.
- `#import g = "module"` names a module *(Committee)*, in place of `#import "module" as g` (now an
  error that says so): the name first, like C++'s `namespace g = ...`.
- Folder modules *(Committee)*: `#import "x"` takes `x.c2`, then `x/x.c2`, then `x/index.c2`.
- A constructor function converts *(Committee)*: `string s = "hi";`, `f("hi")` for `f(string)` and
  `return "hi";` call `string("hi")`, as the constructor's first parameter takes a `char *` and the
  others have defaults.
- The tour in a new order *(Committee)*: modules and scoped macros right after the preprocessor,
  overloading and arguments with the other function features, struct names and `.` on pointers next
  to methods; renumbered 01-44, each demo using only what the earlier ones showed.
- The book follows the tour's order: headers and modules in chapter 3, overloading with the other
  function features, struct names and `.` on pointers in the Methods chapter, the runtime (strings,
  containers, coroutines, try/throw) before debugging and C libraries, hooks with debugging.  Its
  string examples use `string s = "hello"`.
- Commutative operators *(Committee)*: `2 * v`, `1 + v` and `3 == v` use the `v * 2` / `v + 1` / `v == 3`
  function; `[[noncommutative]]` (or `[[nocommutative]]`) on it says no.
- try/throw: only calls that may lead to a `throw()` are followed by a check (a whole-program call graph);
  the try demo went from a check after every call to three.
- Structs that own things *(Committee)*: a struct with `string` (or other `[[destructor]]`) members gets
  `T_free` and `T_copy` member by member, nested structs and arrays included.
- `string` *(Committee)*: `a < b` (so `> <= >=`, and sorting), `"x" + s`, `s += 'c'`.  Operator lookup no
  longer lets a `char` match a `char *` parameter, or the reverse.
- Copies *(Committee)*: `[[copy]] T T_copy(const T *)`; storing or returning a named `T` copies it, so
  `string b = a;` gives `b` its own buffer.  Containers (`array_push`, `map_set`) move what they store.
- try/throw now unwind *(Committee)*: every call returns through its `defer`s and destroys its temporaries;
  a flag checked after each call (only in programs that throw) replaced the coroutine pool, so `try` costs
  a call, works in `c2 -C` output and needs no minicoro.  `try`/`throw` moved to `runtime/try`.
- Destructors and temporaries *(Committee)*: `[[destructor]] void T_free(T *)`; a call's `T` result is
  destroyed at the end of its statement unless it is stored, assigned or returned.
- f-strings *(Committee)*: `f"{name}: {hp:.1f} hp"` makes a `string`, with the conversion picked by type.
- `T(x)` with `x` a temporary `T` is `x` (`string(f"...")`).  A typedef's name is a constructor too: `Vec3(1, 2, 3)`.
- Overloading by receiver: `load(vec2 *)` / `load(vec3 *)`, picked by the first argument, `<receiver>_<name>`
  symbols *(Committee)*.  `x.f()` also finds `T_f()` for a typedef'd anonymous struct.
- Operators on structs *(Committee)*: `[[operator+]] vec3 vec3_add(vec3, vec3);` for
  `+ - * / % == < <= > >= [] += -= *= /= %=` and unary `-` (`!=` from `==`, the other comparisons from
  `<`); operator functions may have defaulted extra parameters; `string` has `==` and `+=`.
- Hot reload *(Committee)*: `[[persist]]` globals keep their values, `__on_reload()` runs after a reload.
- Review fixes *(Committee)*: exact array lengths only for `.c2`/`.h2` declarations (plain `.h` keep C's
  meaning); a changed default is an error; a warning when parameter names change between declarations,
  when a `goto` skips a zeroed declaration, and when a member hides a method; `[[musttail]]`; clearer
  errors for `21.f()`, defaults using later parameters and pointers to `v[3]`.
- Constructors: `vec3(1, 2, 3)`, `vec3(z: 9)`, and `vec3 vec3(...)` as a type's own *(Committee)*.
- Modules: `#import "dir/name"`, `[[export]]`, everything else private; `runtime/` is now made of modules *(Committee)*.
- Type aliases `i8`..`i64`, `int8`..`int64`, `u8`..`u64`, `uint8`..`uint64`, `f32`/`flt32`, `f64`/`flt64` *(Committee)*.
- `f()` means `f(void)`, as in C23 and C++ *(Committee)*.
- Swift-style named arguments: `box(h: 3)`, the same as `box(.h = 3)` *(Committee)*.
- `[[indeterminate]]` (C++26) and its alias `[[unclear]]`: a local that is not zeroed *(Committee)*.
- `defer` and `countof` as keywords; `x.f()` on a struct `T` first looks for `T_f()`.
- `21.twice()` is a method call on an `int`; `21.0.twice()` and `21.f.twice()` on floats *(Committee)*.
- File-scoped macros: `.h2` headers keep their macros, except what they `#export`.
- Optional parameters that use the earlier ones, defaults on function pointers, named arguments.
- Method calls `x.f(a)`, `p.x` on pointers, struct names as type names, exact array lengths `v[3]`.
- Zeroed locals, the common headers always included, 8-character constants, nested functions.
- Self tail calls run in constant stack (a warning says when they can't).
- Hooks: `__on_enter` / `__on_leave` for profilers and call stacks.

**C and its extensions (.c and .c2)**
- `[[unroll]]` and `[[unroll(N)]]` before a `for`, `while` or `do` *(Committee)*: the body is compiled N
  times in a row, with the condition tested between the copies, so one jump back per N iterations (a
  sum over an array: 25% faster with 4).  Plain `[[unroll]]` makes one copy per iteration for a loop
  counting between constants (up to 64), 4 otherwise.  A label, `static` or nested function in the
  body is an error, and the copies' warnings are said once.
- Tour and book: `[[unclear]]` (shorter) instead of `[[indeterminate]]` *(Committee)*;
  `demo_43_dlsym_sdl3.c2` and `demo_44_ffi_sdl3.c2`, named by what they show *(Committee)*.
- GNU labels as values: `&&label` and `goto *p`, also in `static void *tab[] = {&&a, &&b}` (threaded
  interpreters).  In `c2 -C` output they need GCC or Clang.
- Statement expressions: a label before the last statement (`done: r; })`) keeps `r` as the value;
  `({ ... s; }).x` and `(struct P){1, 2}.y` take postfix operators.
- Incomplete arrays: `extern int a[];` sized later in the file, and tentative `int a[];` (one element).
- GNU zero-length arrays (`char data[0]`), a stray `;` in a struct, `HUGE_VALL`.
- `-T` text mode: NaN and infinity in float data.  `c2 -C`: structs passed to `...` (printf of a struct
  of doubles) follow the ABI.
- The c2m suite passes: the tests meant to be rejected count as passing when C² rejects them, and the
  19 it can't run (c2m's own builtins and the like) are skipped, each with a `.skip` file saying why.
- `char a[] = ("wat");`: a string literal in parentheses initializes an array.
- `c ? NULL : p` has `p`'s type (`NULL` is a null pointer constant), as C requires.
- `__builtin_*_overflow`; `__has_builtin`, `__has_attribute`, `__has_c_attribute` in `#if`.
- Same results on every platform *(Committee)*: `%ld` works on Windows too (fixed while compiling),
  gcc's bit-field layout everywhere with `#pragma ms_struct` for MSVC's, structs through `...` to C.
- `[[nodiscard]]` and `[[deprecated("why")]]` warn; the other standard attributes are ignored *(Committee)*.
- `defer` (TS 25755) as `_Defer`, `_Countof` (C2y), GNU statement expressions `({ ... })` *(Committee)*.
- K&R function definitions, `L"..."` / `L'x'` / `wchar_t` and `<wchar.h>`, `_Pragma`, digraphs, trigraphs.
- C23: `#embed`, `__has_include`, `__has_embed`, `#warning`, `#elifdef`, `__VA_OPT__`, digit separators,
  `bool`/`true`/`false`, enums with a fixed type, `typeof`, `auto`, `constexpr`, `u8'x'`.
- Uninitialized locals in `.c` follow C, with a warning when read before being set.

**Runtime (runtime/)**
- try/throw *(Committee)*: `try(fn, arg)` returns 0 or the code of a `throw(code)` below it; coroutines
  from a pool underneath.
- `string` *(Committee)*: the constructor `string("hi")` (was `string_from`), `a + b`, `a - b` (removes every
  occurrence), `s[-1]`, `+=`/`-=`/`==` with another string, `remove`, `string_fmt`, `appendv`.
- Coroutines *(Committee)*: `runtime/coro` on minicoro (`3rd/minicoro.h`): `co_create`, `co_resume`, `co_yield`,
  `co_status`, `co_destroy`; 1 MB stacks; hot reloads are refused while one is suspended.
- `map(K, V)` and `set(K)` in `runtime/map.c2` (part of `#import "runtime"`): dense entries plus a hash index.
- `string`: small-string optimized, 23 bytes inline *(Committee: SSO design)*, plus `append`,
  `appendf`, `copy`, `equal`, `clear`, `reserve`, `string()`.
- `array(T)`: a plain `T *` with a hidden header *(Committee: based on min.c)*.

**Embedding and tools**
- Hot reload: a persisted map or set whose key or value type changed moves entry by entry.
  `C2_safe_reload(c2, 1)` (off by default) makes the script's functions count themselves in and out,
  and refuses a compile while any runs (a script calling the host, which compiles, would return into
  released code).
- Every `tests/` suite has its LICENSE: ours (c2, c2-abi, c2-debug, c2-errors, c2-reload) are
  public domain (the Unlicense); c2m and benchmarks are MIR's (MIT), as lacc's and andrewchambers_c's
  were already theirs.
- Hot reload, a `[[persist]]` struct whose type changed *(Committee)*: it moves field by field, through
  the reflection tables of both programs (when they import the runtime): fields match by name (or
  `[[rename]]`), numbers convert and meet a new `[[clamp]]`, an enum keeps its value's name, arrays keep
  their items (an `array(T)` is rebuilt when `T` changed), strings and containers move, function pointers
  inside move to the new functions; what can't move starts again with a warning naming it
  (`'hero.tag' starts again: its type changed from 'int' to 'string'`), dropped fields are freed.  It
  also catches a struct whose fields changed but whose size didn't (before, it was copied byte for byte).
- Sandboxed `exit()`, `_Exit()`, `quick_exit()` and `abort()` *(Committee)* don't end the host: the
  script leaves every function, running their defers and passing any `try`, and the host's call returns;
  `C2_exited(c2, &code)` tells it (and clears it), `C2_run_main` returns exit's code.  `atexit()` is refused.
- `C2_sandbox(c2, on)` and `c2 -sandbox` *(Committee)*, off by default: a sandboxed script's `remove`,
  `unlink`, `rename`, `fopen` for writing, `system`, `popen`, `exec*`, `dlopen`, `dlsym`,
  `MoveFileExA`... fail when called (-1, NULL or 0, errno `EPERM`; the runtime's saves return false), and
  nothing else is looked up by name in the C library or the OS (`'getpid' isn't available in the
  sandbox`, at compile time).  The host's symbols and the libraries it loaded still work.  A guard
  against mistakes, not a security boundary.
- `c2 -C -prefix ui_` *(Committee)*: what the output defines is renamed `ui_...` (`ui_init`, `ui_main`,
  no C `main`), so several scripts compiled ahead of time link into one program.  The c2-abi suite
  links two, both with the runtime's string.
- Ahead of time without `c2_rt.c` *(Committee)*: c2.c defines `c2_rt()`, so `c2 -C` output links with C²
  built as a library (`C2_NO_MAIN`), the way a game that embeds C² already has it, and gets the same C
  library, coroutines and try/throw as the JIT.  `c2 -Crt` stays, for programs without C² in them.
  The c2-abi suite builds and runs one such program.
- `make release` *(Committee)*: optimized, no debug symbols (Windows `/DNDEBUG /MT /O2`); plain `make` builds
  with debug symbols (Windows `/DNDEBUG /Zi`, Linux/macOS `-O2 -g`).  `clang` combines with either, in any
  order.  The Makefile rebuilds `./c2` when switching.
- `c2-book`: `make test` runs every example of `docs/LANGUAGE.md` and compares its output with the book's
  *(Committee)*.
- Call stacks show which frames resumed a crashed coroutine.
- `-T` (MIR text): `float` globals with an initializer are written `1.5f`, as MIR's reader wants; six lacc
  tests failed on it ("data operand is not of data type").
- Crash reports show each frame's variables *(Committee)*: `hit(e = NULL, dmg = 5) at game.c2:5`, and the
  locals in scope below it.  `-g` keeps variables in memory for that (about twice as slow on call-heavy
  code, as `-O1` alone was 20%).  `verify.py` turns off Windows Error Reporting for its runs, so crashing
  tests exit at once.
- Windows: `C2_crash_handler` also installs a vectored exception handler, since an access violation in JIT
  code never reaches the C runtime's `SIGSEGV` (it can't walk frames without unwind data) *(Committee)*.
- `-g` compiles at most at `-O1`: `-O2` dropped stores to the call stack's frames, so a crash in a small
  function showed garbage *(Committee)*.  New suite `c2-debug` (an `abort()`, a null pointer, a crash in
  a coroutine, each checked against the call stack expected), and `make test` runs the other tests with
  `-g` too (mode `debug`).
- `verify.py` writes the book's examples with `\n` line ends on Windows too (`#embed` saw `\r\n`).
- `math.h`: the remaining `float` variants (`sinhf`, `lgammaf`, `lrintf`, `fmaf`...) and `fpclassify`.
- Debugging *(Committee)*: `c2 -g` / `C2_set_debug` keep a script call stack, `C2_backtrace()` returns it and
  `C2_crash_handler(1)` prints it on a crash; `c2 -C` writes `#line` directives, so C debuggers step
  through the `.c2` source.
- `demo_embed.c` is now `example.c` (`make example`), rewritten *(Committee)*: host symbols without a signature (`C2_add_symbol`) and with one
  (`C2_declare`, `C2_add_function`, `C2_add_variable`), a script that disagrees with the host, hot reload
  with `[[persist]]` and `__on_reload`.  A `.c2` script declaring a host function with other parameters is
  now an error (it became an overload that could never link).
- `#pragma comment(lib, "SDL3")`: a script binds a C library itself, as LuaJIT's `ffi.load` *(Committee)*.
- `demos/` is now `tour/` (suite `c2-tour`, `make tour` runs it all) *(Committee)*: new demos for C23, the C23
  preprocessor, qualified and aliased module names, and SDL3 by `dlsym` or by header + library; it replaces
  `demo_tour.c2`, `demo_module.c2` and `demo_sdl3.c2`, in teaching order: related demos sit together (constructors then destructors; string,
  its operators, f-strings; coroutines then try/throw).  A test whose first lines say `make test: skipped` is.
- `make test` runs our suites first (c2, c2-errors, c2-tour, c2-book, c2-abi, c2-reload), then c2m, lacc, andrewchambers_c.
- Typed host symbols: `C2_declare`, `C2_add_function`, `C2_add_variable` *(Committee)*.
- `extern int (*fp)(int);` got its own storage instead of the host's variable (the parameter list reset `extern`).
- Hot reload contract in `c2.h`; `C2_keep_previous`, `C2_get_previous`, `C2_release_previous`;
  `tests/c2-reload/` (a host harness, in every mode).
- `C2_get` of a global without an initializer returned a wrong address.
- Lazy generation never worked on Windows: MIR's stub for a function's first call wrote its addresses
  10 bytes off (over its own instructions) and called the generator with a misaligned stack.  Fixed in
  `3rd/mir-x86_64.c` (marked `C²:`), the first changes to the vendored MIR.
- `CMakeLists.txt` moved to `docs/` (`cmake -S docs -B .build-cmake`); `make.bat` builds with `/O2`.
- Folders: MIR's sources in `3rd/` (was `mir/`); our own suites are `c2-*`: `tests/c2-errors/` (was `tests/c2/errors/`),
  `tests/c2-abi/`, `tests/c2-reload/`, and `demos/` runs as `c2-demos`.  `make test` / `make bench` stop early without `c2`.
- Less stack while compiling: argument lists of calls live on the heap (a `/Od` Windows build overflowed
  its 1 MB stack on nested `({ })`); `make.bat` gives `c2.exe` an 8 MB stack, as on Linux.
- Tests for compile errors and warnings: `NAME.expecterr` (`tests/c2-errors/`).
- Renamed to C²: `c2.c` / `c2.h` and the `C2_` API.
- `C2_load` / `C2_unload`: bind a header and a dynamic library, no extension needed.
- `C2_compile` returns the error text; tests run per suite (`make test c2`); `make tour`, `make demo`.
- Demos: `demo_tour.c2` (every feature), `demo_sdl3.c2` (an SDL3 window), `demo_embed.c` (the host side).

**Docs**
- `docs/LANGUAGE.md` *(Committee)*: the C² language as a book, in teaching order, with every example run for
  its output; the README keeps a summary.
- `demos/`: one `demo_NN_<feature>.c2` per C² feature, a few lines each, numbered in reading order (each adds
  one idea to the earlier ones); `make test` runs them (suite `c2-demos`) *(Committee)*.
- `IDEAS.md` sorted by value for effort; designs for reflection (with Kit's API), operator attributes
  (`[[operator+]]`) and modules (`#import`).
