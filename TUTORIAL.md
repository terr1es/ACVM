# Axiom — a tutorial

Axiom is a statically typed, compiled language with explicit integer widths, exact overflow
behaviour, value-semantic aggregates, and single-exit functions. This document proceeds from an
empty file through concurrency and the standard library.

Every complete program in this document is executed by `tools/verify_tutorial.py`, and each
stated output is compared line by line. A program that stops producing its documented output
fails that script.

```
python tools/verify_tutorial.py
```

## Contents

| | |
|---|---|
| [0](#0-setup-and-first-program) | Setup and first program |
| [1](#1-types-and-values) | Types and values |
| [2](#2-variables-constants-literals) | Variables, constants, literals |
| [3](#3-operators) | Operators |
| [4](#4-control-flow) | Control flow |
| [5](#5-functions-and-eof) | Functions and `eof` |
| [6](#6-pointers-and-memory) | Pointers and memory |
| [7](#7-aggregates) | Aggregates |
| [8](#8-methods) | Methods |
| [9](#9-generics) | Generics |
| [10](#10-modules-and-native-libraries) | Modules and native libraries |
| [11](#11-concurrency) | Concurrency |
| [12](#12-standard-library) | Standard library |
| [13](#13-errors-and-diagnostics) | Errors and diagnostics |
| [14](#14-writing-efficient-code) | Writing efficient code |
| [15](#15-conventions) | Conventions |
| [16](#16-common-errors) | Common errors |
| [17](#17-cheat-sheet) | Cheat sheet |

## 0. Setup and first program

### 0.1 Build and run

```bash
cmake -S . -B build
cmake --build build -j
acvm run examples/tour.ax
```

`acvm` is copied to `~/.local/bin` during the build, which is normally already on
`PATH`. Pass `-DACVM_INSTALL_COMMAND=OFF` to skip that step.

### 0.2 The smallest program

```axiom
function main():void {
    print("hello, axiom\n");
    eof: pass;
}
```

```
── output ──
hello, axiom
```

Three syntactic properties account for most of the difference from C at first reading:

1. **Statements and declarations end with `;`.**
2. **A function body does not end with `}`.** It ends with `eof: pass;`, which is its only exit
   (see [§5.2](#52-eof-single-exit)).
3. **The return type follows a colon**; there is no leading return-type keyword.

### 0.3 A larger program

```axiom
struct Point {
    int x;
    int y;
};

function distanceSquared(Point a, Point b):int {
    int dx = a.x - b.x;
    int dy = a.y - b.y;
    return dx * dx + dy * dy;
    eof: pass;
}

function main():void {
    Point origin = {0, 0};
    Point p = {3, 4};
    print(distanceSquared(origin, p));
    print("\n");
    eof: pass;
}
```

```
── output ──
25
```

`StructName = {…}` initialises positionally; `p.x` reads a field. The struct is a value:
passing `Point` to a function copies eight bytes.

## 1. Types and values

### 1.1 Integers: the width is in the name

```axiom
function main():void {
    int8   a = 127;                  // 1 byte
    int16  b = 32767;                // 2 bytes
    int    c = 2147483647;           // int32, the default
    int64  d = 9223372036854775807;  // 8 bytes
    int128 e = 1;                    // 16 bytes

    // sizeof takes a type, not a variable: sizeof(a) is an error.
    print(sizeof(int8));  print(" ");
    print(sizeof(int16)); print(" ");
    print(sizeof(int));   print(" ");
    print(sizeof(int64)); print(" ");
    print(sizeof(int128)); print("\n");
    eof: pass;
}
```

```
── output ──
1 2 4 8 16
```

- `int` is `int32`, always. It does not depend on the platform.
- Any width from 2 to 128 is legal, including widths that are not multiples of 8
  (`int4` occupies one byte and is masked on write).
- **All `intN` are signed**, so `int8` is −128..127.

```axiom
function main():void {
    unsigned int32 mask = 4294967295;   // 0 .. 2^32-1
    print(mask);
    print("\n");
    eof: pass;
}
```

```
── output ──
4294967295
```

`unsigned int128` is refused: its maximum (2^128−1) does not fit the signed 128-bit cell
the VM carries every scalar in. Use `int128` for the signed range, or `unsigned int127`
for up to 2^127−1.

### 1.2 Floats

`float` is `float32`, `double` is `float64`. `float16` exists. `doubleA.B` is accepted
syntactically but stored as binary64 — there is no custom exponent/mantissa encoding.

### 1.3 char and bool

`char` is **2 bytes** (UTF-16 code units, so it can hold a BMP code point). `bool` is
1 byte and holds `true` / `false`.

Neither is a number: `bool + int` is a type error, and `char` does not mix with integers
without a cast. This is deliberate — in C, `'a' + 1` quietly produces a number.

### 1.4 string

A `string` is an immutable byte string: a pointer plus a length. `length` is a property,
not a method, and concatenation with `+` allocates.

**Indexing yields the byte at that offset, as a `char`.** `s[0]` is `'a'` for ASCII text,
and the first byte of the encoding for anything else — `"中"[0] as int` is `0xE4`, not the
character. Elements are read-only: a string is a byte buffer, a `char` is two bytes wide,
so storing through `s[i]` would overwrite the neighbouring byte. Character access is
`str.charAt(s, i)`, and `str.codePointAt(s, i)` when the text may contain an emoji.

```axiom
function main():void {
    string s = "axiom";
    print(s.length);            // 5, in bytes
    print(" ");
    print(s[0] as int);         // 97, the byte 'a'
    print(" ");
    print(s[1] == 'x');         // true -- s[i] compares against a char literal
    print(" ");
    print(s + "!");             // axiom!
    print("\n");
    eof: pass;
}
```

```
── output ──
5 97 true axiom!
```

Comparing strings with `==` compares contents, not pointers. `String` values are
compared by bytes; `s == "axiom"` is true for any equal content.

### 1.5 void and null

`void` is the absence of a value. `null` is a literal of its own type: convertible to any
`ptr T`, not dereferenceable, and not arithmetic. Comparisons against it are safe.

```axiom
function main():void {
    ptr int p = null;
    print(p == null);           // true
    print("\n");
    eof: pass;
}
```

```
── output ──
true
```

### 1.6 Type overview

| | Spelling |
|---|---|
| Integers | `int` (=`int32`), `int2`..`int128`, `unsigned intN` (N ≤ 127) |
| Floats | `float16`, `float32`, `float64`, `float`, `double`, `doubleA.B` |
| Others | `char`, `bool`, `void`, `string` |
| Pointers | `ptr T`, stackable: `ptr ptr int` |
| Arrays | `T[N]` |
| Tuples | `(T1, T2, …)` |
| Named | `struct`, `union`, `enum` |
| Missing | `vector T` — use the `vec` module instead |

## 2. Variables, constants, literals

### 2.1 Declarations put the type first

```axiom
function main():void {
    int count = 0;              // explicit type, initialiser
    int copy = count;           // a copy, always
    count = 5;
    print(count); print(" "); print(copy); print("\n");
    eof: pass;
}
```

```
── output ──
5 0
```

Reading a variable that may not have been assigned on every path is a **compile error**,
not undefined behaviour:

```axiom
int x;
print(x);       // error: variable 'x' may be used before initialisation
```

Give it a value at the declaration: `int x = 0;`.

### 2.2 const

`const` is runtime-immutable: assigned once, may depend on runtime values, cannot be
reassigned.

```axiom
function main():void {
    const int limit = 10;
    print(limit);
    print("\n");
    eof: pass;
}
```

```
── output ──
10
```

### 2.3 comptime

`comptime` is a compile-time constant: it must be evaluable during compilation, and it
can be used where a value is required at compile time.

```axiom
comptime int SCALE = 4;

function main():void {
    print(SCALE * 3);
    print("\n");
    eof: pass;
}
```

```
── output ──
12
```

### 2.4 Literal type inference

An integer literal takes the type its context requires, and is checked for range **for
that type**. There is no "everything is int until proven otherwise" phase.

```axiom
function main():void {
    int8 small = 100;          // fits, fine
    int64 big = 100;           // also fine, same literal
    print(small); print(" "); print(big); print("\n");
    eof: pass;
}
```

```
── output ──
100 100
```

A literal that fits nowhere in its context is an error, and the message quotes the value
in full — including values wider than 64 bits:

```axiom
int8 tooBig = 200;      // error: constant '200' is out of range for 'int8'
```

Suffixes pick a type explicitly: `42i64`, `0xFFu8`, `3.5f`, `1.5d`, and `…i128` for
values above 2^63−1 that a plain literal cannot express.

## 3. Operators

### 3.1 Overview

| Group | Operators |
|---|---|
| Arithmetic | `+ - * / %` |
| Comparison | `== != < <= > >=` |
| Logical | `&& \|\| !` |
| Bitwise | `& \| ^ ~ << >>` |
| Assignment | `= += -= *= /= %= &= \|= ^= <<= >>=` |
| Pointer | `#x` address-of, `@p` dereference |
| Cast | `as` |
| Other | `sizeof(T)`, `alignof(T)`, `?:`, `,` |

### 3.2 No implicit conversion, but width promotion

There is no implicit conversion between different types. There **is** widening inside a
binary operation: the narrower operand is promoted to the wider one.

```axiom
function main():void {
    int32 a = 100;
    int64 b = 200;
    print(a + b);              // promoted to int64
    print("\n");
    eof: pass;
}
```

```
── output ──
300
```

Mixing signed and unsigned is refused rather than silently reinterpreted:

```axiom
int a = 1;
unsigned int b = 2;
print(a + b);       // error: mixed signed and unsigned operands
```

### 3.3 as: a checked conversion

`as` converts between numeric types and **checks the range at runtime**. A narrowing
conversion that does not fit traps instead of truncating.

```axiom
int64 big = 300;
int8 small = big as int8;     // traps: integer overflow, nothing is assigned
```

A conversion that fits is silent:

```axiom
function main():void {
    int64 big = 42;
    int8 small = big as int8;
    print(small);
    print("\n");
    eof: pass;
}
```

```
── output ──
42
```

Pointer casts (`p as ptr int8`, `p as int64`) are allowed and do not check anything.

### 3.4 Pointer arithmetic scales by element size

```axiom
function main():void {
    int a[4] = {10, 20, 30, 40};
    ptr int p = #a[0];
    print(@(p + 2));            // 30, not the third byte
    print("\n");
    eof: pass;
}
```

```
── output ──
30
```

`p + 1` advances by `sizeof(int)`. Pointer difference returns the number of elements, not
bytes.

### 3.5 Short circuit and side effects

`&&` and `||` evaluate left to right and stop as soon as the answer is known, so the
right operand may not run at all. That matters when it has side effects.

## 4. Control flow

### 4.1 if / else

```axiom
function main():void {
    int n = 7;
    if (n % 2 == 0) {
        print("even");
    } else if (n % 3 == 0) {
        print("divisible by 3");
    } else {
        print("other");
    }
    print("\n");
    eof: pass;
}
```

```
── output ──
other
```

The condition must be `bool`. An integer is not a condition — `if (n)` is a type error.

### 4.2 Four kinds of loop

```axiom
function main():void {
    // 1. while
    int i = 0;
    while (i < 2) { print("w"); i++; }

    // 2. three-part for
    for (int j = 0; j < 2; j++) { print("f"); }

    // 3. for-in over a range (half-open), or over an array (by element)
    for k in 0..3 { print(k); }
    int a[2] = {7, 8};
    for x in a { print(" "); print(x); }

    // 4. loop(n): repeat n times, no counter
    loop (2) { print("L"); }
    print("\n");
    eof: pass;
}
```

```
── output ──
wwff012 7 8LL
```

`for i in 0..3` runs with `i` equal to 0, 1, 2 — the upper bound is **exclusive**.

### 4.3 switch: no fallthrough, default last

```axiom
function main():void {
    int n = 2;
    switch (n) {
        case 1: print("one");
        case 2: print("two");
        default: print("other");
    }
    print("\n");
    eof: pass;
}
```

```
── output ──
two
```

There is no fallthrough: exactly one arm runs. `default` must come last, and the compiler
checks that. A missing `default` is allowed; then an unmatched value does nothing.

### 4.4 break, continue, labels

`break` leaves the innermost loop or switch, `continue` starts the next iteration. Both
accept a label to target an outer loop:

```axiom
function main():void {
    outer: for i in 0..3 {
        for j in 0..3 {
            if (j == 1) { continue outer; }
            if (i == 2) { break outer; }
            print(i); print(j); print(" ");
        }
    }
    print("\n");
    eof: pass;
}
```

```
── output ──
00 10 
```

### 4.5 Tuple destructuring

```axiom
function two(): (int, int) {
    return (1, 2);
    eof: pass;
}

function main():void {
    (int, int) t = two();
    print(t[0]); print(" "); print(t[1]); print("\n");

    int x = 0;
    int y = 0;
    (x, y) = two();
    print(x); print(" "); print(y); print("\n");
    eof: pass;
}
```

```
── output ──
1 2
1 2
```

## 5. Functions and eof

### 5.1 Declaration and call

```axiom
function add(int a, int b):int {
    return a + b;
    eof: pass;
}

function greet(string name):void {
    print("hi, " + name + "\n");
    eof: pass;
}

function main():void {
    print(add(2, 3));
    print("\n");
    greet("ax");
    eof: pass;
}
```

```
── output ──
5
hi, ax
```

Parameters are `type name`. **Everything is passed by value**, including arrays and
structs — the whole object is copied. To avoid a copy, pass `ptr T`.

Functions may be called before their definition; declaration order does not matter.

### 5.2 eof: single exit

This is the part of Axiom with no C equivalent. **A function has exactly one exit: its
`eof` block.** A `return` writes the value into the return slot and jumps to `eof`.

```axiom
function find(ptr int data, int n, int target):int {
    for (int i = 0; i < n; i++) {
        if (@(data + i) == target) {
            return i;              // jumps to eof
        }
    }
    return -1;
    eof: pass;                     // the real exit
}

function main():void {
    int a[5] = {4, 8, 15, 16, 23};
    print(find(#a[0], 5, 15)); print(" ");
    print(find(#a[0], 5, 99)); print("\n");
    eof: pass;
}
```

```
── output ──
2 -1
```

Consequences, all of them enforced:

| Rule | Reason |
|---|---|
| No `return` inside `eof` | It is the return |
| No `import` inside `eof` | Nothing may begin after the exit |
| Omitting `eof` means `eof: pass;` | Convention for the common case |
| `eof` holds cleanup only | Other logic there runs on every path, including early returns |
| `eof` is **not** `finally` | A trap aborts the process and `eof` does not run |

`eof` executes when a function returns; it does not execute when the program traps. It is
consequently unsuitable for releasing a resource whose state has to survive a failure.

### 5.3 Recursion

```axiom
function fib(int n):int {
    if (n < 2) { return n; }
    return fib(n - 1) + fib(n - 2);
    eof: pass;
}

function main():void {
    print(fib(10));
    print("\n");
    eof: pass;
}
```

```
── output ──
55
```

There is no tail-call elimination; deep recursion grows the frame stack.

## 6. Pointers and memory

### 6.1 Taking and using an address

```axiom
function main():void {
    int value = 41;
    ptr int p = #value;        // address of
    @p = @p + 1;               // dereference to read and write
    print(value);
    print("\n");
    eof: pass;
}
```

```
── output ──
42
```

`#x` is address-of; `@p` is dereference. There is no `&` / `*` pair as in C — the
characters were needed elsewhere, and `#` / `@` are visually distinct from multiplication.

### 6.2 Pointers into arrays

```axiom
function sumAll(ptr int data, int n):int {
    int total = 0;
    for (int i = 0; i < n; i++) {
        total += @(data + i);
    }
    return total;
    eof: pass;
}

function main():void {
    int a[5] = {1, 2, 3, 4, 5};
    print(sumAll(#a[0], a.length));
    print("\n");
    eof: pass;
}
```

```
── output ──
15
```

**Arrays do not decay.** Passing `a` copies the array; passing `#a[0]` plus a length
gives the callee a view of it. `@(p + i)` is not bounds-checked, `a[i]` is.

### 6.3 Arena allocation

`alloc` returns zeroed memory from a region allocator; `free` is a no-op there — memory is
released when the region ends. This is what makes short-lived servers cheap: no
per-object bookkeeping.

```axiom
function main():void {
    ptr int buffer = alloc(10 * sizeof(int)) as ptr int;
    if (buffer == null) {
        print("out of memory\n");
        return;
    }
    @(buffer + 0) = 7;
    print(@(buffer + 0));
    print("\n");
    free(buffer);
    eof: pass;
}
```

```
── output ──
7
```

`alloc` returns `ptr void`, which must be cast to the element type. The result requires a
`null` check before use.

### 6.4 Prohibited operations

- Dereferencing `null` traps.
- Storing a pointer to a local in a longer-lived structure traps or dangles; the VM does not
  track lifetimes. Pointers are valid only to arena memory or to caller-owned data.
- `ptr` arithmetic on `void` is refused: there is no element size by which to scale.

## 7. Aggregates

### 7.1 struct: C layout, value semantics

```axiom
struct S {
    int a;
    int b;
    int c;
};

function main():void {
    S s = {1};                 // positional; the rest is zero
    print(s.a); print(s.b); print(s.c); print(" ");
    print(sizeof(S)); print("\n");
    eof: pass;
}
```

```
── output ──
100 12
```

- **C layout**: fields in declaration order, each naturally aligned, the whole padded to
  the largest alignment. So `{int8 a; int64 b; int8 c}` is 24 bytes, not 10.
- **Value semantics**: assignment, argument passing and return copy the bytes.
- **No self-containment**: `struct A { A a; };` is an error; `ptr A` is fine.
- **Partial initialisation zero-fills** the rest: `{1}` gives `{1, 0, 0}`.
- The initialiser is positional. There is no `{a: 1}` form.
- The declaration ends with `;`.

### 7.2 enum: weak

```axiom
enum Color { RED, GREEN = 5, BLUE };

function main():void {
    Color c = BLUE;
    print(c); print(" ");
    print(RED); print(" ");

    int raw = c;               // implicit both ways
    Color back = 6;
    print(raw); print(" "); print(back); print("\n");
    eof: pass;
}
```

```
── output ──
6 0 6 6
```

The underlying type is `int` and members are compile-time constants. It is **weak**: it
converts to and from `int` implicitly and `Color c = 99;` is legal. Member names live in
the global scope (`RED`, not `Color.RED`).

### 7.3 union: shared storage

```axiom
union U {
    int i;
    float f;
};

function main():void {
    U u;
    u.i = 65;
    print(u.i); print(" ");
    print(sizeof(U)); print("\n");
    eof: pass;
}
```

```
── output ──
65 4
```

Every member starts at offset 0 and the size is the largest member rounded up to the
widest alignment — the C layout, so a union can cross a native boundary unchanged.
Reading a member that was not the last one written is a reinterpretation of those bytes,
not an error; that is the point of the type.

### 7.4 Arrays

```axiom
function main():void {
    int a[4] = {10, 20, 30, 40};
    print(a.length); print(" ");
    print(a[2]); print(" ");

    for x in a { print(x); print(" "); }
    print("\n");

    a[1] = 99;
    print(a[1]); print("\n");
    eof: pass;
}
```

```
── output ──
4 30 10 20 30 40 
99
```

- The **length is part of the type**: `int[3]` and `int[4]` are different types and do not
  assign to each other.
- Indexing is bounds-checked; `@(p + i)` is not.
- Arrays do not decay to pointers.

The bounds check is part of the access, not something the compiler emits around it. When the
array is a global and the index a variable, `buf[i]` compiles to one instruction -- the load,
the check and the scaling folded together -- which is what keeps an array walk within a factor
of two of CPython instead of a factor of six. Pointer arithmetic has no check to fold, which is
why `@p` remains the faster form for a sequential walk: see [14](#14-writing-efficient-code).

### 7.5 Tuples

```axiom
function main():void {
    (int, string) pair = (7, "seven");
    print(pair[0]); print(" "); print(pair[1]); print("\n");
    eof: pass;
}
```

```
── output ──
7 seven
```

Access is by **index**, `t[0]`; `t.0` is not supported. Tuples suit returning two values
locally. They are a poor public API: in three months nobody remembers what `t[1]` means —
use a struct with named fields.

## 8. Methods

Methods are written inside the `struct` body. They take **no storage**: layout is
determined by fields alone, so a struct with methods still crosses a native boundary
unchanged.

```axiom
struct Counter {
    int value;

    // value receiver: works on a copy
    function get(self):int {
        return self.value;
    }

    // pointer receiver: may modify the caller's object
    function bump(ptr self):void {
        self->value = self->value + 1;
    }

    // no receiver: a static-like constructor
    function zero():Counter {
        Counter c = {0};
        return c;
    }
};

function main():void {
    Counter c = Counter.zero();
    c.bump();
    c.bump();
    print(c.get()); print(" ");
    print(sizeof(Counter)); print("\n");
    eof: pass;
}
```

```
── output ──
2 4
```

Rules:

| Rule | Detail |
|---|---|
| Receiver spelling | exactly `self` or `ptr self`; other names are rejected |
| Value receiver | writes are refused: they would modify a copy the caller does not observe |
| Pointer receiver | requires an addressable object; a temporary is rejected |
| Static methods | called as `Type.method(...)`; instance methods as `obj.method(...)` |
| Storage | none — `sizeof` is unaffected by methods |
| Generic methods | not supported; a method inside a **generic struct** is supported |

A struct whose methods mutate through `ptr self` requires initialisation before the first call:
`Counter c = {0};`. Definite assignment applies to methods as to any other write.

## 9. Generics

### 9.1 Generic functions

```axiom
function maxOf<T>(T a, T b):T {
    if (a > b) { return a; }
    return b;
    eof: pass;
}

function main():void {
    print(maxOf(3, 9)); print(" ");
    print(maxOf(2.5, 1.5)); print("\n");
    eof: pass;
}
```

```
── output ──
9 2.5
```

Instantiation is per (template × type): one copy of the code per combination used. For
the handful of types a template is normally used with, that cost is far below the
repetition it saves.

### 9.2 Generic structs

```axiom
struct Pair<T> {
    T first;
    T second;

    function sum(self):T {
        return self.first + self.second;
    }
};

function main():void {
    Pair<int> pi = {10, 20};
    print(pi.sum()); print(" ");

    Pair<double> pd = {1.5, 2.5};
    print(pd.sum()); print(" ");
    print(sizeof(Pair<int>)); print("\n");
    eof: pass;
}
```

```
── output ──
30 4 8
```

### 9.3 No constraints, so operators are checked per type

Axiom has no operator overloading, so what a generic body can do depends on the type it is
instantiated with. There is no `T: Comparable` syntax; the error appears at the operator,
pointing at the line in the template.

```axiom
function sumOf<T>(T a, T b):T {
    return a + b;      // for T = Point: '+' requires a numeric operand
    eof: pass;
}
```

The diagnostic is the design: reporting that a template does not apply to a type, at the operator
that fails, is more useful than requiring constraints to be declared in advance at this
language's current size. One consequence: `maxOf<Point>` also fails, because `<` is not defined
for aggregates (see below).

### 9.4 Aggregate operations

`==` and `!=` on aggregates compare storage bytes. `<`, `<=`, `>`, `>=` are **compile errors**:
field order is arbitrary, and byte order carries no meaning for an aggregate. To order structs,
compare the intended field, or sort by a key with `sortIndicesByKey`.

## 10. Modules and native libraries

### 10.1 Importing a module

```axiom
function import (return:(json));

function main():void {
    Doc d = docNew(0);
    ptr Value root = jsonParse(#d, "{\"port\": 8080}");
    if (root == null) {
        print("parse failed\n");
    } else {
        print(valInt(valGet(root, "port"), 0));
        print("\n");
    }
    docFree(#d);
    eof: pass;
}
```

```
── output ──
8080
```

`import (name)` looks for `name.axd` (declarations for a shared library) then `name.ax`
(pure Axiom). Names imported this way are called unqualified.

The `function` keyword is noise for a module list, so the short form is accepted too. Both
normalise to the same declaration, so they can be mixed in one file:

```axiom
import (io, json);                    // short form: a module list
function import (return:(str));       // the long form, same meaning
```

### 10.2 Importing a file by path

```axiom
import "lib/sort.ax";              // statement form
import ("lib/sort.ax");            // parenthesised, also a path
function import (return:("lib/sort.ax"));   // equivalent
```

A quoted string is always a path and an identifier is always a module name, so one token
of lookahead tells the two apart: `import (io);` is a module, `import ("io.ax");` is a
file.

### 10.3 Declaring a module's identity

```axiom
module "json" version "0.1.0";
```

A file-level directive, sibling to `library`. The name must equal the file's own stem and the
version must be `MAJOR.MINOR.PATCH`; both are checked, because package tooling will trust this
string and a stale one would resolve the wrong module silently.

### 10.4 Name collisions across files

| Situation | Behaviour |
|---|---|
| Same signature twice in one file | **error** |
| Entry file vs an imported file | entry file wins (a library helper may be overridden) |
| Two imported files, same signature | **error** — neither is privileged |
| Either side `private`, different files | independent; `private` scopes the name |

The third row is load-bearing: before it was enforced, the winning definition depended on
`import` order, so an unrelated import could replace a function the program depended on, with
nothing at the call site to indicate it.

`private` is file-scoped in the strict sense: other files cannot see the name **and are not
affected by it**. Two libraries may each declare a `private helper` with the same signature.

### 10.5 Calling C

A `.axd` file declares library functions:

```axiom
library "ext";

function nativeSort(ptr int data, int64 count):int64;   // signature only
```

The VM resolves symbols with the platform loader; a call that cannot find its library
traps with `cannot load library`. Arguments and results cross the boundary as 64-bit
values, so widths above 64 bits are not meaningful there.

## 11. Concurrency

Three layers, often confused:

| Layer | Mechanism | Nature |
|---|---|---|
| Scheduling | `process`, `event`, `yield` | **cooperative**, single-threaded round robin |
| Synchronisation | `sync` module | atomics, mutexes, semaphores, channels |
| Real parallelism | `parallel`, `threadpool` | actual OS threads, pure computation only |

### 11.1 Safe points

The interpreter can switch processes only at a **safe point**: a loop back-edge, a
function call or return, or `yield()`. Two adjacent instructions are never interrupted.

This is cooperative, not preemptive. Races are rare and the implementation is simple; the
price is that a loop which never yields starves everything else.

### 11.2 process

```axiom
global int counter = 0;

function process worker():void {
    while (true) {
        counter = counter + 1;
        if (counter >= 6) {
            return;
        }
        yield();
    }
    eof: pass;
}

function main():void {
    loop (20) { yield(); }
    print("counter="); print(counter); print("\n");
    eof: pass;
}
```

```
── output ──
counter=6
```

A `process` has an implicit main loop, so it needs at least one reachable `return`.
Globals are shared. A single read or write of a scalar is atomic; a read-modify-write is
**not** — use `sync` for that.

### 11.3 event: edge triggered

```axiom
function event onThree()(counter == 3):void {
    print("[reached 3] ");
    eof: pass;
}
```

The condition sits in double parentheses and is evaluated at safe points. It fires once
when the condition goes from false to true — edge triggered, not level triggered. An event
body may not reference its own parameters, because the condition is evaluated outside that
scope.

### 11.4 sync

```axiom
function import (return:(sync));

global int counter = 0;

function process worker():void {
    for (int i = 0; i < 1000; i++) {
        atomicAdd(#counter, 1);      // read-modify-write, atomically
        yield();
    }
    eof: pass;
}
```

The core of the module is `atomicAdd`, an atomic load/store pair, `mutex` create/lock/unlock/
destroy, semaphores, and bounded channels.

### 11.5 parallel: OS threads

`parallel` runs a function on a pool of OS threads. It is intended for pure computation: the
body receives no VM services, so no allocation, no safely writable globals, and no printing.

## 12. Standard library

Imported by name; names are unqualified at the call site.

| Module | Contents |
|---|---|
| `algorithm` | `sort`, `sortInts`, `sortInt64s`, `sortFloats`, `binarySearch`, `reverseInts`, `sortIndicesByKey`, and generic forms -- plus `sortBy` / `minByIndex` / `findIf`, which take a key or a predicate as a `Func` |
| `map` | `MapIntInt`, `MapIntStr`, `MapStrInt`, `MapStrStr`, and pointer-keyed variants |
| `set` | sets over the same key types |
| `vec` | growable arrays: `vector T` is not implemented, this replaces it |
| `str` | `split`, `replace`, trim, find, case conversion, number formatting |
| `hash` | FNV-1a, CRC-32, xxHash-style hashing over both scalars and bytes |
| `time` | wall clock, monotonic clock, sleep, ISO-8601 parse and format, calendar arithmetic |
| `sync` | atomics, mutexes, semaphores, channels |
| `random` | seeded generator |
| `test` | `testGroup`, `check`, `checkInt`, `checkInt64`, `checkInt128`, `checkString`, `testFinish`, and `testCase` / `testRunAll` for a registered case table |

```axiom
function import (return:(algorithm, str));

function main():void {
    int a[5] = {5, 3, 1, 4, 2};
    sort(#a[0], 5);
    for x in a { print(x); }
    print("\n");

    // Splitting returns a list rather than an array: the number of parts is not
    // known until it runs, and array lengths are part of the type.
    SplitList parts = makeSplit("a,b,c", ",");
    print(splitAt(#parts, 1)); print(" ");
    print(splitCount(#parts)); print("\n");
    splitFree(#parts);          // the elements are alloc'd; release them
    eof: pass;
}
```

```
── output ──
12345
b 3
```

### 12.1 Writing tests with the test module

```axiom
function import (return:(test));

function main():void {
    testGroup("arithmetic");
    checkInt(2 + 2, 4, "addition");
    checkInt(6 * 7, 42, "multiplication");
    checkInt128(1, 1, "a 128-bit check");
    testFinish();
    eof: pass;
}
```

```
── output ──
-- arithmetic --
[PASS] addition
[PASS] multiplication
[PASS] a 128-bit check
== 3 passed, 0 failed ==
```

A failing check makes `testFinish` exit non-zero, which is what the test suite relies on.

`testFinish` prints the summary and exits non-zero if anything failed, which is what the
test suite relies on.

## 13. Errors and diagnostics

Axiom has no exceptions. Three mechanisms cover the ground:

1. **Traps** — a runtime error stops the program with a message and a non-zero exit.
   Arithmetic overflow, division by zero, an out-of-range `as`, a null dereference, a
   failed bounds check, and an exhausted instruction budget are all traps.
2. **Sentinel returns** — the standard style for recoverable failures: a pointer function
   returns `null`, an integer function returns `-1` or 0, and the caller checks.
3. **`assert`** — a condition that must hold; compiled out under `--no-assert` and at `-O4`.

```axiom
int a = 2147483647;
int b = a + 1;      // traps: signed overflow in 'main'
```

The trap message names the function and the operation, because locating the overflow only at
program level is not actionable.

Overflow is exact rather than wrapping: each `intN` traps at its own boundary. Unsigned
arithmetic wraps modulo 2^N, as the specification requires.

## 14. Writing efficient code

Axiom executes on an interpreter by default and on a compiler with `--jit`. The two tiers are
rewarded by different properties, so this section is divided accordingly.

### 14.1 Properties rewarded by both tiers

The cost model is instruction count plus per-instruction overhead, in approximate order of
impact:

1. **Move per-element work into C.** The standard library does this: `sort` and its relatives
   take `(ptr T, count)` and operate on the whole array in one call. A million-element loop in
   Axiom costs a million dispatches; the same loop in `stdlib/algorithm.c` costs one.
2. **Walk with a pointer rather than an index.** `@p` and `p++` are one instruction each;
   `data[i]` is a bounds check, a scale and an add. The optimizer fuses `buf[i]` into a single
   instruction when the index is a variable and `buf` a global, but a pointer walk has no
   bounds check to perform at all.
3. **Prefer arrays and pointers to `map`.** Hash lookup is a function call and a probe
   sequence; indexing is an add and a load.
4. **Keep frequently used values in locals.** A local resides in the frame and is loaded
   directly; a global is reached through the globals table.
5. **Avoid allocation inside loops.** `alloc` is cheap per call, but a region is not reclaimed
   until it ends, so a loop that allocates grows memory monotonically.
6. **Do not obstruct the optimizer.** At `-O2` it folds constants, propagates locals, removes
   dead stores and threads jumps. A `comptime` computation the program depends on should be
   confirmed eliminated with `--stats-detail`.

### 14.2 Behaviour of the two tiers

`--jit` compiles a whole function or refuses it; there is no partial compilation and no
re-profiling. What it cannot compile is handed to the interpreter at the instruction it
reached, so the program remains correct and loses only speed.

- **Loops over locals gain most from the JIT.** A loop body in registers is 4–18× faster than
  the same body dispatched. When a function is refused -- `--jit` reports the reason, and
  `ACVM_JIT_DEBUG=1` reports it per function -- the cause is usually an opcode the lowering
  does not implement, and the remedy is usually to move that operation outside the loop.
- **Calls are the weakest point of the JIT.** Each invocation costs one C++ frame. Computation
  distributed across many small functions gains less than the same computation in one loop.
- **Recursion gains least.** A recursive function pays the call at every level.
- **Memory access still traverses the page table.** Neither tier has a flat address space, so
  an access does not cost what it costs in C.

Measured on `examples/quicksort_max.ax` (50 000 integers, one algorithm implemented for four
engines): the interpreter is 2.1× behind CPython 3.13, `--jit` is 2.1× ahead of it, and C++
`-O2` is 6.2× ahead of `--jit`. The measurements and their method are in
[`README.md`](README.md#performance) and [`docs/performance.md`](docs/performance.md).

The residual gap to C++ is per-dispatch and per-call cost rather than algorithmic: the
quicksort executes a comparable number of operations per element to CPython's bytecode count.

## 15. Conventions

The conventions the codebase follows:

- **Comments state why, not what.** The code states what it does; a comment justifies itself by
  recording the alternative that was rejected, or the failure that forced the design.
- **Record the failure in the comment.** Where a rule exists because something broke, name what
  broke. `// do not merge these: it produced a wrong answer at O2` is more durable than any
  amount of `// careful!`.
- **One blank line between functions**, and a section banner before a group of related ones.
- **Prefer early returns**; they state the exit conditions first.
- **No abbreviations in public names**; `count`, not `cnt`. Loop counters may be `i`.
- **Test names describe behaviour**, not the function under test: `"the false arm when the
  condition is false"`.

## 16. Common errors

Recorded from observed mistakes:

| Error | Result |
|---|---|
| Omitting `;` after a `struct` | `expected ';' after the struct declaration` |
| Treating `a` as a pointer | The array is copied; pass `#a[0]` with a length |
| `s.length()` | `length` is a property: `s.length` |
| `if (n)` with an integer | The condition must be `bool` |
| Assuming `int` is 64-bit | `int` is `int32`; use `int64` explicitly |
| Assuming `int8 + int8` yields `int8` | Operands are widened; the result follows the wider type |
| `switch` with fallthrough | Exactly one arm executes |
| Assigning `a` to `b` expecting shared state | Aggregates are values; the copies are independent |
| `<` on a struct | Compile error; compare a field or sort by key |
| Depending on `eof` after a trap | `eof` is not `finally`; a trap skips it |
| Deep recursion | No tail-call elimination; the frame stack grows |
| Reading an uninitialised variable | Compile error, not garbage |

## 17. Cheat sheet

The sheet is a reference rather than a program, so it is fenced as text.

```text
// program
function main():void {
    eof: pass;
}

// types
int x = 1;              int8 a = 1;        int128 w = 1;
unsigned int32 u = 1;   float f = 1.5;     double d = 1.5;
char c = 'A';           bool b = true;     string s = "text";
bit1 flags[8];          bit256 digest = 0;   // packed bits, not a number
ptr int p = null;       int arr[3] = {1,2,3};   (int, string) t = (1, "a");
Func<int, int> f = twice;               // a function value: returns int, takes one

// declaration
int v = 0;              const int k = 1;    comptime int K = 1;
struct S { int a; };    union U { int i; float f; };
enum E { A, B = 5, C }; module "name" version "0.1.0";

// control
if (b) { } else { }
while (b) { }
for (int i = 0; i < n; i++) { }
for i in 0..n { }
for x in arr { }
loop (3) { }
switch (n) { case 1: ; default: ; }
break; continue; outer: for i in 0..2 { }

// operators
a + b   a % b   a == b   a && b   !b   a << 2   a & b   a ** 2
#value          // address of
@pointer        // dereference
value as int8   // checked conversion
sizeof(S)       alignof(S)      sizeof(T) in a generic

// functions
function f(int a, int b):int { return a + b; eof: pass; }
function g<T>(T a):T { return a; eof: pass; }
Func<int, int, int> op = f;   op(1, 2);      // named as a value, called through it
function h(Func<int, int> k):int { return k(3); }   // passed as a parameter

// struct with methods
struct P {
    int x;
    function get(self):int { return self.x; }
    function set(ptr self, int v):void { self->x = v; }
    function make():P { P p = {0}; return p; }
};

// modules
function import (return:(algorithm json str test sync));
import "path/to/file.ax";

// concurrency
global int shared = 0;
function process worker():void { yield(); eof: pass; }
function event onCond()(shared == 1):void { eof: pass; }
atomicAdd(#shared, 1);

// allocation
ptr int buf = alloc(64) as ptr int;
free(buf);

// output
print("text");  print(42);  print("\n");
```
