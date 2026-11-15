## Ideas
What's left to do, by priority.  Finished work is in CHANGELOG.md.  Effort is a rough guess at the size
of the change in `c2.c` (S: < 100 lines, M: a few hundred, L: a large change or a design question first).

Settled: the name stays C² and the extension stays `.c2` (the older C2 language also uses it; ours is the
one that matters here), and `LICENSE.μ` keeps its name.

### 1. Next
- **Operators, the rest**: done: `+ - * / % == < <= > >= [] += -= *= /= %=`, unary `-`, and operator
  functions with defaulted extra parameters.  **On hold**: `[[operator=]]` from another type (`s = "hi"`): implicit
  conversions on assignment are where JavaScript-style surprises come from; `s.assign("hi")` stays.
- **Hot reload, the rest**: done: a `[[persist]]` global whose type changed moves field by field (when
  both programs import the runtime), maps and sets entry by entry; `C2_safe_reload` refuses a compile
  while a script function runs.  Open: `[[persist]]` without the runtime's reflection (the compiler could
  emit the tables for persisted types alone).
- **Sandbox, the rest** (S-M).  Done: `C2_sandbox(c2, 1)` / `c2 -sandbox`: deleting, renaming and writing
  files, starting programs and loading libraries fail (`EPERM`), nothing outside C²'s own C library
  table is found by name, and `exit()` / `abort()` return to the host (`C2_exited`).  Next:
  - **`C2_sandbox_dir(c2, "saves/")`** *(Committee)*: a folder sandboxed scripts may write in.  `fopen`
    for writing, `remove`, `rename` (both ends) and `mkdir` allowed for paths inside it, refused
    elsewhere.  A path is made absolute against the folder (a relative one is relative to it, not to the
    process's working directory), `.` and `..` resolved, and refused if it leaves the folder; links
    inside the folder resolved before the check (`realpath` / `GetFinalPathNameByHandle`), so a link
    can't point out.  Several folders: call it again.  Saves (`x.save(path)`, `write_whole`'s
    `path.tmp` + rename) then work in the sandbox, in that folder only;
  - `assert()` failures: unwind to the host as `abort()` does (today `__c2_assert_fail` aborts the process);
  - `-C` output: a sandboxed `c2_rt()` (a flag in the generated `c2_init`);
  - sockets and the network: outside C²'s table, so refused already; say so in the docs;
  - reading: limit `fopen` for reading to folders as well, if mods should not read the player's files.
  It stays a guard against mistakes and casual misuse; for hostile code (untrusted mods) the answer is
  a separate process, since C can write anywhere in its own.

