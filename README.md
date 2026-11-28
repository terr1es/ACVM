# ACVM — the Axiom compiler and virtual machine

ACVM compiles the Axiom language (`.ax`) to stack-based bytecode and executes it. Bytecode
can be emitted as a self-contained `.acvm` module that runs without the compiler.

```
source .ax ──► Lexer ──► Parser ──► Sema ──► IRGen ──► Optimizer ──► Module ──► VM
                                  (type check)                     (bytecode)    │
                                                                    ├──► .acvm file
                                                                    └──► disassembly
```

## Design properties

- **C++20, no third-party dependencies.** The only external symbol is the platform's dynamic
  loader.
- **Hand-written front end.** Lexer, recursive-descent parser with a Pratt expression parser,
  and two-pass semantic analysis. No parser generator.
- **Own bytecode and VM.** Paged memory, exact arithmetic at every width from `int2` to
  `int128`, element-scaled pointer arithmetic, value-semantic aggregates, region (arena)
  allocation, single-exit functions via `eof`, and cooperative scheduling with
  compiler-inserted safe points.
- **Two execution tiers.** An interpreter that defines the semantics, and an optional
  x86-64 baseline compiler (`--jit`) that is validated against it differentially.

## Implementation size

| Layer | Lines |
|---|---:|
| Compiler and VM (`src/`, `include/`) | 28,994 C++ |
| Standard library and extra modules (`stdlib/`, `extralib/`) | 21,132 Axiom, 7,692 C |
| Tests (`tests/`) | 10,247 Axiom, 3,192 C++ |
| Examples (`examples/`) | 3,599 Axiom |
| Tooling and editor support | 229 Axiom, 1,147 C/C++ |
| **Axiom total** | **35,207** |
| **C/C++ total** | **41,025** |

