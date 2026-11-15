# The C² language

C² is C with a set of additions that make everyday programming shorter and safer, without leaving C
behind. A C² program is a C program: the same types, the same memory, the same calling convention, the
same libraries. What C² adds is mostly about saying things once: no headers to include for the basics,
no `struct` keyword everywhere, functions that know their defaults, values that clean up after
themselves.

This book teaches the language in order. Each chapter uses only what the chapters before it showed,
plus one new idea, and every complete example in it was compiled and run to produce the output shown
under it. The same progression, one feature per file, is in [tour/](../tour/).

The [README](README.md) covers the rest: how to run `c2`, how to embed it in a program, which parts of
standard C are supported, and how C² is built and tested.

**Contents**

- Part I, C made convenient: [1. A first program](#1-a-first-program) ·
  [2. C23, everywhere](#2-c23-everywhere) · [3. Headers and modules](#3-headers-and-modules) ·
  [4. Types and data](#4-types-and-data) · [5. Blocks and functions](#5-blocks-and-functions)
- Part II, functions and types: [6. Arguments](#6-arguments) · [7. Methods](#7-methods) ·
  [8. Constructors](#8-constructors) · [9. Owning values](#9-owning-values) · [10. Operators](#10-operators)
- Part III, the runtime: [11. Strings](#11-strings) · [12. Arrays, maps and sets](#12-arrays-maps-and-sets) ·
  [13. Coroutines](#13-coroutines) · [14. Errors: try and throw](#14-errors-try-and-throw)
- Part IV, programs: [15. Reflection](#15-reflection) · [16. Debugging](#16-debugging) ·
  [17. C libraries](#17-c-libraries) · [18. Attributes](#18-attributes)
- [Appendix: C and C² side by side](#appendix-c-and-c²-side-by-side)

---

# Part I: C made convenient

## 1. A first program

C² compiles two kinds of files. A `.c` file is plain C: C² compiles it as a C compiler would, so
existing code keeps working. A `.c2` file is C², with everything in this book turned on.

```c
int main() {
    char *s = strdup("no #include needed");
    printf("%s, sqrt(2) = %.3f\n", s, sqrt(2));
    free(s);
}
```

Output:

```text
no #include needed, sqrt(2) = 1.414
```

Two things are missing compared with C, on purpose:

- **No `#include`s for the basics.** A `.c2` file always sees `<stdio.h>`, `<string.h>`,
  `<stdlib.h>`, `<math.h>`, `<stdint.h>` and `<stddef.h>`. Other headers are included as usual.
- **`int main()` means what it says.** In C² (as in C23 and C++), an empty parameter list means "no
  parameters", the same as `(void)`. Calling `main(1)` or `answer(1)` below is an error, where C would
  quietly accept it:

```c
int answer() { return 42; }
int main() {
    printf("%d\n", answer());   // answer(1) wouldn't compile
}
```

Output:

```text
42
```

Run a program with `c2 file.c2`. Its `main` runs right away (C² compiles to machine code in memory,
then calls it) and its result is the exit code.

## 2. C23, everywhere

C23 is the latest C standard, and C² has most of it, in `.c` files too. If you learned C before 2023,
these are the new everyday tools:

```c
enum dir : unsigned char { NORTH, EAST, SOUTH, WEST };   // an enum with a fixed type
int main() {
    constexpr int SIZE = 0b1000;            // a named constant (binary literal), usable as an array size
    int grid[SIZE] = {};                    // {} zeroes everything
    auto speed = 2.5;                       // the type comes from the value: double
    typeof(speed) twice = speed * 2;        // the type of an expression
    bool ok = twice == 5.0 && sizeof grid == 32;   // bool, true and false are keywords
    static_assert(sizeof(enum dir) == 1);   // the message is optional
    int *none = nullptr;
    printf("%s %d %d\n", ok ? "true" : "false", 1'000'000, none == NULL);   // ' separates digits
}
```

Output:

```text
true 1000000 1
```

The preprocessor learned new tricks too: `#embed` pastes a file's bytes into an initializer,
`__VA_OPT__(x)` produces `x` only when a variadic macro got arguments, and `#elifdef` / `#elifndef`
shorten `#elif defined(...)`. C² also defines `__C2__` always, and `__C2_EXT__` in `.c2` files, so a
header shared with other compilers can tell them apart:

```c
// file: embed_me.txt
hello from a file
```

```c
#define LOG(fmt, ...) printf(fmt "\n" __VA_OPT__(,) __VA_ARGS__)
#ifndef __C2_EXT__
#error "save me as .c2"
#endif
int main() {
    static const char text[] = {
#embed "embed_me.txt"
    , 0 };
    LOG("embedded: %s", text);
    LOG("no arguments: __VA_OPT__ dropped the comma");
#ifdef __C2__
    LOG("compiled by C2");
#elifdef __GNUC__
    LOG("compiled by gcc");
#endif
}
```

Output:

```text
embedded: hello from a file

no arguments: __VA_OPT__ dropped the comma
compiled by C2
```

**Multi-character constants.** C has always allowed `'ab'`, a character constant with several
characters, but left its value up to each compiler. C² defines it: the characters are packed in source
order, so `'AB'` is `0x4142` everywhere, and a `.c2` file allows up to 8 of them (a `long long`). They
make readable tags, and since they are constants they work as `case` labels:

```c
int main() {
    long long tag = 'SAVEGAME';
    printf("%llx\n", tag);
    switch (tag) { case 'SAVEGAME': puts("a save game"); }
}
```

Output:

```text
5341564547414d45
a save game
```

## 3. Headers and modules

A program soon outgrows one file. C² keeps C's `#include` and makes headers keep their macros to
themselves, and adds modules, which keep their names to themselves.

### Macros that stay home

In C, a header's macros leak into every file that includes it: a header's `MIN` breaks your own `MIN`.
In C², macros defined in an included `.h2` (or `.c2`) file are visible only in that file and in the
expansions of its own macros. `#export NAME` (or `#export define NAME ...`) makes one visible
everywhere:

```c
// file: minmax.h2
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define CLAMP(x, lo, hi) MIN(MAX(x, lo), hi)
#export CLAMP
```

```c
#include "minmax.h2"
int main() {
#ifndef MAX
    printf("%d, and MAX isn't defined here\n", CLAMP(42, 0, 10));
#endif
}
```

Output:

```text
10, and MAX isn't defined here
```

Include guards keep working (a header sees its own guard). Plain `.h` files behave as in C.

### `#import`

`#import "dir/name"` brings in `dir/name.c2` once, however many times it is imported. It is searched
next to the importing file, in `-I` folders, and next to `c2`. A module can also be a folder: without a
`dir/name.c2`, C² takes `dir/name/name.c2`, then `dir/name/index.c2`. That is how `#import "runtime"`
finds `runtime/runtime.c2`. In a module, only what is marked
`[[export]]` (a C23 *attribute*: chapter 4 has more) is public: functions, globals, structs, unions, enums and typedefs. Everything else is
private to the module, so two modules can each have a helper called `clamp_` without clashing, and
`static` isn't needed.

```c
// file: lib/greet.c2
static int calls;                                    // private, like everything not exported
const char *greeting(void) { return "hello"; }       // private
[[export]] void greet(const char *who) { calls++; printf("%s, %s (%d)\n", greeting(), who, calls); }
```

```c
#import "lib/greet"
#import g = "lib/greet"                      // imported once: g = only adds a name for it
const char *greeting(void) { return "hi"; }  // ours: the module's private greeting() doesn't clash
int main() {
    greet("plain");
    greet.greet("qualified");                // module.name
    g.greet("aliased");
    printf("%s\n", greeting());
}
```

Output:

```text
hello, plain (1)
hello, qualified (2)
hello, aliased (3)
hi
```

A module's names can be written qualified, `module.name`, with the module's file name or with a name of
your own: `#import g = "lib/greet"`. The name comes first, so a list of imports lines up by what you
type.

Exported names keep their C names, so the host and C code see them unchanged. Prototypes of C
functions stay public. A module's overloads of one name are all exported or all private.

A module's macros stay inside it too, unless `#export`ed, as a header's do.

## 4. Types and data

### Short names for sized types

`i8`, `i16`, `i32`, `i64` and `u8` ... `u64` are `int8_t` ... `uint64_t`; `f32` and `f64` are `float`
and `double`. The longer `int8` ... `uint64`, `flt32` and `flt64` are spelled out the same way. A
program's own `typedef ... i32;` still works.

```c
int main() {
    i8 a = -1; u16 b = 65535; i64 c = 1LL << 40; f32 d = 0.5f;
    printf("%d %u %lld %g\n", a, b, c, d);
}
```

Output:

```text
-1 65535 1099511627776 0.5
```

### Locals start at zero

In C, a local without an initializer holds whatever was in that memory, and reading it is a classic
bug. In C², every local starts zeroed: numbers are 0, pointers NULL, structs and arrays all zeros.

```c
struct Vec { float x, y; };
int main() {
    int n; char *p; Vec v; int a[4];
    printf("%d %d %g %d\n", n, p == NULL, v.y, a[3]);
}
```

Output:

```text
0 1 0 0
```

A `goto` that jumps past such a declaration into its scope gets a warning, since the jump skips the
zeroing.

Zeroing costs time on big buffers you are about to fill anyway. `[[unclear]]` leaves one alone
(`[[indeterminate]]` is the same, under its C++26 name):

```c
int main() {
    [[unclear]] char buf[1 << 16];   // 64 KB, not cleared
    snprintf(buf, sizeof buf, "filled by hand");
    puts(buf);
}
```

Output:

```text
filled by hand
```

That `[[...]]` is a C23 **attribute**, like the `[[export]]` of chapter 3. Two more are useful every day: `[[nodiscard]]` warns when a
call's result is ignored, and `[[deprecated("why")]]` warns at each call. Warnings don't stop the
program. Several attributes can share one list: `[[export, nodiscard]]`.

```c
[[nodiscard]] int must_use(void) { return 1; }
[[deprecated("use new_api")]] void old_api(void) {}
int main() {
    must_use();   // warning: the result is discarded
    old_api();    // warning: deprecated
    puts("still runs");
}
```

Output:

```text
example.c2:4: warning: the result of 'must_use' is discarded ([[nodiscard]]; (void) to ignore it)
example.c2:5: warning: 'old_api' is deprecated: use new_api
still runs
```

`[[unroll]]` before a `for`, `while` or `do` compiles its body several times in a row, so the loop
jumps back once per few iterations instead of every time. `[[unroll(N)]]` makes N copies;
plain `[[unroll]]` copies the body once per iteration when the loop counts between constants (up to 64
times), and 4 times otherwise. The condition is still tested between the copies, so any number of
iterations works, and `break` and `continue` too. It pays on short bodies in hot loops (summing an
array went 25% faster with `[[unroll(4)]]`); on big bodies it only makes the code bigger.

```c
int main() {
    int a[10] = {3, 1, 4, 1, 5, 9, 2, 6, 5, 3}, sum = 0;
    [[unroll(4)]] for (int i = 0; i < 10; i++) sum += a[i];   // 4 copies; 10 isn't a multiple: fine
    int dot = 0;
    [[unroll]] for (int i = 0; i < 3; i++) dot += a[i] * a[i + 3];   // 3 copies, one per iteration
    printf("%d %d\n", sum, dot);
}
```

Output:

```text
39 44
```

A body that is copied can't hold what must exist once: a label, a `static` variable or a function
defined inside it is an error under `[[unroll]]`.

### Arrays that know their length

`countof(a)` is the number of elements of an array (from C2y, the next C standard). Unlike the old
`sizeof a / sizeof a[0]`, it refuses a pointer instead of returning a wrong number:

```c
int main() {
    int primes[] = {2, 3, 5, 7, 11};
    for (int i = 0; i < countof(primes); i++) printf("%d ", primes[i]);
    puts("");
}
```

Output:

```text
2 3 5 7 11 
```

In C, an array parameter like `int v[3]` is just a pointer: any array, or any pointer, can be passed.
In a `.c2` file, `int v[3]` takes exactly an array of 3; another length, or a pointer, is a compile
error. C's `v[static 3]` ("at least 3") keeps its meaning:

```c
int sum3(int v[3]) { return v[0] + v[1] + v[2]; }
int sum_any(int v[static 3]) { return v[0] + v[1] + v[2]; }
int main() {
    int three[3] = {1, 2, 3}, four[4] = {10, 20, 30, 40};
    printf("%d %d\n", sum3(three), sum_any(four));   // sum3(four) wouldn't compile
}
```

Output:

```text
6 60
```

This only applies to declarations written in `.c2` (or `.h2`) files: a plain `.h` header keeps C's
meaning, so C libraries are unaffected.

## 5. Blocks and functions

### Blocks with a value

`({ ... })` is a block that is also an expression: its value is the value of its last statement. It
comes from GCC and is handy in macros and for computing a value in steps:

```c
int main() {
    int sq = ({ int t = 11; t * t; });
    printf("%d\n", sq);
}
```

Output:

```text
121
```

### Functions inside functions

A function can be defined inside another one, next to where it is used. It is visible in the
enclosing block and can call itself, but it can't use the enclosing function's variables (it is not a
closure):

```c
int main() {
    int square(int x) { return x * x; }
    int fact(int n) { return n <= 1 ? 1 : n * fact(n - 1); }
    printf("%d %d\n", square(7), fact(5));
}
```

Output:

```text
49 120
```

### `defer`: cleanup next to the setup

`defer statement;` runs the statement when the enclosing block ends, however it ends: at the closing
brace, a `return`, a `break` or a `continue`. Several defers run in reverse order. The value of a
`return` is computed before the defers run.

```c
int lines(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    defer fclose(f), puts("closed");     // runs at every return below
    int n = 0;
    for (int c; (c = fgetc(f)) != EOF;) n += c == '\n';
    return n;
}
int main() {
    printf("%d lines\n", lines(__FILE__));
}
```

Output:

```text
closed
11 lines
```

The same works in `.c` files as `_Defer` (it is ISO's technical specification for C). It can't be
combined with `goto` in the same function.

### Recursion without a stack

`return f(...)` inside `f` itself is a *tail call*: nothing is left to do after it. C² turns it into
a loop, so tail recursion runs in constant stack at any depth. When it can't (a local's address was
taken, a variable-length array is in scope, the function is variadic or passes structs), it warns and
keeps the call; `[[musttail]]` makes that an error instead:

```c
long sum_to(long n, long acc) {
    if (n == 0) return acc;
    [[musttail]] return sum_to(n - 1, acc + n);
}
int main() {
    printf("%ld\n", sum_to(10000000, 0));   // ten million calls deep, as a loop
}
```

Output:

```text
50000005000000
```

### Overloading

Functions can share a name when their first parameters have different types. The first argument picks
one:

```c
void show(int x) { printf("int %d\n", x); }
void show(const char *s) { printf("text %s\n", s); }
int main() {
    show(42);
    show("hi");
}
```

Output:

```text
int 42
text hi
```

Only the first parameter counts: it is overloading by *receiver* (in chapter 7, the value a method is
called on). The
match is exact (`T`, `T *` and `const T *` are one receiver), or after the integer promotions (a `short`
finds the `int` one). Each function gets a symbol of its own, `<receiver>_<name>`: here `int_show` and
`str_show`. That is the name the host program (`C2_get`) and function pointers use; a bare `show` with
no call is an error that lists them. A C library function keeps its own name next to overloads of it:
`abs(-3)` is C's, `abs(v)` your vector's.

---

# Part II: functions and types

## 6. Arguments

### Defaults

A parameter can have a default value, used when a call leaves the argument out. The default is an
expression, evaluated at each such call, and it can use the parameters before it:

```c
int box(int w, int h = 10, int d = w * 2) { return w * h * d; }
int main() {
    printf("%d %d %d\n", box(2), box(2, 3), box(2, 3, 4));
}
```

Output:

```text
80 24 24
```

Once a parameter has a default, the ones after it need one too. A later declaration may repeat a
default but not change it.

### Named arguments

With defaults, you often want to set one parameter and leave the others alone. Name it: `name: value`
(Swift style) or `.name = value` (like a designated initializer), after any positional arguments.

```c
void spawn(const char *what, int x = 0, int y = 0, int count = 1) {
    printf("%d %s at %d,%d\n", count, what, x, y);
}
int main() {
    spawn("orc", y: 7);
    spawn("bat", count: 3, x: 5);
    spawn("slime", .y = 2);
}
```

Output:

```text
1 orc at 0,7
3 bat at 5,0
1 slime at 0,2
```

This makes parameter names part of a function's interface: renaming one breaks the calls that use
it, and C² warns when two declarations of a function disagree on names.

### Defaults on function pointers

A function pointer type can carry defaults of its own, so the same function can be called two ways:

```c
int hit(int damage, int crit) { return damage * crit; }
int main() {
    int (*strike)(int damage, int crit = 3) = hit;
    printf("%d %d\n", strike(10), strike(10, 5));
}
```

Output:

```text
30 50
```

## 7. Methods

`x.f()` calls `f(x)`. It is only notation, with no hidden object model, so it works on any value,
literals included. After a number's `.`, a name that can't continue the number is a method: `21.twice()` is `twice(21)` (an `int`), `2.5.half()` is `half(2.5)`. `21.f()` is the
float literal `21.f` followed by `()`, an error; write `(21).f()`.

```c
int twice(int x) { return 2 * x; }
size_t chars(const char *s) { return strlen(s); }
int main() {
    printf("%d %zu %d\n", 21.twice(), "hello".chars(), 3.twice().twice());
}
```

Output:

```text
42 5 12
```

With structs, methods become a way to organize code. Two conveniences come first.

### A struct's name is a type

In C, `struct Vec { ... };` is used as `struct Vec v;`, which is why so much C code adds
`typedef struct Vec Vec;`. In C² the name works directly, as in C++ (unless a variable, function or
enumerator has the same name). The same goes for unions and enums:

```c
struct Vec { float x, y; };
enum Dir { NORTH, EAST, SOUTH, WEST };
int main() {
    Vec v = {3, 4};
    Dir d = EAST;
    printf("%g %g %d\n", v.x, v.y, d);
}
```

Output:

```text
3 4 1
```

### `.` on pointers

C has two member operators: `v.x` for a struct, `p->x` for a pointer to one. C² accepts `.` for both,
so moving code between values and pointers doesn't mean changing every access (`->` still works):

```c
struct Vec { float x, y; };
void grow(Vec *p) { p.x *= 2; p.y *= 2; }
int main() {
    Vec v = {3, 4};
    grow(&v);
    printf("%g %g\n", v.x, v.y);
}
```

Output:

```text
6 8
```

### Methods on structs

`x.f(a, b)` calls `f(x, a, b)` when `x` has no member called `f`. If `f` takes a pointer and `x` is
not one, `&x` is passed, so the method can change `x`:

```c
struct Counter { int n; };
void add(Counter *c, int k) { c.n += k; }
int main() {
    Counter c = {0};
    c.add(5);
    c.add(2);
    printf("%d\n", c.n);
}
```

Output:

```text
7
```

`c.add(5)` is exactly `add(&c, 5)`.

### Methods that belong to a type

Many types want a `len()` or a `print()`. On a struct or union `T` (or a pointer to one), `x.f()`
looks for `T_f()` first, then `f()`. So every type can have its own methods without their names
clashing:

```c
struct Vec { float x, y; };
float Vec_len(Vec v) { return sqrtf(v.x * v.x + v.y * v.y); }
int main() {
    Vec v = {3, 4};
    printf("%g\n", v.len());   // Vec_len(v)
}
```

Output:

```text
5
```

A member wins over a function: if `x` has a member `len` (a function pointer), `x.len()` calls it,
and C² warns if a `len()` or `T_len()` also exists.

Overloads (chapter 5) are methods too: with the two `show` functions, `"hi".show()` calls `str_show`.

## 8. Constructors

`T(...)` builds a `T` from its fields, in order or by name. Missing fields are zero:

```c
struct vec3 { float x, y, z; };
int main() {
    vec3 a = vec3(1, 2, 3), b = vec3(z: 9), c = vec3();
    printf("%g %g %g | %g %g %g | %g\n", a.x, a.y, a.z, b.x, b.y, b.z, c.x);
}
```

Output:

```text
1 2 3 | 0 0 9 | 0
```

It's the same as C's compound literal `(vec3){1, 2, 3}`, with the named-argument syntax of chapter 6.
`T(...)` works for structs and unions, nested (`rect(vec3(0, 0, 0), hi: vec3(1, 1, 1))`), and through a
typedef's name: after `typedef struct { float x, y, z; } Vec3;`, `Vec3(1, 2, 3)` works too. The
`name: value` form also works in `{ }` initializers: `Vec3 v = {z: 3};`.

When building a `T` needs more than filling fields, write a function named like the type that returns
one. It becomes `T`'s constructor, with defaults and named arguments:

```c
struct vec3 { float x, y, z; };
vec3 vec3(float x = 1, float y = 2, float z = 3) { return (vec3){x, y, z}; }
int main() {
    vec3 v = vec3(z: 30);
    printf("%g %g %g\n", v.x, v.y, v.z);
}
```

Output:

```text
1 2 30
```

`T(x)` where `x` is already a temporary `T` is just `x`, so a constructor can wrap something that
already made a `T`: `string(f"{n} hp")` (chapter 11).

A constructor function also converts: where a `T` is expected and the value isn't one, C² calls
`T(value)`, when the constructor's first parameter takes that value and the others have defaults. That
holds for initializers, arguments and `return`:

```c
struct meters { double v; };
meters meters(double v, double scale = 1) { return (meters){v * scale}; }
double twice(meters m) { return m.v * 2; }
int main() {
    meters m = 3.5;                          // meters(3.5)
    printf("%g %g\n", m.v, twice(10));       // twice(meters(10))
}
```

Output:

```text
3.5 20
```

A value is never converted between a number and a pointer this way, and a struct with no constructor
function isn't converted at all: `vec3 v = 1;` is still an error.

## 9. Owning values

Some values own something: a heap buffer, a file, a texture. In C, freeing them is entirely up to
you, and two classic bugs follow: forgetting to free (a leak) and freeing twice (a crash), usually
because two copies of the value point at the same memory. C² gives a type three tools to handle this,
and the rest of the language uses them: a destructor, a copy function, and the rule that temporaries
clean themselves up.

### Destructors and temporaries

`[[destructor]]` on a function `void T_free(T *)` makes it `T`'s destructor. From then on, a `T`
returned by a function and not kept anywhere is a **temporary**: C² destroys it at the end of the
statement. A `T` you keep in a variable is yours, to free when you are done:

```c
struct buf { char *p; };
buf buf(const char *s) { return (buf){strdup(s)}; }
[[destructor]] void buf_free(buf *b) { printf("free %s\n", b.p); free(b.p); b.p = NULL; }
int main() {
    printf("%s\n", buf("temporary").p);   // printed, then freed right after this line
    buf kept = buf("kept");               // stored in a variable: not freed for you
    printf("%s\n", kept.p);
    kept.free();                          // buf_free(&kept): a type method (chapter 7)
}
```

Output:

```text
temporary
free temporary
kept
free kept
```

The precise rules:

- A call returning a `T` by value makes a temporary. It is destroyed at the end of its statement: an
  expression statement, each declarator of a declaration, an `if` / `while` / `for` / `switch`
  condition, a `for` step. Several are destroyed newest first.
- A temporary is **moved**, not destroyed, when it is stored (`T x = f();`, `x = f();`, a member, an
  array element), returned, or is the value of a `({ })`. A `?:` whose two branches are temporaries is
  a temporary; mixing a temporary with a named `T` is an error, since C² couldn't tell which one to
  free.
- Assigning a temporary to a variable destroys the variable's old value first, so `s = s + "!"` doesn't
  leak.
- Passing a temporary to a function *lends* it: the function may use it but must not keep it, because
  it is freed when the statement ends.
- Named variables are never destroyed for you. C² doesn't track a variable's last use, so it can't know
  when you are done with it.

### Copies

With destructors alone, `buf b = a;` copies `a` byte by byte: both now point at the same memory, and
freeing both frees it twice. `[[copy]]` on a function `T T_copy(const T *)` tells C² how to copy a `T`
properly, and then storing a *named* `T` calls it:

```c
struct buf { char *p; };
buf buf(const char *s) { return (buf){strdup(s)}; }
[[destructor]] void buf_free(buf *b) { free(b.p); b.p = NULL; }
[[copy]] buf buf_copy(const buf *b) { return buf(b.p); }
int main() {
    buf a = buf("hello");
    buf b = a;                      // a copy, with memory of its own
    b.p[0] = 'j';
    printf("%s %s\n", a.p, b.p);
    a.free(); b.free();             // each frees its own
}
```

Output:

```text
hello jello
```

A `T` is copied when a named one (a variable, a member, `*p`, an array element) is:

- used to initialize or assign another (`T b = a;`, `b = a;`; the old `b` is destroyed first, and
  `a = a;` is safe);
- stored in a member or a container (`array_push(v, a)` puts a copy in the array);
- returned when it isn't the function's own local (`return p.name;` hands out a copy, never the member
  itself).

And moved, not copied, when it is a temporary, a fresh `(T){...}` or `T(...)`, the value of a
`({ })` (which is how `array_pop` hands an element out), or the function's own local being returned.

**Why not `operator=`?** In C++, the same job takes two functions, a copy constructor for `T b = a;`
and an `operator=` for `b = a;`, and the second must free the old value and survive `a = a`. In C²,
the compiler does "destroy the old value, then move the copy in" itself, so one plain function covers
both. It also runs where no `=` is written (putting a value in a container, returning a member), and
doesn't run for every `=` (`b = f()` moves). A `[[copy]]` describes what copying a `T` *means*; the
compiler decides where that is needed.

### Structs that own things

Most owning types are built from other owning types: a `player` with a `string` name and an inventory
of items with names. Writing `player_free` and `player_copy` by hand for each of them is tedious and
easy to get wrong. C² writes them for you: a struct whose members have a destructor gets
`T_free`, which frees those members last to first, and, if all of them have a copy, `T_copy`, which
copies them (the other members are copied byte by byte). Nested structs and fixed-size arrays of them
are included:

```c
struct buf { char *p; };
buf buf(const char *s) { return (buf){strdup(s)}; }
[[destructor]] void buf_free(buf *b) { free(b.p); b.p = NULL; }
[[copy]] buf buf_copy(const buf *b) { return buf(b.p); }

struct hero { buf name; buf title; int hp; };    // gets hero_free and hero_copy
int main() {
    hero a = {buf("Ada"), buf("the Bold"), 100};
    hero b = a;                                   // hero_copy: copies both bufs
    b.name.p[0] = 'E';
    printf("%s %s | %s %s\n", a.name.p, a.title.p, b.name.p, b.title.p);
    a.free(); b.free();                           // hero_free: frees both bufs
}
```

Output:

```text
Ada the Bold | Eda the Bold
```

A struct that defines its own `T_free` or `T_copy` keeps them. Unions are left alone (C² can't know
which member is in use), as is a struct declared before its members' destructors.

## 10. Operators

`[[operator+]]` on a function makes `+` call it when an operand is a struct or union:

```c
struct vec2 { float x, y; };
[[operator+]] vec2 vec2_add(vec2 a, vec2 b) { return vec2(a.x + b.x, a.y + b.y); }
[[operator*]] vec2 vec2_scale(vec2 a, float k) { return vec2(a.x * k, a.y * k); }
[[operator==]] bool vec2_eq(vec2 a, vec2 b) { return a.x == b.x && a.y == b.y; }
int main() {
    vec2 v = vec2(1, 2) + vec2(3, 4) * 2;
    printf("%g %g %d %d\n", v.x, v.y, v == vec2(7, 10), v != vec2(7, 10));
}
```

Output:

```text
7 10 1 0
```

The operators are `+ - * / % == < <= > >= [] += -= *= /= %=` and unary `-` (an `[[operator-]]` with
one parameter). Some come for free:

- `a != b` is `!(a == b)`.
- Comparisons that aren't declared come from `<`: `a > b` is `b < a`, `a <= b` is `!(b < a)`, `a >= b`
  is `!(a < b)`. One `[[operator<]]` is enough to sort.
- `a += b` without an `[[operator+=]]` is `a = a + b`.

How a function is picked: the struct operand must match its parameter exactly (`T`, `T *` or
`const T *`; a pointer parameter gets `&a`), and the other operand converts as in a call, except that
numbers and pointers never stand in for each other. When two functions fit equally well it's an
error. Parameters after the two operands may have defaults.

A number on the left finds the function written for the number on the right: `2 * v` calls
`vec2_scale(v, 2)`, and the same goes for `+` and `==`. Those operators are commutative for numbers
and vectors. When yours isn't, mark it `[[noncommutative]]`, and `2 * m` becomes an error that says
so (the other way round, `m * 2`, still works):

```c
struct vec2 { float x, y; };
[[operator*]] vec2 vec2_scale(vec2 v, float k) { return vec2(v.x * k, v.y * k); }
[[operator+]] vec2 vec2_offset(vec2 v, float k) { return vec2(v.x + k, v.y + k); }
int main() {
    vec2 v = 2 * vec2(1, 2) + 1;        // vec2_offset(vec2_scale(vec2(1, 2), 2), 1)
    vec2 w = 0.5f + v;
    printf("%g %g | %g %g\n", v.x, v.y, w.x, w.y);
}
```

Output:

```text
3 5 | 3.5 5.5
```

Only a number and a struct swap places this way. Two structs of different types (a matrix and a
vector) never do: `m * v` and `v * m` are different functions.

`[[operator[]]]` returning a pointer makes `v[i]` an lvalue, so `v[i] = x` works:

```c
struct grid { int cells[4]; };
[[operator[]]] int *grid_at(grid *g, int i) { return &g.cells[i & 3]; }
int main() {
    grid g = {};
    g[1] = 5;
    g[6] += 2;          // i & 3: cell 2
    printf("%d %d %d\n", g[1], g[2], g[0]);
}
```

Output:

```text
5 2 0
```

Operators never change what C operators do on other types, and the functions keep their names, so C
code and the host can call them.

---

# Part III: the runtime

The runtime, `runtime/`, is a set of modules written in C²: strings, containers, coroutines and
`try`/`throw`. `#import "runtime"` brings in all of them, or import one: `#import "runtime/string"`. They are
ordinary C² code built on the features of the previous chapters, so they are also examples of how to
write such code. The `c2` command finds `runtime/` next to its executable; a program that embeds C² passes
that folder with `C2_include_dir`.

## 11. Strings

`string` holds text of any length. Up to 23 bytes live inside the `string` itself; longer text moves to
the heap (the *small-string optimization*). A zeroed `string` is empty.

```c
#import "runtime"
int main() {
    string s = "hello";                             // string("hello"): the constructor converts
    s += ", world";
    printf("%s %zu %d %s\n", s.data(), s.len(), s == "hello, world", s.is_long() ? "heap" : "inline");
    s.append(", now long enough for the heap").appendf(" (%d)", 2026);   // methods chain
    printf("%s %zu %s\n", s.data(), s.len(), s.is_long() ? "heap" : "inline");
    s.free();
}
```

Output:

```text
hello, world 12 1 inline
hello, world, now long enough for the heap (2026) 49 heap
```

The methods: `assign`, `append` (a `char *`, and how many bytes), `appendf` (printf-style),
`data` (the text, always terminated), `len`, `cap` (bytes, terminator included), `is_long`, `clear`
(keeps the memory), `reserve`, `remove` (every occurrence, in place), `copy`, `equal`, `debug` and
`free`. `string("hello", 4)` makes one from the first 4 bytes.

### Operators

```c
#import "runtime"
int main() {
    string hi = "hello";
    string all = hi + " world" + "!";        // hi + " world" in between is a temporary: freed
    printf("%s | %c %c\n", all.data(), all[0], all[-1]);
    printf("%s\n", (all - "l").data());      // removes every "l"
    string a = "apple", b = "banana";
    printf("%d %d %d\n", a < b, b >= a, a < "apricot");
    all += '?';
    string quoted = "<" + all + ">";
    printf("%s\n", quoted.data());
    hi.free(); all.free(); a.free(); b.free(); quoted.free();
}
```

Output:

```text
hello world! | h !
heo word!
1 1 1
<hello world!?>
```

| Operator | Meaning |
|---|---|
| `a + b`, `a + "x"`, `"x" + a` | a new string |
| `a - b`, `a - "x"` | a new string without any occurrence |
| `a += x`, `a -= x` | in place; `+=` also takes a `char` |
| `==`, `!=` | with a string or a `char *` |
| `<`, `>`, `<=`, `>=` | byte by byte, a prefix first |
| `s[i]` | a `char` you can assign; `s[-1]` is the last; out of range aborts |

Strings follow chapter 9: `string_free` is the destructor and `string_copy` the copy, so `a + b`
results are temporaries, `string b = a;` gives `b` its own text, and a struct with string members gets
its own `free` and `copy`.

### f-strings

`f"..."` makes a string with `{expressions}` in it:

```c
#import "runtime"
int main() {
    string name = "Ada";
    int lives = 3; double hp = 87.5; char grade = 'A';
    string hud = f"{name}: {lives} lives, {hp:.1f} hp, {lives * 100} pts, grade {grade}, {{braces}}";
    printf("%s\n", hud.data());
    printf("%s\n", f"[{lives:4}] [{lives:-4}] [{hp:08.3f}] [{255:x}]".data());
    name.free(); hud.free();
}
```

Output:

```text
Ada: 3 lives, 87.5 hp, 300 pts, grade A, {braces}
[   3] [3   ] [0087.500] [ff]
```

`{expr}` picks the conversion from the type: a `string` or `char *` as text, a `char` as a character,
other integers as numbers, floats with `%g`, pointers with `%p`. `{expr:spec}` adds printf's flags,
width, precision and conversion. `{{` and `}}` are literal braces; a `?:` goes in parentheses, and a
string inside is written `\"`. An f-string is a call to `string_fmt` (printf formatting), so it's a
temporary: freed after its statement unless you keep it.

## 12. Arrays, maps and sets

### Arrays

`array(T)` is a plain `T *` with its count and capacity stored just before the elements. So `a[i]`
works, it can be passed to C functions taking a `T *`, and a null pointer is an empty array:

```c
#import "runtime"
int by_value(const void *a, const void *b) { return *(int *)a - *(int *)b; }
int main() {
    array(int) a;
    for (int i = 5; i >= 1; i--) a.push(i * i);
    a.sort(by_value);
    array_foreach(a, it) printf("%d ", *it);
    int top = a.pop();
    printf("| popped %d, %zu left, last %d\n", top, a.count(), a.last());
    a.free();
}
```

Output:

```text
1 4 9 16 25 | popped 25, 4 left, last 16
```

`array_push`, `array_pop`, `array_last`, `array_count`, `array_cap`, `array_reserve`, `array_resize`,
`array_insert`, `array_erase`, `array_clear`, `array_copy`, `array_sort`, `array_foreach` and
`array_free`. Pushing a named owning value stores a copy (chapter 9); `array_pop` moves it out.

They are macros (they work for any `T`, and may move the array when it grows), but they are methods
too, as functions are (chapter 7): `a.push(x)` is `array_push(a, x)`, `a.count()` is `array_count(a)`.
That works because `array(T)` is a type the compiler knows, not only a `T *`. The same holds for maps
and sets below: `ages.set("ana", 31)`, `seen.add(3)`. A name after `.` is never expanded as a macro of
its own, so `.set(` is the method even though `set(K)` is a macro.

### Maps and sets

`map(K, V)` is a hash map that is also a plain pointer, to a dense array of `{ key, value }` entries
(with the hash index stored before them), kept in insertion order. `m[k]` is `m.at(k)`: the value
for key `k`, added zeroed when missing. For a set, `s[k]` is `s.has(k)`, `s += k` adds `k` (once)
and `s -= k` removes it (if it is there):

```c
#import "runtime"
int main() {
    map(string, int) ages;
    ages.set("ana", 31);
    ages["bo"] += 7;                         // added zeroed when missing
    printf("%d %d %zu %d\n", ages["ana"], *ages.get("bo"), ages.count(), ages.has("cy"));
    for (auto *e : ages) printf("%s=%d ", e.key.data(), e.value);
    puts("");
    ages.free();

    set(int) seen;
    for (int i : 0..10) seen += i % 3;
    seen -= 1;
    printf("%zu distinct, has 2: %d, has 1: %d\n", seen.count(), seen[2], seen[1]);
    seen.free();
}
```

Output:

```text
31 7 2 0
ana=31 bo=7 
2 distinct, has 2: 1, has 1: 0
```

`char *` keys compare by text and the map keeps its own copy. A key type with `[[operator==]]` and
a `T_hash(T *)` compares and hashes through them, and the map owns its keys (copied in with the
type's `[[copy]]`, freed with its destructor): `string` is one, so `map(string, V)` compares keys by
their text, however long. Other keys compare by their bytes. `map_set` returns 1 when the key is
new, `map_get` a pointer to the value (NULL if missing), `map_at` the value itself, added zeroed when
missing. `set(K)` is a map with keys only: `set_add`, `set_has`, `set_del`, `set_count`,
`set_foreach`, `set_free`. (In .c2, `m[i]` is always by key; a loop walks the entries.)

### One loop for everything

`for (T x : things)` walks whatever `things` is. A range `a..b` counts:
it is half-open, as C's loops are, so `0..n` is 0 to n-1 and `n..0` is n-1 down to 0 (an array's
indices either way); a third part is the step, and its sign is the direction (`0..n..2`,
`n..0..-2`). To include an end, add 1 to it: `0..10+1` is 0 to 10, and `10+1..0` counts down from
10 to 0. When both ends are variables and there's no step, the compiler asks for one: the
direction is never guessed while the program runs. Text walks as bytes with a `char`, or as UTF-8
characters with a `u32`. An array, an `array(T)`, a map or a set walks its items: `T x` copies each
one (a copy that owns nothing), `T *x` points at it, so the loop can change it:

```c
#import "runtime"
int main() {
    for (int i : 0..4) printf("%d ", i);
    for (int i : 10..0..-3) printf("%d ", i);       // 9 6 3 0
    puts("");
    string word = string("añil");
    for (u32 c : word) printf("U+%04X ", c);
    puts("");
    array(int) scores;
    for (int i : 1..4) scores.push(i * 10);
    for (int *s : scores) *s += 1;
    int total = 0;
    for (int s : scores) total += s;
    map(const char *, int) ages;
    ages.set("ana", 31);
    ages.set("bo", 7);
    for (auto *e : ages) printf("%s %d, ", e.key, e.value);
    for (int age : ages.values()) total += age;
    printf("total %d\n", total);
    word.free(); scores.free(); ages.free();
}
```

Output:

```text
0 1 2 3 9 6 3 0 
U+0061 U+00F1 U+0069 U+006C 
ana 31, bo 7, total 101
```

A map gives its entries (`e.key`, `e.value`), and `keys()` or `values()` one side; a set its keys.
`break`, `continue` and `defer` work as in any loop. Two more kinds come later: `reflect(T)` walks a
type's fields (chapter 15), and a type of your own walks with a method `bool T_next(T *it, X *out)`,
called until it returns false.

## 13. Coroutines

A coroutine is a function with a stack of its own that can pause in the middle and continue later.
`co_yield()` pauses; the next `co_resume()` continues right after it. Game logic that spans frames (a
cutscene, an enemy's patrol, a dialog) then reads as straight code instead of a state machine:

```c
#import "runtime"
void walk(void *arg) {
    for (int step = 1; step <= 3; step++) { printf("step %d\n", step); co_yield(); }
}
int main() {
    co *c = co_create(walk);
    for (int frame = 1; c.resume(); frame++) printf("frame %d drawn\n", frame);
    c.free();
}
```

Output:

```text
step 1
frame 1 drawn
step 2
frame 2 drawn
step 3
frame 3 drawn
```

`co_resume` returns true while the coroutine can go on. `co_status`, `co_done` and `co_running` tell
where things stand. Each coroutine gets a 1 MB stack, but the system only backs the pages it touches,
so a shallow one costs a few KB; `co_create`'s third argument sets another size. A program that embeds
C² can't hot-reload while a coroutine is paused, since its stack returns into the old code. Coroutines
run on [minicoro](https://github.com/edubart/minicoro): `c2 -C` output needs `3rd/minicoro.h` on its
include path.

## 14. Errors: try and throw

C reports errors through return values, which every caller must check and pass up. That is fine for
one level and painful for five. `try` and `throw` (in `#import "runtime/try"`, part of `runtime`) skip the
levels in between: `try(fn, arg)` runs `fn(arg)` and returns 0, unless something below it calls
`throw(code)`. Then every call in between returns at once, and `try` returns the code.

```c
#import "runtime"
void check(int hp) { if (hp <= 0) throw(1); }
void turn(void *hp) {
    defer puts("end of turn");               // runs on a throw too
    check(*(int *)hp);
    puts("still alive");
}
int main() {
    int hp = 10;
    printf("try: %d\n", try(turn, &hp));
    hp = 0;
    printf("try: %d\n", try(turn, &hp));
}
```

Output:

```text
still alive
end of turn
try: 0
end of turn
try: 1
```

On the way out, every function runs its `defer`s and destroys its temporaries, exactly as a `return`
would, so a throw doesn't leak. How it works: `throw` sets a flag, and the compiler follows each call
that might lead to a `throw()` with a check of that flag (one load and one branch); a set flag returns
from the function. It works out which calls those are from the whole program: a call to a function
that never reaches a `throw()` gets no check, so the demo above has three checks for nearly two
hundred calls. Calls through function pointers and into C get one only when a function that throws
has been used as a value somewhere (passing it to `try()` doesn't count). Programs that never throw
pay nothing.

Some limits:

- C code between the `try` and the `throw` (a `qsort` comparator, say) isn't checked: it carries on
  until it returns to C² code, which then leaves.
- A throw inside a coroutine (chapter 13) ends the coroutine and continues from its `co_resume()`.
- `throw(0)` aborts, since 0 is what `try` returns when nothing was thrown; so does a throw outside of
  any `try`.

---

# Part IV: programs

## 15. Reflection

A program sometimes needs to look at its own types: to print a whole struct, save it to a file, or
show it in an editor. In C that means writing, by hand, a list of every field next to every struct,
and keeping it in sync. In C², `reflect(T)` (or `reflect(expr)`, like `sizeof`) gives a description the
compiler builds: a `const c2type *` with the type's name, kind and size, and for a struct its fields
with their exact offsets. It is part of the runtime (`#import "runtime"`).

```c
#import "runtime"
enum Class { WARRIOR, MAGE };
struct Vec { float x, y; };
struct Hero { string name; int hp; Vec pos; Class cls; array(int) items; };
int main() {
    const c2type *t = reflect(Hero);
    printf("%s: %d bytes, %d fields\n", t.name, t.size, t.count);
    for (const c2field *m : reflect(Hero))         // (each_member(Hero, m) too)
        printf("  %-5s %-10s at +%d\n", m.name, m.type.name, m.offset);
    printf("%s\n", enum_name(reflect(Class), MAGE));
}
```

Output:

```text
Hero: 48 bytes, 5 fields
  name  string     at +0
  hp    int        at +24
  pos   Vec        at +28
  cls   Class      at +36
  items array(int) at +40
MAGE
```

Every type is described: numbers, pointers, arrays, structs, unions and enums (with their names and
values), and the runtime's `string`, `array(T)`, `map(K, V)` and `set(K)` as kinds of their own. A
table also points to the type's `[[destructor]]` and `[[copy]]`, so code that walks values can make
and free owned ones. Tables are only built for the types a program reflects, and for what those lead
to.

On top of the tables, every value has a few methods: `x.print()` prints it (`x.print(stderr)` too),
`x.text()` gives the same as a `string`, and an f-string prints a whole struct, or an enum by name.
They work on any value because of one rule: a parameter of type `any` (a value's type and address)
takes any value, and `x.f()` on a value that has no `f` of its own calls `any_f(x)`. So the functions
named `any_*` in the runtime are methods of everything:

```c
#import "runtime"
enum Class { WARRIOR, MAGE };
struct Vec { float x, y; };
struct Hero { string name; int hp; Vec pos; Class cls; array(int) items; };
int main() {
    Hero h = {"Ada", 75, {3, 4}, MAGE};
    array_push(h.items, 7);
    h.print();
    printf("%s\n", f"{h.pos}, a {h.cls} ({h.cls:d})".data());
    h.free();
}
```

Output:

```text
Hero { name = "Ada", hp = 75, pos = Vec { x = 3, y = 4 }, cls = MAGE, items = [7] }
Vec { x = 3, y = 4 }, a MAGE (1)
```

`print` writes text as `"..."` (a `string`, a `char *`, a `char` array), follows no pointer but a
`char *`, and shows a map as `{key: value, ...}`.

`[[skip]]` leaves things out. On a field, the field stays in the table, marked `C2_SKIP`, and
`each_member`, `print` and saving leave it out: a cache, a pointer back to a parent. On a struct,
union, enum or typedef, reflection knows nothing about the type at all, and `reflect` of it is an
error:

```c
#import "runtime"
struct Enemy { int hp; [[skip]] void *ai_cache; };
int main() {
    Enemy e = {30, &e};
    e.print();
}
```

Output:

```text
Enemy { hp = 30 }
```

A host program gets the same tables from a script (a function returning `reflect(T)`): `c2.h` declares
`c2type`, `c2field` and `c2enum`.

### Saving and loading

The tables are enough to write any value out and read it back. Saving and loading live in their own
module, `#import "runtime/serial"`, which isn't part of `#import "runtime"` (it is big, and would
triple the time every script takes to compile). `x.json5()` makes the text and `x.save(path)`
writes a file, in JSON5: JSON that people can edit, with keys unquoted, trailing commas, comments,
`NaN` and `Infinity`:

```c
#import "runtime"
#import "runtime/serial"
enum Class { WARRIOR, MAGE };
struct Item { string name; int count; };
struct Hero {
    string name;
    [[rename("health")]] int hp;
    Class cls;
    array(Item) bag;
    map(const char *, int) skills;
    [[skip]] int frame_cache;
    Hero *ally;
};
int main() {
    Hero h = {"Ada", 75, MAGE};
    h.bag.push(Item("potion", 3));
    h.skills.set("fire", 3);
    printf("%s", h.json5().data());
    if (!h.save("hero.json5")) perror("hero.json5");
    remove("hero.json5");
    h.free();
}
```

Output:

```text
{
  name: "Ada",
  health: 75,
  cls: "MAGE",
  bag: [
    {
      name: "potion",
      count: 3,
    },
  ],
  skills: {
    fire: 3,
  },
}
```

A struct is an object, an array or `array(T)` a list, an enum its name, text a string; a map with
text or enum keys is an object, any other map a list of `[key, value]` pairs, and a set a list.
`[[rename("health")]]` saves a field under another name (so the C name can change without breaking
old saves), and `[[skip]]` leaves one out. Pointers aren't followed, so they are left out (a `char *`
is saved as its text), as are unions, whose member in use isn't known. Floats are written with the
fewest digits that read back as the same value.

`save_json5` writes `path.tmp` first and only then puts it in place of `path`, so a crash or a full
disk never leaves half a save behind; it returns `false` (with `errno` set) when it couldn't.

`x.load(path)` reads a file back into `x`, and `x.parse(text)` a text. Fields are found
by name (or by their `[[rename]]` name). A field the file doesn't have keeps the value it had, so set
the defaults first; a key `x` doesn't have is ignored, so older and newer saves both load. Owned
values are rebuilt: a `string` through its constructor, an `array(T)` item by item, and the old ones
are freed. What doesn't fit is reported, with its path, and the rest still loads:

```c
#import "runtime"
#import "runtime/serial"
enum Class { WARRIOR, MAGE };
struct Hero { string name; int hp; Class cls; array(int) items; unsigned level : 4; };
int main() {
    Hero h = {};
    h.hp = 100;                                   // a default: the text below has no hp
    string errors = {};
    bool ok = h.parse("{name: 'Ada', cls: 'ROGUE', items: [1, 2, 'x'], level: 20, lives: 3}", &errors);
    printf("ok %d: %s%s", ok, h.json5().data(), errors.data());
    h.free(); errors.free();
}
```

Output:

```text
ok 0: {
  name: "Ada",
  hp: 100,
  cls: "WARRIOR",
  items: [1, 2, 0],
  level: 0,
}
Hero.cls: a Class expected (WARRIOR, ...), not 'ROGUE'
Hero.items[2]: an integer expected, not text
Hero.level: 20 doesn't fit 4 bits
```

XML works the same way: `x.xml()` is the text, and `x.save(path)` and `x.load(path)` use XML for a path
ending in `.xml`; `x.parse(text)` reads XML when the text starts with `<`. A struct is an element with a
child element per field (an attribute of that name is read too), a list's items are `<item>`
elements, and a map's items carry their key as `key="..."`.

### Documents

To read a file without a struct to load it into (a config, a level description from a tool),
`#import "runtime/doc"` parses JSON5 or XML into a tree of nodes, read by path: `doc.parse(text)` (XML
when it starts with `<`) and `doc.open(path)` (XML for a `.xml` path). `T.f(...)` calls a function that
belongs to the type, `T_f(...)`: `doc_parse`, `doc_open`. Paths walk keys with `/`, `.` or `[key]`, and
items with `[2]`; in XML, `@name` is an attribute and an element's text is the element itself (or
`$`). Each getter (`integer`, `number` for reals, `text`, `boolean`) takes the value to give when the path
is missing:

```c
#import "runtime/doc"
int main() {
    doc cfg = doc.parse("window: { width: 1280, title: 'C²' }, levels: [{name: 'cave'}, {name: 'tower'}]");
    printf("%lld x %lld, %s\n", cfg.integer("/window/width"), cfg.integer("/window/height", 720), cfg.text("window.title"));
    for (int i = 0; i < cfg.count("levels"); i++) printf("level %d: %s\n", i, cfg.text(f"levels[{i}].name".data()));
    doc ui = doc.parse("<ui><button id='ok' w='80'>OK</button><button id='no'>Cancel</button></ui>");
    printf("%d buttons, the first '%s' (%s, %lld wide)\n", ui.count("/button"), ui.text("/button"),
           ui.text("/button/@id"), ui.integer("/button/@w"));
    doc bad = doc.parse("{hp: 75, name 'Ada'}");
    printf("%s\n", bad.error);
    cfg.free(); ui.free();
}
```

Output:

```text
1280 x 720, C²
level 0: cave
level 1: tower
2 buttons, the first 'OK' (ok, 80 wide)
line 1: ':' expected after 'name'
```

A document that doesn't parse has its `error` set ("line 1: ':' expected after 'name'") and no nodes.
`count` is the number of items of a list or object, or, for XML's repeated elements, how many there
are; `button[1]` is then the second one. The parsers are rewritten from those in Kit (r-lyeh's
kit.data: JSON5 by Dominik Madarasz and r-lyeh, XML by Tor Andersson, public domain).

### By name, and what a field accepts

A console or an editor reaches values by name. `global("hero.pos.x")` finds the program's global
`hero` and walks to the part: an `any`, with its `type` and its address `p` (`type` is `NULL` when
there is no such thing). Paths are the documents' (`hero/bag[2]`, `scores[Ada]` for a map's key), and
pointers to structs are followed. `x.find("pos.x")` starts from any value, and
`globals()` lists the globals (those of the script's own files, but not a module's private ones,
nor `[[skip]]` ones). `r.text()` gives the value as text, and `r.set(text)`, from
`runtime/serial`, sets it from text, the way loading does.

Two attributes say what a field accepts. `[[clamp(min, max)]]` on a number (or a list of numbers: each
item): loading or `ref_set` keeps the nearest end and reports it. `[[clamp(N)]]` on text: at most `N`
characters (UTF-8 characters, not bytes: a cut never splits one), the rest cut and reported.
`[[comment("...")]]` is written into saves, as `//` in JSON5 and `<!-- -->` in XML:

```c
#import "runtime"
#import "runtime/serial"
struct Hero { [[clamp(8)]] string name; [[clamp(0, 100), comment("0 is dead")]] int hp; };
Hero hero;
int main() {
    hero.name = string("Ada");
    string errors = {};
    global("hero.hp").set("250", &errors);
    global("hero.name").set("Bartholomew", &errors);
    printf("%s%s", errors.data(), hero.json5().data());
    any r = global("hero.hp");
    printf("hero.hp (%s): %s\n", r.type.name, r.text().data());
    hero.name.free(); errors.free();
}
```

Output:

```text
hp: 250 is above the maximum, 100
name: text longer than 8 characters, cut
{
  name: "Bartholo",
  // 0 is dead
  hp: 100,
}
hero.hp (int): 100
```

The host has the same: `C2_reflected(c2, "hero.hp")` (`c2.h`), for a script that imports the
runtime.

### Calling by name

A console wants to run functions too. `eval("heal(hero, 20)")`, from `runtime/serial`, calls the
program's function `heal` with the global `hero` and the number 20. Arguments are written as in C²:
by position or by name (`heal(hero, bonus: 0)`), and the missing optional ones take their defaults. An
argument naming a global, or a part of one (`hero.pos`), passes it, or its address to a pointer
parameter; anything else is read as a value, the way loading reads one (`20`, `'Ada'`, `MAGE`,
`{x: 6, y: 8}`). `hero.hit(30)` is a method call: `Hero_hit(&hero, 30)`. The result is appended to a
string, as `print` writes it:

```c
#import "runtime"
#import "runtime/serial"
struct Vec { float x, y; };
struct Hero { int hp; Vec pos; };
Hero hero = {50, {3, 4}};
int heal(Hero *h, int amount = 10, int bonus = amount / 2) { h.hp += amount + bonus; return h.hp; }
int Hero_hit(Hero *h, int damage) { h.hp -= damage; return h.hp; }
float len(Vec v) { return sqrtf(v.x * v.x + v.y * v.y); }
int main() {
    const char *lines[] = {"heal(hero)", "heal(hero, bonus: 0)", "hero.hit(30)", "len(hero.pos)", "heal(hero, 'x')"};
    for (int i = 0; i < countof(lines); i++) {
        string result = {}, errors = {};
        if (eval(lines[i], &result, &errors)) printf("%s = %s\n", lines[i], result.data());
        else printf("%s", errors.data());
        result.free(); errors.free();
    }
}
```

Output:

```text
heal(hero) = 65
heal(hero, bonus: 0) = 75
hero.hit(30) = 45
len(hero.pos) = 5
heal: amount: an integer expected, not text
```

`functions()` lists what can be called: the program's own functions (not a module's), with
their parameters' names, types and whether each is optional.

### Loops over fields, at compile time

Reflection works while the program runs. Some code is better written once for every field and
compiled as if typed out: comparing two values, copying some fields, printing a struct with each
field's own `printf`. `foreach_field(T, X)` expands the macro `X(name)` once per field of `T`, in
order (as reflection sees them: `[[skip]]` fields left out):

```c
#import "runtime"
struct Vec { float x, y, z; };
#define DIFF(f) if (a.f != b.f) printf("%s: %g -> %g\n", #f, a.f, b.f);
#define COUNT(f) n++;
int main() {
    Vec a = {1, 2, 3}, b = {1, 5, 3};
    foreach_field(Vec, DIFF)
    int n = 0;
    foreach_field(Vec, COUNT)
    printf("%d fields\n", n);
}
```

Output:

```text
y: 2 -> 5
3 fields
```

Each expansion is ordinary code, type-checked where it stands: a field that `DIFF` can't compare is
an error at compile time, naming it.

## 16. Debugging

**Call stacks.** `c2 -g` compiles the program so that it keeps track of its own call stack: a crash
then prints where every function was, not just that it crashed. An out-of-range string index, for
instance:

```c
#import "runtime"
void show(string *s, int i) { printf("%c\n", (*s)[i]); }
void scene(string *s) { show(s, 1); show(s, 5); }
int main() { string s = string("ab"); scene(&s); }
```

```text
$ c2 -g crash.c2
string: index 5 out of range [0, 2)
script crashed (SIGABRT, abort()); call stack, innermost first:
  string_at(str = 0x7fffbf468420, i = 5) at runtime/string.c2:166
      n = 2
  show(s = 0x7fffbf468420, i = 5) at crash.c2:2
  scene(s = 0x7fffbf468420) at crash.c2:3
  main() at crash.c2:4
      s = {...}
```

Each frame shows its parameters, and below them the local variables in scope, with their values when
the program crashed: numbers, `true`/`false`, characters, pointers (`NULL` when null). Structs and
arrays show as `{...}` and `[...]`, and a pointer's target is never read (the report must not crash
in turn). A variable whose initializer is the line that crashed shows whatever was in its memory.

(The `b` that `show(s, 1)` printed is lost: the crash came before standard output was flushed.)
A crash inside a coroutine shows the coroutine's frames, then `(a coroutine, resumed from:)` and the
frames that resumed it. The same report comes from a bad pointer (`SIGSEGV`) or a division by zero
(`SIGFPE`, where the processor traps it: x86 does, ARM doesn't).

How it works, and what it costs: each function keeps a small frame (its name, its file, its current
line, where its variables are) linked into a chain, each statement stores its line there, and the
crash handler walks the chain. For the values to be there, `-g` keeps every variable in memory instead
of in a register, and compiles with at most `-O1` so that no store is dropped: about twice as slow for
a program made of tiny calls (`fib`), much less for typical code. Without `-g` nothing is kept and
nothing is paid.

From a program that embeds C²:

- `C2_set_debug(c2, 1)` makes the next compiles keep the call stack.
- `C2_backtrace()` returns it as text, innermost first (`  update() at game.c2:42`, one line per frame),
  say from a host function that a script called and that found something wrong.
- `C2_crash_handler(1)` prints it to stderr when the program crashes (a bad memory access, a division
  by zero, an illegal instruction, a stack overflow on Windows, `abort()`), then lets the crash happen as
  before, so a debugger or the system's crash reporter still sees it. On Windows it uses a vectored
  exception handler, since a crash in JIT-compiled code never reaches the C runtime's `SIGSEGV`; that
  handler runs before any `__try`, so under `-g` a host or a DLL that catches its own access violations
  on purpose would trigger a report too.

To check that it works on a machine, crash on purpose: the `c2-debug` suite of `make test` does exactly
that (`tests/c2-debug/`: an `abort()`, a null pointer, a crash in a coroutine, the variables shown),
and compares the call stack printed with the one expected. `make test` also runs every other test with `-g` (the `debug`
mode), so programs must behave the same with it.

**A real debugger.** `c2 -C program.c2 > program.c` translates the program to C, with `#line`
directives that point back at the `.c2` file. Compile that with debug information and the debugger
shows the original `.c2` lines: breakpoints, stepping, the call stack and the locals all refer to the
`.c2` source. The C runtime that the translation needs comes from `c2 -Crt`, or from C² itself
when the program also embeds it (built with `C2_NO_MAIN`, as a game is): link with that instead.

On Linux or macOS, with gdb (or lldb):

```text
$ c2 -C game.c2 > game.c
$ c2 -Crt > c2_rt.c
$ cc -g -O0 -I 3rd -o game game.c c2_rt.c -lm
$ gdb ./game
(gdb) break game.c2:12
(gdb) run
```

On Windows, with Visual Studio:

```text
> c2 -C game.c2 > game.c
> c2 -Crt > c2_rt.c
> cl /Zi /Od /I 3rd game.c c2_rt.c
> devenv /debugexe game.exe
```

Then open `game.c2` in Visual Studio, put a breakpoint on a line, and press F5: it stops in the `.c2`
source, and F10 / F11 step through it with the locals in view. Code that C² generated itself (no `.c2`
line behind it) shows as `(c2 -C output)`. `-I 3rd` is only needed for coroutines (minicoro).

### Hooks

When a program defines `__on_enter(func, file, line)` or `__on_leave(...)`, every other function
calls them on entry and before each return. That's a profiler, a tracer or a call stack in a few
lines. A program without them pays nothing.

```c
void __on_enter(const char *func, const char *file, int line) { printf("-> %s\n", func); }
int twice(int x) { return 2 * x; }
int main() {
    printf("%d\n", twice(21));
}
```

Output:

```text
-> main
-> twice
42
```

## 17. C libraries

A C² program can call any C library. There are two ways to reach one from a script.

**By hand,** with `dlopen` and `dlsym` (`<dlfcn.h>`, which C² provides on Windows too, on top of
`LoadLibrary`). You declare function pointers and fill them yourself. This needs no headers and lets
you decide when, and whether, to load the library: see `tour/demo_48_dlsym_sdl3.c2`.

**Through a header,** as LuaJIT's `ffi.cdef` and `ffi.load` do: `#include` the declarations, then
name the library with `#pragma comment(lib, "name")` (MSVC's spelling). Calls are plain C calls,
resolved from the library when the program is linked:

```c
#include "sdl3.h"                     // the declarations: bool SDL_Init(uint32_t flags); ...
#pragma comment(lib, "SDL3")          // SDL3.dll, libSDL3.dylib or libSDL3.so
int main() {
    if (!SDL_Init(SDL_INIT_VIDEO)) return 1;
    SDL_Window *w = SDL_CreateWindow("Hello", 640, 360, 0);
    ...
}
```

A name without a dot gets the platform's prefix and extension; a name with one (`"libSDL3.so.0"`) is
that exact file. Either is looked for by the system first, then in the current folder. A program that
embeds C² can do the same for every script with `C2_load(c2, "sdl3.h", "SDL3")`, and the `c2` command
with `-load sdl3.h,SDL3`. `tour/demo_49_ffi_sdl3.c2` opens a window this way.

## 18. Attributes

The chapters met attributes one at a time, where each was useful. This one puts them all together.

An attribute is C23's `[[name]]`, or `[[name(arguments)]]`, written before what it applies to: a
declaration (a function, a variable, a struct, a field), or a statement (a loop). Several share one
list, `[[export, nodiscard]]`, or stand side by side, `[[export]] [[nodiscard]]`. C compilers ignore
the ones they don't know, so C code with C² attributes still compiles elsewhere; C² does the same with
other compilers' attributes (`[[gnu::cold]]`), but in a `.c2` file it warns about a name it doesn't
know, since that is usually a typo that would otherwise silently do nothing.

| Attribute | Goes on | What it does | See |
|---|---|---|---|
| `[[export]]` | a function, global, struct, union, enum or typedef | makes a module's name visible to the files that `#import` it (a macro: `#export NAME`) | [3](#3-headers-and-modules) |
| `[[unclear]]`, `[[indeterminate]]` | a local variable | leaves it uncleared (locals start at zero) | [4](#4-types-and-data) |
| `[[nodiscard]]` | a function | warns when a call's result is thrown away | [4](#4-types-and-data) |
| `[[deprecated("why")]]` | a function | warns at each call, with the reason | [4](#4-types-and-data) |
| `[[pure]]`, `[[reproducible]]`, `[[unsequenced]]` | a function | says a call only computes its result; a call whose result is thrown away is warned about | below |
| `[[unroll]]`, `[[unroll(N)]]` | a `for`, `while` or `do` | compiles the body several times in a row | [4](#4-types-and-data) |
| `[[musttail]]` | a `return f(...)` in `f` | an error, instead of a warning, when the tail call can't become a loop | [5](#5-blocks-and-functions) |
| `[[destructor]]` | a `void T_free(T *)` | frees a `T`: temporaries, owned fields, `x.free()` | [9](#9-owning-values) |
| `[[copy]]` | a `T T_copy(const T *)` | copies a `T`, so that two never share what the destructor frees | [9](#9-owning-values) |
| `[[operator+]]` (and `- * / % == < <= > >= [] += -= *= /= %=`) | a function | makes the operator call it for structs | [10](#10-operators) |
| `[[noncommutative]]` | an operator function | `2 * m` no longer means `m * 2` | [10](#10-operators) |
| `[[skip]]` | a struct, an enum, a typedef, a field, a global | leaves it out of reflection: no table, not saved, not listed | [15](#15-reflection) |
| `[[rename("hp")]]` | a field | the name saves and paths use for it | [15](#15-reflection) |
| `[[clamp(min, max)]]` | a number field (or a list of numbers) | loading keeps a value between `min` and `max`, and reports it | [15](#15-reflection) |
| `[[clamp(N)]]` | a text field (`string`, `char [N]`) | loading keeps at most `N` characters, and reports it | [15](#15-reflection) |
| `[[comment("...")]]` | a field | written into saves as a comment | [15](#15-reflection) |
| `[[persist]]` | a global | keeps its value when the host reloads the program | below |
| `[[maybe_unused]]`, `[[fallthrough]]`, `[[noreturn]]` | as in C23 | accepted; C² has no warnings for them to silence | |

### Pure functions

`[[pure]]` says a function does nothing but compute its result from its arguments and what it reads:
no output, no writes to globals or through pointers. It is C23's `[[reproducible]]` under a shorter
name, and GCC's `__attribute__((pure))`. The stricter C23 `[[unsequenced]]` (GCC's `const`) also
promises not to read anything but the arguments. C² takes all three as a promise about calls: since
such a call does nothing else, throwing its result away is a mistake, and it says so.

```c
[[pure]] int sq(int x) { return x * x; }
int main() {
    sq(3);                       // warning: nothing happens
    printf("%d\n", sq(4));
}
```

Output:

```text
example.c2:3: warning: the result of 'sq' is discarded ([[pure]]: the call does nothing else)
16
```

C² checks the promise where it can see it: a `[[pure]]` function that writes a global, or calls a
function that isn't pure (C's math and string functions are), gets a warning. Writing its own locals,
and reading anything, is fine. It doesn't optimize on the promise yet.

### Kept across reloads

A program that embeds C² can compile a script again while it runs (hot reload, in the
[README](README.md)). The new program starts from its own initializers, except for the globals marked
`[[persist]]`, which take the values the old program left:

```c
[[persist]] int score;                // the same name and type: kept as it is
[[persist]] Hero hero;                // Hero gained a field: moved field by field (#import "runtime")
```

When the type changed, a struct moves field by field, matched by name: numbers convert, an enum keeps
its value's name, arrays keep their items, strings move; what can't move starts again from its
initializer, with a warning that names it.

# Appendix: C and C² side by side

| You write in C | In C² (`.c2`) |
|---|---|
| `#include <stdio.h>` ... | nothing, for stdio, string, stdlib, math, stdint, stddef |
| `int f(void)` | `int f()` |
| `typedef struct Vec Vec;` | nothing: `Vec` is a type |
| `p->x` | `p.x` (or `p->x`) |
| `int n = 0;` | `int n;` (zeroed) |
| `sizeof a / sizeof a[0]` | `countof(a)` |
| `cleanup:` labels and `goto` | `defer` |
| `(Vec){1, 2}` | `Vec(1, 2)`, `Vec(y: 2)` |
| `vec_add(a, b)` | `a + b`, with `[[operator+]]` |
| `vec_len(&v)` | `v.len()`, with `Vec_len` |
| `print_int(1); print_str("x");` | `print(1); print("x");` |
| `void f(int a, int b) ... f(1, 0)` | `void f(int a, int b = 0) ... f(1)` |
| headers with `static` helpers and `#undef`s | modules: `#import`, `[[export]]` |
| `free(x)` on every path, by hand | `[[destructor]]`, `[[copy]]`, temporaries |
| `if (err) return err;` at each level | `try` / `throw` |
| `char buf[64]; snprintf(...)` | `string`, `f"..."` |

Things C² keeps from C, unchanged: the types and their sizes (`long` is 64-bit everywhere, Windows
included), memory and pointers, `malloc` and `free` (the host's own, so memory passes freely between a
program and the host that embeds it), the C library, and the platform's calling convention, so C² and C
code call each other directly.