### 2. Postponed
- **Reflection and serialization** (L), **in progress** *(Committee)*: A, B and C (JSON5, XML) done, D
  begun (hot reload); INI, CSV and msgpack postponed.  Every type is reflected: no
  `[[reflect]]` to opt in.  `[[skip]]` on a struct, union, enum or typedef leaves it out entirely
  (reflection knows nothing about it); on a field, the field is listed but marked, and serializers and
  `dump` leave it out.  Phases, in this order:
  - **A. Tables** (done).  `reflect(T)` / `reflect(expr)`, like `sizeof`: a `const c2type *` built by the compiler
    (exact sizes, offsets, bit-fields, arrays, nested structs, enum names).  Kinds of their own for
    `string`, `array(T)`, `map(K, V)` and `set(K)` (they become types the compiler can tell from a plain
    pointer), and each type's `[[destructor]]` and `[[copy]]`, so code walking a table can build and
    free owned values.  `for each_member(T, m)`, `enum_name(T, v)`, `x.print()`, and `f"{x}"` for a
    whole struct.  The host sees the same tables (`c2.h`).
  - **C. Serialization** (JSON5 and XML, saving and loading: done; INI, CSV, msgpack: postponed).
    `runtime/doc` reads JSON5 and XML documents by path, with no struct to load them into.  `runtime/serial`: a walk over the tables with a writer per format.  **Save
    first**: JSON5.  **Load only once saving is bulletproof**: it builds owned fields through their
    constructors and frees old values; fields match by name (missing: zero, unknown: skipped).
    `[[rename("hp")]]` on a field is its saved name.  Pointers aren't followed (owned strings and
    containers are).  Later formats: XML; INI (for historical reasons); CSV, which maps big
    collections well (an array, an array of structs: one row each); msgpack, as a transport (compact,
    binary, quick to write and read: network messages, IPC, caches).
  - **D.**  Done: hot reload moving a global field by field when its struct changes; compile-time loops
    over fields (`foreach_field(T, X)`, an X-macro the compiler writes); functions by name with their
    named and default arguments (`eval("heal(hero, 20)")`, `functions()`).  Open: a module's
    exported functions in the list; the host calling by name (`C2_eval`); variadic functions.
  - **B. By name and metadata** (done).  `global("player.pos.x")` (and the host's `C2_reflected`) for
    consoles, config files and editors; field attributes as metadata: `[[clamp(0, 100)]]` on numbers
    (values clamped on load), `[[clamp(16)]]` on strings, `[[comment("...")]]` (written as a comment
    by formats that have them).  `[[range]]` became `[[clamp]]`: it says what loading does with a value
    outside.  Later: `[[clamp]]` on a global (it has no c2field to carry it; today it is ignored), and
    calling functions by name (part of D).

  ```c
  typedef struct c2field { const char *name; const struct c2type *type; int offset, bits, bitpos, flags;
                           const char *key, *comment; double min, max; int maxlen; } c2field;   // flags: C2_SKIP C2_CLAMP
  typedef struct c2enum  { const char *name; long long value; } c2enum;
  typedef struct c2type {
      const char *name;          // "Player", "int", "float *", "char [8]", "array(int)"
      int kind, size, align;     // C2_INT C2_UINT C2_FLOAT C2_BOOL C2_PTR C2_ARRAY C2_STRUCT C2_UNION C2_ENUM C2_FUNC
                                 // C2_STRING C2_DYNARRAY C2_MAP C2_SET
      const struct c2type *of;   // pointee, element, enum's underlying type, a map's key
      const struct c2type *val;  // a map's value
      int count;                 // array length, fields, enum values
      const c2field *fields;     // struct / union
      const c2enum  *values;     // enum
      void (*free)(void *);      // [[destructor]], or NULL
      void (*copy)(void *dst, const void *src);   // [[copy]], or NULL
  } c2type;
  ```

- **Reflection and serialization: one API, `x.verb(args)`** *(Committee)*: **done** (Nov/2026): `any`,
  the `any` parameter rule, the `any_` method fallback, `T.f()` type calls; `x.print()`, `x.text()`,
  `x.json5()`, `x.xml()`, `x.save()`, `x.load()`, `x.parse()`, `x.find()`, `x.set()`, `global()`,
  `globals()`, `functions()`, `eval()`, `doc.open()`, `doc.parse()`, `d.number()` (see the CHANGELOG).
  Open: loading's messages name the type (`Hero.hp: ...`); the variable's own name (`hero.hp: ...`, as
  the old macros gave) would need the compiler to put the argument's text in the `any`.
- **Calls over many values** (S-M) *(Committee)*, **designed, not built** (Nov/2026; separate `x.free()` calls
  meanwhile).
  - **`free(a, b, c)`**, expanded by the compiler (no list, no reflection at run time): one free per
    argument, by its type: a `[[destructor]]` type → `T_free(&a)`; `array(T)`, `map`, `set` → their
    free; a plain pointer → C's `free(p)`.  Decided: every argument is set to NULL (or zeroed) after
    (C is risky land: safe defaults); a temporary is a compile error; something that owns nothing (an
    `int`, a struct without a destructor) is a compile error.  It also fixes a trap: C's `free()` on an
    `array(T)` or a map crashes today (their pointer is past a hidden header).  Open: `free(make_thing())`
    (an rvalue pointer that can't be set to NULL) in C code compiled as .c2: allowed, or an error.
  - **`print(...)` / `prints(...)`** (prints: a space between the arguments): each argument printed by
    its type (numbers and text directly, without the runtime; structs, enums, containers through
    reflection).  Proposed details: a character constant written in the call prints as text (`'a'`,
    `'ab'`), a stored `int` as a number; floats as f-strings print them (`%g`); no newline added
    (`x.print()` made the same); a user's own `print` wins; `fprint(stream, ...)`; `string(x)` (a
    constructor overload taking any value) as the conversion, `x.string()` with it, for today's `x.text()`.
  - **How**: by the compiler, as for free (chosen), or as a language feature, an `any...` parameter that
    collects the arguments as an array of `{type, address}` on the stack (Go's `...interface{}`, Odin's
    `..any`): any function can then be written that way, at the cost of run-time checks (16 bytes and a
    switch per argument: nothing next to printing).  Kept for later.
  - **User-declared fan-out** (an idea): `void log(int level, string msg, float t, ...);` declared with
    no body, and `log(lv, msg, t, A, B, C)` expanded to `A.log(lv, msg, t); B.log(lv, msg, t);
    C.log(lv, msg, t);`, each receiver finding its own `T_log` (or `any_log`).  `free(...)` and
    `print(...)` are this with no fixed arguments, so the compiler's `free` could be one such
    declaration in the runtime.  Uses: `update(dt, player, enemies, camera)`, `draw(screen, a, b, c)`,
    `reset(a, b, c)`, `save_all(file, ...)`.  The syntax can't be C's `...` alone (that is a C variadic,
    `printf`): an attribute marks it, e.g. `[[each]] void log(int level, string msg, float t, ...);`.
    The receivers come last, so the fixed arguments are evaluated once, before the first call.
    Spelling (Nov/2026): two dots as the last parameter, `void log(int level, string msg, float t, ..);`
    ("and a range of receivers", as `..` makes ranges in for loops; C's three dots stay C's variadic).
    No fixed parameters: `void free(..);`, `void print(..);`.  A body is the default for receivers
    without their own `T_log`; it names the receiver, an `any`: `void log(int level, string msg, float t,
    ..who) { fprintf(stderr, "%s\n", msg.data()); }`.  The risk: `..` and `...` differ by one
    character and mean different things, so the compiler catches the mix-ups it can see: `va_start` in
    a `..` function is an error ("did you mean ...?"), and so is a receiver without `T_log` when there's
    no body.  Not valid C for other compilers (a header shared with C: `#ifdef __C2__`).
  - **The spellings discussed** (Nov/2026, to decide later):
    1. `[[each]] void log(int level, string msg, float t, ...);`: an attribute before the function;
       C's `...` would then mean two things, told apart by the attribute.
    2. `void log(int level, string msg, float t [[...]]);`: an attribute after the last parameter; no
       spelling for no fixed parameters.
    3. `void log(int level, string msg, float t) [[...]];`: an attribute after the parameter list (where
       C23 puts the function type's attributes); `void free() [[...]];`.
    4. `void log(int level, string msg, float t, ..);`, with `..who` to name the receiver in a body:
       two dots, as ranges; `void free(..);`.  Risk: one dot from C's `...` (see above).
    5. `void log(int level, string msg, float t, any... who) { ... }`: a typed variadic.  Other languages
       read `T... name` as one call receiving a list (Java's `Object... args`, C#'s `params`, Go's
       `args ...any`, Odin's `..any`), so the body runs once and loops: `for (any w : who) w.log(level,
       msg, t);`, `w.log` found at run time through reflection (slower than direct calls; a missing
       method found at run time).  It can't be a typo for C's `...` (C never puts a type before it),
       and it generalizes: `int... nums` makes `sum(1, 2, 3)` see `nums.count`, `nums[i]`, `for (int n
       : nums)` (an array on the caller's stack).
    Suggested split, if both are wanted: `T... name` for typed variadic lists (one call), and the bare
    two dots (no body) for compile-time fan-out (direct calls, compile errors for a missing method):
    `free(..)` and `print(..)` declared that way.  Or `T... name` alone, with fan-out at run time.
- **Generics with `T` and `S`** (L) *(Committee)*, **proposed** (Nov/2026): templates as simple as
  `T add(T one, T two) { return one + two; }`.  `T` and `S` are the type parameters, by name: a .c2
  function whose signature uses them is generic, and each call makes (or reuses) the version for the
  types it passes (`add(1, 2)` → `add` for int, `add(1.5, 2.0)` → for double), as C++ templates do;
  the body is checked for each version.  `S` is a second, independent type: `S convert(T from)`,
  `void copy(T *to, const S *from, size_t n)`.
  - What to settle: reserved only where not declared (C code with `typedef struct T T;`, a macro `T`, a
    variable `S` keeps working), or always in .c2 (simpler to read, breaks such code); a call where T
    can't be deduced (`T zero()`): `zero(int)()`, or `T zero(T = int)`; `T add(T, T)` called with an
    int and a double: an error, or the usual conversions (then T is double); structs: `struct pair { T
    first; S second; };`, `pair(int, string)` as the type (as `map(K, V)` is today); methods
    (`T_add`) and overloads with a generic version: the specific one wins; where versions live in a
    module (made where called, merged by name), and in `c2 -C` output (named `add__int`).
  - Today's nearest: macros with `typeof`, and the runtime's `array(T)`, `map(K, V)`, `set(K)`.
- **More save formats** (M each) *(Committee)*, **postponed**: INI (for historical reasons: sections are
  the top-level struct's fields, keys their fields); CSV, which maps big collections well (an array of
  structs: a row each, a column per field); msgpack, as a transport (compact, binary, quick to write and
  read: network messages, IPC, caches).  Each is a writer and a reader over the same tables and the
  `runtime/doc` tree, as JSON5 and XML are.
- **Iteration** (M) *(Committee)*, **done** (Nov/2026, except glyphs: `text.glyphs()` needs Unicode's
  segmentation tables; and `[[unroll]]` on a for-each): one loop for everything, with an operator for ranges
  (`..`) and the loop variable's `*` for a pointer to each item.  Today: `array_foreach` / `map_foreach` /
  `set_foreach` macros and `for each_member(T, m)`.

  ```c
  for (int i : 0..n)            // 0, 1 ... n-1          (half-open, as C's loops are; n read once)
  for (int i : 1..=6)           // 1 ... 6               (..= includes the end)
  for (int i : n..0)            // nothing: a range counts up; down is (n - 1)..=0 by -1
  for (int i : 0..100 by 10)    // 0, 10 ... 90          (a step: `by`, a word, not an operator)
  for (char c : name)           // a string's chars: string, char [N] (up to the '\0'), const char *
  for (Item it : hero.bag)      // each item: array(T), T [N]; a view (an owned T isn't copied or freed)
  for (Item *it : hero.bag)     // a pointer to each item: change it in place; it - hero.bag is its index
  for (auto *e : scores)        // a map's entries: e.key, e.value (const key); a set's: e.key
  for (auto *k : scores.keys)   // keys alone; scores.values: the values alone
  for (auto m : Hero)           // a type: its members, const c2field * (as each_member; [[skip]] left out)
  ```

  - **Why this shape.**  `for (decl : expr)` reads as C++'s range-for and Java's, and `:` can't start
    anything else there (a for's first clause never has one), so the grammar stays unambiguous.  The
    pointer is C's own `*` in the declarator: no new operator to learn, and `auto *` (C23 `auto`) spares
    writing the type.  The value form of an owned type is a view, so a loop never copies strings by
    surprise; write `Item copy = it;` to copy.
  - **The range operator `..`** stands on its own: `case 1..5:` (GNU's `case 1 ... 5`), and later slices
    (`s[2..5]`, `bag[1..]`).  The lexer needs one rule: `0..10` is `0` `..` `10`, not `0.` `.10`.
  - **Meaning**, as plain C (what the compiler writes):
    `for (Item *it : bag)` → `for (Item *it = bag, *end_ = bag + array_count(bag); it < end_; it++)`;
    a map walks its entries array the same way; a range is `for (int i = a, end_ = b; i < end_; i += step)`.
    `break`, `continue`, `defer` and `return` work as in any loop.
  - **Changing the container while looping** (pushing to the array, adding to the map) moves its items:
    an error to catch.  `-g` could check that the count didn't change at each step.
  - **The four open questions, with the answers proposed** (Nov/2026):
    - strings walk **bytes** (`char c`), as C does; `name.chars()` is a view that walks UTF-8 code points
      (`for (int cp : name.chars())`), and `name.lines()`, `name.split(",")` views the same way later;
    - **no index syntax**: an array's index is `it - bag` (with `Item *it`); anything else counts itself
      (`int i = 0; for (...) { ...; i++; }`), or walks a range and indexes (`for (int i : 0..count)`);
    - **reverse** by a view, not a syntax: `reversed(bag)`, `reversed(0..n)` (n-1 down to 0);
    - **user types** by one method, `bool T_next(T *it, X *out)`: `for (X x : lines)` calls
      `Lines_next(&lines, &x)` until it returns false (an iterator object holds its own state; a
      container gives one with a method: `for (Node *n : tree.nodes())`).  Views (`chars()`, `keys()`,
      `reversed()`) are such iterator types themselves, written in the runtime.
  - **Feedback** (Nov/2026), and where it landed:
    - ranges both ways, the direction from the ends: `0..count` counts up, `count..0` down.  Half-open
      both ways, so `count..0` is count-1 down to 0: the same numbers as `0..count`, backwards (indices
      walk either way without -1s).  When an end is a variable, the direction is decided at run time,
      which can surprise (`0..n` with n = -2 walks 0, -1): to weigh against `reversed()`;
    - a step as a third part: `0..count..10` (from 0 to count, by 10); the step is a size, the direction
      still comes from the ends (`count..0..10` counts down by 10);
    - the loop variable's type picks how text is walked: `char` / `u8` bytes, `u32` / `char32_t` code
      points (decoded from their 1-4 bytes), anything else an error;
    - glyphs (grapheme clusters: é as e + accent, 👨‍👩‍👧‍👦 as 7 code points and 25 bytes) don't fit a number
      (`u64` holds 8 bytes, 2 code points): a glyph is a `string`, `for (string g : text.glyphs())`: with
      the 24-byte small-string buffer (23 bytes + '\0') almost every glyph lives inline, no allocation (the
      longest emoji sequences, 25+ bytes, go to the heap); the loop frees each one as it moves on.  The
      walk needs Unicode's segmentation tables (some 10-20 KB of data);
    - `for (Item it : bag)` is a copy that doesn't own (a bitwise copy: changing it doesn't change the
      array, and an owned member isn't copied or freed);
    - `scores.keys()`, `scores.values()` are iterable views.
  - **Ranges, settled** (Nov/2026, second round):
    - `from..to` is half-open as a set: `[low, high)`.  Ascending walks low up to high-1; descending
      walks high-1 down to low, so `count..0` is count-1 ... 0 (an array's indices, backwards), and
      `count+1..0..-1` includes count.  A descending step leaves out `from` the same way:
      `count+1..0..-10` is count, count-10 ... down to 0 at the lowest;
    - the step is the third part, signed: `0..count..10` (or `..+10`) ascends, `count..0..-10`
      descends.  The sign is the direction, whichever end is bigger; a step against the ends
      (`0..10..-1`) walks nothing, and with constant ends it's an error;
    - no step: `0..to` ascends (to below 0: nothing), `from..0` descends (from below 0: nothing),
      constant ends (`-30..30`, `30..-30`) go the way they point, read at compile time; two variable
      ends without a step (`a..b`) are an error that asks for one (`..1` or `..-1`): the direction is
      never guessed at run time;
    - the step is a nonzero constant (a variable step: a plain for loop, for now);
    - the loop is written so that unsigned counters work going down (`for (size_t i : n..0)`: tested
      before the decrement, never `i >= 0`);
    - characters are `u32` (`char32_t`): Unicode stops at U+10FFFF, 21 bits; `u64` isn't needed;
    - `for (Item it : bag)` copies (a copy that doesn't own); `Item *it` changes the items in place;
      `const Item *it` reads them without copying.
  - **Inclusive ends** (Nov/2026, third round): no `..=` for now.  Other languages split three ways
    (Python and Swift's `to:` run the first number and never the end: `10..0..-1` is 10 ... 1; Rust and
    D give the same numbers reversed: 9 ... 0; Kotlin's `downTo` and Julia include both ends: 10 ... 0).
    C² keeps "the same numbers, reversed"; an end is included by adding 1 to it: `0..10+1` is 0 ... 10,
    `10+1..0` is 10 ... 0.  `..=` (Rust, Odin) stays an option if `+1` keeps tripping people.
  - **How it looks**, all together:

    ```c
    for (int i : 0..count)                    // 0 ... count-1
    for (int i : 0..count..10)                // 0, 10, 20 ... below count
    for (int i : count..0)                    // count-1 ... 0
    for (char c : title)                      // bytes
    for (u32 c : title)                       // code points (UTF-8 decoded)
    for (string g : title.glyphs())           // glyphs (inline: 23 bytes fit)
    for (Item *it : hero.bag) it.n = 0;       // in place; it - hero.bag is the index
    for (Item it : hero.bag) total += it.n;   // a copy that doesn't own
    for (auto *e : scores) printf("%s %d\n", e.key, e.value);   // scores.keys(), scores.values() too
    for (int id : seen) ...                   // a set
    for (const c2field *f : reflect(Hero)) printf("%s\n", f.name);   // a type's fields, at run time
    for (string line : file.lines()) ...      // your own types: bool T_next(T *it, X *out)
    foreach_field(Hero, X)                    // (compile time: done, as an X-macro)
    ```

### 3. Open
- **Set theory on sets and maps** *(Committee)*, not now: when `A` and `B` are both sets (or both maps
  of the same types), `A + B` the union, `A - B` the difference (what is in A and not in B), `A ^ B`
  the symmetric difference (in one and not both), each a new set the caller frees; `A += B`, `A -= B`,
  `A ^= B` change `A` in place.  Today `s += k` / `s -= k` take one key, so the operator would pick by
  the right side's type (a set: the whole set; a key: one key).  Open: for maps, whose value wins in a
  union (B's, as `set` would), and whether `A & B` (the intersection) belongs with them; whether the
  results are new values (they own memory: `set(string) c = a + b;` must be freed) or only the `=`
  forms exist, to keep ownership obvious.
- **String interning** (S): `const char *intern(const char *text)` returns one shared copy per distinct
  text, so names compare by pointer (`if (tag == intern("enemy"))`) and live as long as the runtime (or
  until `intern_reset()`).  ~20 lines over `set(const char *)` in `runtime/map.c2`.  Open: whether interned
  strings survive a hot reload (they're heap, so yes, but the set itself is a script global: it needs
  `[[persist]]`, or to live in the host).
- **Custom allocators** (S): `rt_allocator(alloc, realloc, free, user)` routes the memory of `string`,
  `array`, `map` and `set` through the game's allocator (arenas, tracking, console ports).  Plain
  `malloc` in scripts stays the C library's.  Open: per-container allocators (an arena per level) or one
  global; and whether `C2_add_symbol(c2, "malloc", ...)` is already enough for most hosts.
- **Overloading, the rest**: done: by receiver.  Open: a function pointer initialized from an overloaded
  name could pick by its target type (`void (*f)(vec2 *) = load;`; today write `vec2_load`).  Then slices
  and range-for, and interfaces (`typedef { load, save } I;`) on top.
- **`[[packed]] struct x {};`** (S) **on hold**.  Unaligned access is only safe for plain loads and
  stores: on aarch64 (Apple Silicon included) atomics and some vector/pair instructions fault on
  unaligned addresses, and packed structs by value have to match clang on each ABI.  Test on
  macOS/aarch64 first (the abi tests can cover the by-value part).
- **`C2_load`, the rest**: headers that need more of the compiler extensions found in system and library
  headers (inline asm, `__builtin_*`, MSVC intrinsics), `long` on Windows (64-bit in C², 32-bit in
  MSVC-built libraries: a per-library `long` size, and a warning when such a header uses `long`), and
  scoping the macros of plain `.h` headers (`windows.h`), e.g. `#include "windows.h" private`.
- **Coroutines, the rest**: done: `runtime/coro` on minicoro (1 MB stacks, reloads refused while one is
  suspended).  Open: passing values through `co_yield(v)` / `co_resume` (minicoro's push/pop storage),
  and keeping the previous program alive until suspended coroutines finish, as an alternative to refusing.
- **More debugging** (M).  Done: `c2 -g` call stacks with crash reports, `#line` in `c2 -C` output.  Next:
  breakpoints and stepping in the interpreter through a hook per statement, and printing locals in a
  crash report (the compiler knows each frame's variables and their slots).
- **`[[operator=]]` as another spelling of `[[copy]]`** (S) *(Committee)*.  For C++ readers: same signature
  (`T T_copy(const T *)`) and the same meaning.  Not equivalent to C++'s `operator=`, though: `[[copy]]`
  also runs where no `=` is written (initialization, storing in a container, returning a member), not on
  every `=` (`b = f()` moves a temporary in), and the compiler destroys the old value itself, so the
  function never sees the target.  The name should make clear that it is the copy, not assignment.
- **Closures: nested functions that use the enclosing function's variables** (L) *(Committee)*.  Postponed;
  what would build on it: `try { } catch (e) { }` blocks (the block becomes a closure that `try` runs);
  `array_foreach` / `map_foreach` bodies that are functions; callbacks without a `void *arg` (`array_sort`
  with a comparator that reads locals, event handlers, timers); coroutines started from a block of code
  (`co_create({ ... })`); lazily computed defaults.  The main design question is lifetime: a closure that
  outlives its frame needs its captures on the heap (or copied, as C++'s `[=]`), and owning values in
  there bring the destructors in.
- **`try { } catch` blocks** (M) *(Committee)*.  `try(fn, arg)` / `throw(code)` are done (runtime/try); a block
  form needs the block lifted into a hidden function that sees the enclosing locals (a closure).
- **A Lua-like API** (M-L).
- **Allocation headers: `((void *)ptr)[-1]` holding size, capacity, type and a deallocator** (M), for
  memory that knows its size and a single routing `free`, as C²'s own allocation API (memory from the
  host or plain `malloc` has no header).  Then **zeroed heap allocations and zeroed growth** (S).
- **Native executables and DLLs** (M with [libtcc](https://bellard.org/tcc/)): today through `c2 -C`
  and a C compiler; libtcc could compile the `-C` output to an exe or DLL in-process.  Other back ends:
  [QBE](https://c9x.me/compile/), Cranelift, LLVM (each a large dependency).
- **Modules, the rest**: methods for private types (`x.f()` on a private struct looks for its internal
  name), messages that show `__m3_x` for private names in places, separate compilation of modules.
- **Captures and lambdas** (L).  Captured variables have to reach the inner function, which a plain C
  function pointer can't carry: a hidden struct plus a "fat" pointer breaks calls from C, trampolines
  need executable stacks.  Nested functions without captures are done.
- **`a["b"]` as `a.b`** (M) for constant strings (a compile-time member lookup); with a run-time string
  it is reflection.  `a[b]` as `a.b` clashes with indexing: dropped.
- **Local macros scoped to a loop or a function** (L for little gain): the preprocessor runs before
  scopes exist; file-scoped macros in `.h2` give most of the benefit.

- **`[[pure]]`, the rest** (S-M).  Done: `[[pure]]` (C23 `[[reproducible]]`, gcc's `pure`) and
  `[[unsequenced]]` (gcc's `const`) warn when a call's result is thrown away, and a pure function that
  writes a global or calls one that isn't pure is warned about.  Open: writes through a pointer
  parameter (`*out = x` in a pure function), calls through function pointers; then use it: a pure call with the same arguments twice in a row computed once, a pure call
  in a loop's condition hoisted, `-C` output carrying `__attribute__((pure))`.

### 4. From the reviews, not picked yet
Items from the two reviews (FEEDBACK1/2) that are still open, kept here so they aren't lost:
- **More tests** (postponed): one `.c2` test per pair of features used together (method call × default,
  × named arguments, function pointer × named, nested function × default...); differential testing
  against gcc with generated programs (Csmith/YARPGen over every `test.py` mode); a fuzzer that feeds
  arbitrary bytes to `C2_compile` under ASan (a game will compile text it didn't write).  Nov/2026 found
  four bugs no test caught (`C2_get` on bss globals, `c ? NULL : p`, `extern` function pointers, MIR's
  Windows lazy stub): the case for doing this before mods compile untrusted text.
- **Outside C test suites** (M each) *(Committee)*.  Today: c2m (MIR's), lacc, andrewchambers_c.  To add,
  each as a `tests/<name>/` suite in `test.py` (with a `.skip` per test we can't run, saying why).  Their
  licenses decide how: vendored with its LICENSE (as lacc is) when it allows it, else fetched by a script
  into a folder git ignores, and run from there.
  - [c-testsuite](https://github.com/c-testsuite/c-testsuite): single-file tests (`tests/single-exec/`,
    each with `.expected` stdout+stderr and `.tags`: c89/c99/c11, libc, preprocessor...), exit 0 on
    success.  The runner is MIT; the tests have their own `tests/LICENSE`, to read before vendoring.  By
    the same author as andrewchambers_c: expect overlap.  The easiest to add first.
  - [gcc's C tests](https://gcc.gnu.org/onlinedocs/gccint/C-Tests.html): `gcc.c-torture/execute` (programs
    that call `abort()` when wrong and `exit(0)` when right: thousands of small regressions, the best
    coverage of the four) and `gcc.c-torture/compile` (must compile); `gcc.dg` needs DejaGnu's comments
    read.  GPL: fetched, not vendored.  Many use GNU extensions and `__builtin_*`: skips by the hundred.
  - [llvm-test-suite](https://github.com/llvm/llvm-test-suite): `SingleSource/` (UnitTests, Regression,
    Benchmarks: one file each, with a `.reference_output`) is the part for us; `MultiSource/` (whole
    programs: sqlite3, lua...) later, as end-to-end tests and benchmarks.  Apache 2.0 with LLVM exception:
    vendorable with its license.
  - [Fujitsu's compiler-test-suite](https://github.com/fujitsu/compiler-test-suite): C, C++ and Fortran;
    the `C/` part, run through lit; reference outputs are still partly missing (some tests only check
    that they run).  Apache 2.0 with LLVM exception.
  - [CompCert-small-tests](https://github.com/AbsInt/CompCert-small-tests): `c/` (benchmarks with known
    outputs), `regression/` and `abi/` (calling-convention tests across compilers: compare with c2-abi),
    plus programs (raytracer, compression, spass).  GPL-2: fetched, not vendored.
- **`-T` (text) mode**: MIR's text reader rejects how C² writes float data and negative numbers
  (`data operand is not of data type`, `no number after a sign -`): 6 lacc and 3 c2m tests fail in
  `text` mode only.  A debugging path (C² prints MIR as text, MIR reads it back); scripts never use it.
- **API polish**: `C2_undef` / `C2_reset_options`, a `C2_run_main` that tells "no
  main" apart from a result of -1, and structured diagnostics through a callback (editors, mod tools).
  (The restricted mode is done: `C2_sandbox`.)
- **Docs**: say near the top of the README that C² is not a security sandbox, `C2_sandbox` included (native code, full process access),
  lead with the embedding use case, and add a short limitations box (64-bit only, one translation unit,
  built-in headers, `long` is 64-bit); say that `C2_load`'s `header` is a path.
- **Prelude names**: the always-included headers declare names games use (`y0 y1 j0 j1 jn yn`, `gamma`,
  `index`, `time`...): drop the obscure ones from the `.c2` prelude, and let a file-scope definition
  shadow a prelude declaration the file never uses.  Plus a regression test for struct names that are
  also a function's name (`struct stat` and `stat()`).

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