Axiom is not in [linguist's language database](https://github.com/github-linguist/linguist/blob/main/lib/linguist/languages.yml),
so GitHub reports `.ax` files under no language. The TextMate grammar in [`editors/`](editors/)
provides syntax highlighting.

## Building

Requires CMake ≥ 3.20 and a C++20 compiler. Tested with clang 23 (llvm-mingw) on Windows and
GCC/Clang on Linux.

```bash
cmake -S . -B build
cmake --build build -j
```

The build installs the `acvm` binary to `~/.local/bin` (`%USERPROFILE%\.local\bin` on
Windows), which is normally on `PATH`. To disable installation:

```bash
cmake -S . -B build -DACVM_INSTALL_COMMAND=OFF
```

Where CMake is not on `PATH`, the full invocation is:

```bash
"/c/Program Files/CMake/bin/cmake.exe" -S . -B build -G "MinGW Makefiles" \
  -DCMAKE_CXX_COMPILER="C:/llvm-mingw/llvm-mingw-20261104-ucrt-x86_64/bin/clang++.exe"
"/c/Program Files/CMake/bin/cmake.exe" --build build -j 8
```

Artifacts are written to `build/bin/`.

## Command line

### Compilation and execution

```bash
acvm run   examples/tour.ax                           # compile and execute
acvm build examples/tour.ax                           # emit examples/tour.acvm
acvm exec  examples/tour.acvm                         # execute a compiled module
acvm check examples/tour.ax                           # lex, parse and type check
acvm run   examples/quicksort_max.ax --jit            # compile functions to machine code
acvm run   examples/quicksort_max.ax --stats-detail   # per-opcode instruction counts
acvm ir    examples/tour.ax -O2                       # disassemble after optimizing
```

### Inspection and scaffolding

```bash
acvm size examples/tour.ax          # bytecode size per function, largest first
acvm doc  stdlib -o docs/api.md     # Markdown reference from comments
acvm test tests                     # run every *_check.ax in a directory
acvm init myproject                 # project skeleton
```

`size` ranks functions by bytecode size. `doc` parses the source rather than the syntax tree,
so it operates on files that do not compile. `test` runs each file in a separate process, so a
trapping test does not terminate the suite. `init` writes a program and a test that pass.

### Optimization levels

| Level | Effect |
|---|---|
| `-O0` | None. The instructions IRGen emitted. |
| `-O1` | Local peepholes, unreachable-code removal, jump threading. |
| `-O2` | Default. Also constant propagation and folding, branch resolution. |
| `-O3` | Also store/load forwarding and dead-store removal. |
| `-O4` | Also removes safe points and assertions. **Not semantics-preserving.** |

`-O0` through `-O3` preserve observable behaviour. `-O4` removes safety checks and scheduler
safe points, so programs that depend on preemption or on an assertion firing behave
differently. This is a documented contract, not a defect.

## Language overview

```axiom
struct Point {
    int x;
    int y;

    function lengthSquared(self):int {
        return self.x * self.x + self.y * self.y;
    }
};

function maxOf<T>(T a, T b):T {
    if (a > b) { return a; }
    return b;
    eof: pass;
}

function main():void {
    Point p = {3, 4};
    print(p.lengthSquared());        // 25
    print("\n");

    int8  small = 100;               // one byte, exact
    int128 wide = 1;                 // -2^127 .. 2^127-1
    wide = wide << 100;

    int values[5] = {5, 3, 1, 4, 2};
    sortInts(#values[0], 5);         // in place: 1 2 3 4 5

    string name = "axiom";
    print(name.length);              // 5
    print("\n");
    eof: pass;
}
```

### Differences from C

| Property | Axiom | C |
|---|---|---|
| Integer types | `int8`/`int16`/`int32`/`int64`/`int128`, any width 2–128 | `int8_t`… |
| `int` default | `int32` | platform-dependent |
| Implicit numeric conversion | none; narrower operands are widened to the wider type | implicit promotions |
| Function body terminator | `eof: pass;` | `}` |
| Statement terminator | `;` required; function bodies do not end with one | both |
| Aggregate comparison | `==`/`!=` only; `<` on an aggregate is a compile error | not applicable |
| Struct size | field layout only; methods occupy no storage | not applicable |
| Duplicate function definitions | error; `private` is file-scoped | tentative definitions |

[`TUTORIAL.md`](TUTORIAL.md) documents the complete language, including every deliberate
deviation from the specification.

## Performance

All figures are produced by `docs/bench/compare.py` on one machine, with process startup
measured and subtracted for each engine. Each value is the minimum of eleven runs.

`examples/quicksort_max.ax` is a quicksort reduced to a low instruction count: 50,000
integers, Hoare partitioning with median-of-three by comparison, insertion sort below 20
elements, recursion into the smaller side. `docs/bench/qs.cpp` implements the same algorithm
with the same generator, threshold and ordering, and prints identical inversion counts.

| Engine | Time | vs CPython 3.13 | vs C++ `-O2` |
|---|---:|---:|---:|
| ACVM interpreter | 98.6 ms | 2.1× slower | 27× slower |
| ACVM `--jit` | 22.4 ms | 2.1× faster | 6.2× slower |
| CPython 3.13 | 46.6 ms | — | 13× slower |
| C++ `-O0` (clang) | 4.3 ms | 11× faster | 1.2× slower |
| C++ `-O2` (clang) | 3.6 ms | 13× faster | — |

Run-to-run variation is 2–5%; ratios are accurate to two significant figures.

The interpreter is within 2.1× of CPython on a pointer-heavy program. `--jit` places it 2.1×
ahead of CPython and 6.2× behind unoptimized C++. The remaining gap to C++ follows from
absent inlining, from register allocation limited to a few pinned slots, and from a call that
traverses one C++ frame per invocation.

Per-benchmark comparison against CPython, same run:

| Benchmark | Interpreter | `--jit` | CPython 3.13 | `--jit` vs CPython |
|---|---:|---:|---:|---:|
| empty loop | 426.9 ms | 99.5 ms | 938.1 ms | 9.4× faster |
| arithmetic | 483.4 ms | 24.0 ms | 441.1 ms | 18.4× faster |
| array walk | 208.3 ms | 26.6 ms | 117.2 ms | 4.4× faster |
| recursion (`fib 10` × 300,000) | 2792.7 ms | 1028.0 ms | 1857.5 ms | 1.8× faster |

The interpreter is fastest only on the bare loop. Fusion reduces that loop body to four
instructions; memory access and function calls pay for a dispatch granularity the IR does not
eliminate. [`docs/performance.md`](docs/performance.md) contains the analysis, the
per-instruction costs, and the changes that were measured and reverted.

Two qualifications apply to these tables:

- **`--jit` is not a like-for-like comparison.** CPython 3.13 is a specializing interpreter
  with no compilation tier. Outperforming it with `--jit` measures the compiler, not the
  interpreter; the interpreter column is the appropriate comparison between interpreters.
- **One input size is measured.** Axiom has no command-line argument mechanism, so the
  example runs at its tuned 50,000 elements. A previous revision reported three sizes
  obtained by three different methods, which is less useful than one size measured
  consistently.

Reproduce:

```bash
./docs/bench/compare.py path/to/acvm          # Axiom and CPython
./docs/bench/compare.py path/to/acvm --build  # also C++, compiled from docs/bench/qs.cpp
```

## The JIT

`--jit` compiles whole functions to x86-64 and executes them. There are no tiers and no
profiling: a function is compiled on first call or refused, and a refusal is permanent because
the cause is a property of the code rather than of the run.

```bash
acvm run  prog.ax   --jit
acvm exec prog.acvm --jit
```

Speedup relative to the interpreter, same run as the table above:

| Program | Interpreter | `--jit` | Ratio |
|---|---:|---:|---:|
| empty loop | 426.9 ms | 99.5 ms | 4.3× |
| arithmetic | 483.4 ms | 24.0 ms | 20.1× |
| array walk | 208.3 ms | 26.6 ms | 7.8× |
| recursion (`fib 10` × 300,000) | 2792.7 ms | 1028.0 ms | 2.7× |
| `examples/quicksort_max.ax` | 98.6 ms | 22.4 ms | 4.4× |

The variation across benchmarks depends on which functions are compiled and on the ratio of
arithmetic to calls within each loop, not on code quality.

### Design decisions

**The entry function is compiled.** The interpreter reaches compiled code only through `CALL`,
so `main` previously ran interpreted regardless of its content, and `main` is where small
programs place their entire workload.

**Compilation halts at the first instruction the interpreter owns.** A partially compiled body
would have to spill every live register to memory at the unsupported instruction and reload
afterwards, which is the point at which a baseline compiler begins producing incorrect results
at the boundaries. Instead the body ends there: the frame returns to the interpreter at the
nearest preceding instruction that begins with an empty expression stack, and the remainder of
the function runs interpreted. An empty stack is the only state requiring no translation, since
a compiled body holds operands in untagged frame slots while the interpreter reads
`process.stack`.

**A program declaring a `process` keeps its entry function interpreted.** Compiled code cannot
yield, and completion of `main` terminates the run, so a compiled `main` never yields to the
scheduler and no worker process executes. Callees remain compiled.

### Lowered instructions

String literals and aggregate copies are compiled. A string is a 16-byte `{ptr, len}`
descriptor rather than a register value, so `string s = "hi";` emits `LOCAL_ADDR`, `MEM_ZERO`,
`CONST_STR` and `MEM_COPY`. All four are lowered through three VM bridges
(`constantString`, `zeroThroughPointer`, `copyThroughPointers`), which keeps them consistent
with the interpreter about literal placement and slot extent.

This affects more than it appears to. Compilation is all-or-nothing per function, so a single
unused string literal previously caused the entire function to be refused:

| 20,000,000-iteration loop | Interpreter | `--jit` |
|---|---:|---:|
| without a string | 2681 ms | 170 ms |
| with one unused `string label = "...";` | 2676 ms | 165 ms |

Before string lowering, the second row measured 2865 ms — 8% slower than the interpreter,
because the tier paid to analyse a function it then refused. `tests/jit_string_check.ax`
covers this, including a literal passed to an interpreted callee.

Recursion and ordinary calls are compiled. Two defects previously obscured this, both now
covered by `tests/jit_call_check.ax`: `!x` was lowered as a bitwise complement, so every
negated condition took the wrong branch; and a safe point with call arguments already pushed
yielded, after which the interpreter resumed from a value stack that did not contain them.

Calls are covered in both directions. `tests/jit_mixed_call_check.ax` exercises a compiled
function calling a refused one, mutual recursion, and arguments beyond the fourth, where the
x64 ABI stops passing them in registers.

### Refusals

| Refusal | Cause |
|---|---|
| struct returned by value | the caller supplies a destination slot |
| return value wider than a register | `int128` has no single-register form |
| unsigned division | no single x86-64 instruction matches the interpreter's rules |
| `process` body | deliberate: compiled code cannot yield and therefore cannot be preempted |
| interpreter needed at the first instruction | the hand-off point would be the function's first instruction, leaving only a prologue |
| indirect call (`CALL_INDIRECT`) | the target is a run-time value |
| frame wider than eight pages | frame pages are pinned at entry and addressed through a table |

`--stats` reports the outcome:

```
jit: 4 compiled, 0 refused                                    (quicksort_max.ax)
jit: 4 compiled, 1 refused (first refusal: in 'printRun': unsupported opcode BUILTIN)
```

`ACVM_JIT_DEBUG=1` reports every compilation decision, which identifies why a specific
function remained interpreted.

Under `--jit` the instruction count in `--stats` is not comparable to an interpreted run: a
compiled body executes as one native block, so the count covers only the bytecode that ran.
Wall-clock time is the measure to compare.

### Backend and validation

The backend targets x86-64 without a run-time CPU probe. On any other architecture `--jit` is
an error rather than a silent fallback, because a silent fallback is indistinguishable from a
JIT that produced no improvement. The JIT tests are skipped on those hosts for the same reason.

Correctness rests on differential testing: fourteen programs run interpreted and compiled, with
byte-identical output required (`tests/compare_jit.cmake`). A fifteenth test covers the encoder
in isolation.

Two properties of the harness require emphasis, as both were initially absent and both made
the suite appear stronger than it was:

- **Matching output is insufficient.** A program whose assertions all fail exits non-zero in
  both modes, so the runs agree and a comparison-only test passes. The harness also requires
  the interpreted run to exit zero. `EXPECT_RC` overrides this for programs intended to fail.
- **A minimum compiled count is asserted.** A program in which every function is refused still
  matches the interpreter, so tests intended to exercise compiled code assert a floor on the
  number of functions compiled. `jit_mixed_call_check.ax` pins an exact count, since one of its
  functions exists only to be refused.

## Function values: `Func<...>`

A function can be stored, passed and invoked through a value:

```axiom
function import (return:(io));

function add(int a, int b):int { return a + b; }
function mul(int a, int b):int { return a * b; }
function twice(int a):int { return a * 2; }
function square(int a):int { return a * a; }

function apply(int value, Func<int, int> transform):int {
    return transform(value);
}

function main():void {
    Func<int, int, int> op = add;
    print(op(2, 3));          // 5

    op = mul;                 // the same slot, a different callee
    print(op(2, 3));          // 6

    Func<int, int> table[2] = {twice, square};
    print(table[1](4));       // 16
}
```

The signature uses the type-argument syntax already used by generics, with the return type
first, so `Func<int, int, int>` returns `int` and accepts two `int` arguments. `Func` is an
ordinary identifier, not a keyword.

**A function value is one word.** It holds the callee index plus one; zero denotes "never set",
so invoking one traps rather than calling the module's first function. There is no
environment, since Axiom has no closures to capture one (specification Q99), which is why
`sizeof(Func<int, int>)` is 8.

**Positions accepting a function name as a value:** an initialiser, an assignment, an array
element, a struct field, or an argument to a parameter of function type — including a generic
function's parameter, so `sortBy(points, n, byX)` infers `T` from the data and resolves the key
signature after instantiation. Not a call site: type arguments are not written at calls
(`f<int>(x)` parses as a comparison), so only the destination can indicate which overload is
intended. An ambiguous signature is a compile error, as is one matching nothing.

**A compiled function never contains an indirect call.** `CALL_INDIRECT` makes the function a
hand-off point for the JIT, so it runs interpreted from there: the call graph ceases to be
static, and everything derived from it — inlining, dead-code elimination, the debugger's frame
walk — would require a run-time lookup. Excluding it from compiled code keeps the compiled
portion static. The cost is bounded: `algorithm`'s `sortBy` compiles, and only the comparison
loop that invokes the key does not.

**A function value is permitted wherever a `Func` type can be written**, including a global, an
array element and a struct field. The following is a fragment, not a complete program:

```axiom
Func<int, int> table[2] = {twice, square};   // a dispatch table
Func<int, int> handler = twice;               // a global, initialised at startup

struct Cmd { string name; Func<int, int> run; };
Cmd cmd = {"double", twice};
print(cmd.run(21));                           // 42 -- a field, called as a method
```

Globals were the final gap: file-scope initialisers were not type-checked, so a global `Func`
was silently zeroed and `global int x = "hello";` compiled. Type checking now runs in a
dedicated pass after every declaration is in scope, because `Func<int, int> h = serve;` may
name a function declared later in the file.

## Standard library

The standard library and the extra modules are written in Axiom and tested.

| Directory | Contents |
|---|---|
| `stdlib/` | `algorithm`, `map`, `hash`, `sync`, `time`, `str`, `set`, `vec`, `astl`, `random`, `test` |
| `extralib/` | `json`, `xml`, `yaml`, `toml`, `ini`, `csv`, `http`, `net`, `inflate`, `sha256`, `glob`, `cli`, `value` |

Three modules changed shape once `Func<...>` existed, each having a gap that only a function
value could close:

| Module | Previously impossible |
|---|---|
| `algorithm` | `sortBy(ptr T, int, Func<int, T>)` orders structs by one field, stably. `sort()` knows only the built-in rules, so struct sorting had to be written by hand. Also `minByIndex`/`maxByIndex` and the predicate family `findIf`/`findLastIf`/`countIf`/`anyOf`/`allOf`/`noneOf`. |
| `test` | `testCase(name, body)` registers a case; `testRunAll()` runs the table. A test file no longer ends in a hand-maintained call list where adding a group required editing two places. |
| `cli` | `cliDispatch(a, names, handlers, n, fallback)` matches `cliCommand(a)` against a name table and invokes the handler. |

`io` gained `readWholeFile` and `readLineList`, unrelated to `Func`: every parser in
`extralib` takes a `string`, and the only way to obtain one from a file was to guess a buffer
size. With `readTextFile` refusing rather than truncating, that guess bounded which files a
program could open.

`extralib/xml.ax` is an XML parser written in Axiom, `extralib/inflate.ax` a DEFLATE decoder,
and `extralib/http.ax` an HTTP/1.1 client with chunked decoding. Each declares its identity and
version in a declaration that must match the file name:

```text
module "json" version "0.1.0";
```

## Testing

```bash
ctest --test-dir build --output-on-failure
```

128 tests. Four categories:

- **`*_check.ax` programs** assert and report pass/fail, so a failure names a behaviour rather
  than a byte offset.
- **Differential tests** run each script at `-O0`, `-O3` and `-O4` and require byte-identical
  output. Single-level tests cannot detect an optimizer that computes a wrong result only
  after folding, because the `-O0` run succeeds and conceals it.
- **JIT differential tests** run each program interpreted and compiled and require identical
  output, with a floor on the compiled function count.
- **Diagnostic tests** require the compiler to reject a program with a specific message,
  because a silently accepted program that should have been an error is the most costly
  failure mode.

`tools/verify_tutorial.py` extracts every complete program from `TUTORIAL.md`, executes it, and
compares the output against the document.

```bash
python tools/verify_tutorial.py
```

## Repository layout

```
src/          Lexer, Parser, Sema, IR, IRGen, Optimize, VM, Driver
include/      Headers, mirroring src/
stdlib/       Standard library (.ax), native declarations (.axd), C sources (.c)
extralib/     Modules built on the standard library
tests/        Script tests, C++ unit tests, differential harnesses, fixtures
examples/     Runnable programs
docs/         Performance analysis and the benchmark harness
tools/        verify_tutorial.py and the acvm wrapper
editors/      A TextMate grammar for syntax highlighting, and sample files
brand/        The logo as SVG
```

## Status and known limitations

The implementation follows the Axiom language specification with a documented list of
deviations. The most significant:

- `vector T` is not implemented; `vec.ax` provides an equivalent container.
- `intN` is signed, so `int127` reaches 2^126−1. `int128` is supported as an extension so the
  specification's own example is valid. `unsigned int128` is refused because its maximum does
  not fit the VM's signed 128-bit cell.
- `doubleA.B` parses but is stored as binary64.
- Aggregate `<` is rejected. `==` and `!=` compare storage bytes.
- The JIT compiles a subset. String literals, aggregate copies, `GLOBAL_ADDR`, `TRAP` and the
  fused array load are lowered; builtins, global variable access, library calls and indirect
  calls are hand-off points, so a function is compiled up to the first of these and interpreted
  from there. A function whose first instruction requires the interpreter is refused. The
  interpreter is the fallback and the definition of correct behaviour, which means a new fused
  instruction is a change to both tiers: if the JIT is not taught it, a compiled loop silently
  loses the fusion and runs interpreted.
- `--jit` is disabled while `--adb` is attached: compiled code has no instruction-level address
  mapping, so a breakpoint inside it cannot stop execution.
- A program declaring a `process` keeps its entry function interpreted, because compiled code
  cannot yield and completion of `main` terminates the run.

Not implemented: tagged variants (`union` with an automatic tag and `match`), operator
overloading, and constraints on generic type parameters (`T: Comparable`).

## License

MIT. See [`LICENSE`](LICENSE).
