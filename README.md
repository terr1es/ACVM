# ACVM — the Axiom compiler and virtual machine

ACVM compiles the Axiom language (`.ax`) to stack-based bytecode and executes it. The
bytecode can also be saved as a self-contained `.acvm` module that runs without the
compiler.

```
source .ax ──► Lexer ──► Parser ──► Sema ──► IRGen ──► Module ──► VM
                                  (type check)      (bytecode)    │
                                                       ├──► .acvm file
                                                       └──► disassembly
```

- **C++20, zero third-party dependencies.** The only external symbol is the platform's
  dynamic loader. A full build is `cmake -S . -B build && cmake --build build`.
- **Hand-written front end:** lexer, recursive-descent parser with a Pratt expression
  parser, and two-pass semantic analysis. No parser generator.
- **Own bytecode and VM:** paged memory, exact arithmetic for every `int2`..`int128`
  width, pointers with element-scaled arithmetic, value-semantic aggregates, region
  (arena) allocation, single-exit functions via `eof`, and cooperative scheduling with
  compiler-inserted safe points.

Axiom is the largest language in this repository — roughly 32,000 of its 66,000 lines,
against 24,000 of C++. GitHub does not show that yet: its language detection knows only
the languages in [linguist's database](https://github.com/github-linguist/linguist/blob/main/lib/linguist/languages.yml),
and Axiom is not one of them, so `.ax` files appear under no language at all. Registering
it needs a minimum volume of use across repositories that a new language has not reached.
The grammar in [`editors/`](editors/) is what makes the code readable in an editor today.


## Building

Requires CMake ≥ 3.20 and any C++20 compiler. Tested with clang 23 (llvm-mingw) on
Windows and GCC/Clang on Linux.

```bash
cmake -S . -B build
cmake --build build -j
```

The build copies the `acvm` binary to `~/.local/bin` (`%USERPROFILE%\.local\bin` on
Windows). That directory is normally already on `PATH`, so afterwards you can type
`acvm` directly. To leave `PATH` alone:

```bash
cmake -S . -B build -DACVM_INSTALL_COMMAND=OFF
```

On a machine where CMake is not on `PATH`, the full invocation used during development
was:

```bash
"/c/Program Files/CMake/bin/cmake.exe" -S . -B build -G "MinGW Makefiles" \
  -DCMAKE_CXX_COMPILER="C:/llvm-mingw/llvm-mingw-20261104-ucrt-x86_64/bin/clang++.exe"
"/c/Program Files/CMake/bin/cmake.exe" --build build -j 8
```

Artifacts land in `build/bin/`.

## Usage

```bash
acvm run   examples/tour.ax                    # compile and execute
acvm run   examples/quicksort.ax               # readable quicksort
acvm run   examples/quicksort_max.ax --stats-detail   # instruction-level counts
acvm build examples/tour.ax                    # emit examples/tour.acvm
acvm exec  examples/tour.acvm                  # execute the module
acvm run   examples/quicksort_max.ax --jit     # compile hot functions to machine code
acvm check examples/tour.ax                    # lex, parse and type-check only
acvm ir    examples/tour.ax -O2                # disassemble after optimizing
```

Optimization levels are `-O0` (none), `-O1`, `-O2` (default), `-O3`, and `-O4`.
**O0–O3 preserve observable semantics; O4 does not** — it removes safety checks and safe
points, so programs that depend on preemption or on an assertion firing behave
differently. That is a deliberate contract, not a bug.

## A quick look at the language

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

    int8  small = 100;               // one byte, exactly
    int128 wide = 1;                 // full 128-bit range: -2^127 .. 2^127-1
    wide = wide << 100;

    int values[5] = {5, 3, 1, 4, 2};
    sortInts(#values[0], 5);         // in place: 1 2 3 4 5

    string name = "axiom";
    print(name.length);              // 5
    print("\n");
    eof: pass;
}
```

Things that surprise people coming from C:

| | Axiom | C |
|---|---|---|
| Integer types | `int8` / `int16` / `int32` / `int64` / `int128`, and any width from 2 to 128 | `int8_t`… |
| Default `int` | `int32` | platform-dependent |
| Implicit numeric conversion | **none** — but narrower operands are widened to the wider one | implicit promotions |
| Function bodies | closed by `eof: pass;`, not by `}` | `}` |
| Statements | require `;`; function bodies do **not** end with one | both |
| Aggregate comparison | `==` / `!=` only; `<` on an aggregate is a compile error | n/a |
| Struct size | field layout only — **methods take no storage** | n/a |
| Function collisions | two imported files defining the same signature is an error; `private` is file-scoped | tentative definitions |

Details, including every deliberate deviation from the language specification, are in
[`TUTORIAL.md`](TUTORIAL.md).

## Performance

`examples/quicksort_max.ax` is a quicksort tuned down to the instruction floor. The same
algorithm on the same data (cross-checked by checksum) across three engines:

| n | ACVM | CPython 3.13 | C++ `-O0` | C++ `-O2` |
| ---: | ---: | ---: | ---: | ---: |
| 50 000 | 89.2 ms | 33.5 ms | 2.83 ms | 1.90 ms |
| 200 000 | 386 ms | 156 ms | 10.84 ms | 7.47 ms |
| 1 000 000 | 2064 ms | 1082 ms | 53.1 ms | 37.0 ms |

- 31–39× slower than unoptimized C++, 47–56× slower than `-O2`
- 1.9–2.7× slower than CPython 3.13

The CPython comparison is the interesting one. Counting with `sys.settrace`, CPython
executes **179 bytecodes per element** and ACVM executes **175 instructions** — the same
amount of work. The remaining difference is the cost of a single dispatch, about 10 ns
against CPython's 3.3 ns.

That gap used to be 3.8× rather than 3.0×. The dispatch loop was the first thing worth
attacking, because it was paying for structure rather than for work: every instruction
re-read `frames.back()`, copied the `Frame`, re-derived the code pointer and the base
address, and returned through an `enum` the caller switched on. Hoisting that into a loop
with the instruction pointer held in a register removed roughly a quarter of the wall
clock, and a fast path for native-width integers removed a little more — the generic
arithmetic path works in 128 bits with masks and range checks at every width, which is
right for `int128` and wasteful for an `int`.

What remains is not dispatch overhead. It is the 32-byte `Value` copy on every push and
pop, memory access through a page table rather than a flat array, and a `MemType` lookup
per instruction. Closing that means changing the representation of `Value` and the memory
model, not adding more fused instructions — the instruction count is already at the floor.

These numbers were measured on one machine (llvm-mingw, Windows x86-64) and are reported
as ratios rather than absolute times for that reason. The ACVM column is derived from the
original cross-engine run by applying the factor measured for the dispatch rework on a
single machine (1.23× at 50 000, 1.25× at 200 000, 1.29× at 1 000 000); the other three
columns were not re-run, so they remain as measured together.

## The JIT

`--jit` compiles whole functions to x86-64 and runs the result. There are no tiers and no
profiling: a function is compiled the first time it is called, or refused, and a refusal
is permanent because the reason is a property of the code rather than of the run.

```bash
acvm run  prog.ax   --jit
acvm exec prog.acvm --jit
```

Measured on the same machine as the table above, best of three:

| Program | Interpreter | `--jit` | |
|---|---:|---:|---|
| 20 000 000 iteration loop | 3020 ms | 189 ms | 16× |
| `examples/quicksort_max.ax` | 218 ms | 112 ms | 1.9× |
| `examples/quicksort.ax` | 94 ms | 89 ms | 1.06× |

The spread is the whole story: **a leaf function is where the 16× is.** `quicksort_max`
now compiles all four of its functions including the recursive partition, but each call
still goes through the interpreter's calling convention — arguments pushed onto the value
stack one helper call at a time, a frame zeroed, the page table consulted — so the win
there is bounded by call overhead rather than by arithmetic. `quicksort.ax` is dominated
by startup and printing and gets almost nothing.

**Compilation is all or nothing, per function.** A half-compiled body would have to flush
every live register back to memory at the unsupported instruction and reload afterwards,
which is where a JIT stops being simple and starts producing wrong answers at the edges.
So anything the lowering does not handle refuses the whole function, and these are the
refusals most likely to be met:

| Refusal | Why |
|---|---|
| `unsupported opcode BUILTIN` | `print`, `eprint` and the other builtins call back into the interpreter |
| `call` to a `library` function | the native call is marshalled by the interpreter |
| a global (`LOAD_GLOBAL`) | globals live in a different address space |
| a struct returned by value | the caller supplies a destination slot |
| `STR_EQ`, `CONCAT` | string comparison and concatenation are interpreter builtins |
| `int128`, floats, constants past 64 bits | the lowering works in 64 bit registers |
| unsigned division | no single x86-64 instruction matches the interpreter's rules |
| a `process` body | deliberate: compiled code cannot yield, so it could not be preempted |

The first row is the one most often met in practice, and it is also the one that limits the
measured speedup: a function is refused for containing a single `print`, so programs that
report their own progress stay interpreted however much arithmetic they do.

**String literals and aggregates are compiled.** A string is not a register value: it is a
16 byte `{ptr, len}` descriptor, so `string s = "hi";` is four instructions --
`LOCAL_ADDR`, `MEM_ZERO`, `CONST_STR`, `MEM_COPY`. All four are lowered, through three
bridges that call into the VM (`constantString`, `zeroThroughPointer`,
`copyThroughPointers`), which is what makes them agree with the interpreter about where a
literal lives and how much of a slot it fills.

This mattered more than it looks. Before it, one unused string literal refused the whole
function, and compilation is all or nothing per function, so a hot loop with a leftover
label ran interpreted end to end:

| 20 000 000 iteration loop | Interpreter | `--jit` | |
|---|---:|---:|---|
| with no string | 2681 ms | 170 ms | 16× |
| with one unused `string label = "...";` | 2676 ms | 165 ms | 16× |

The second row was 2865 ms before the string was lowered -- the JIT made it 8% *slower*,
because it paid to look at a function it then refused. `tests/jit_string_check.ax` holds
this down, including the case where the literal crosses into an interpreted callee.

Recursion and ordinary calls *are* compiled. Two bugs made that look false for a while,
both now covered by `tests/jit_call_check.ax`: `!x` was lowered as a bitwise complement
(so every negated condition took the wrong branch, and it was already wrong for leaf
functions), and a safe point with call arguments already pushed yielded, after which the
interpreter resumed from a value stack that did not have them.

Calls work in both directions. `tests/jit_mixed_call_check.ax` holds down the one the other
tests could not reach — a compiled function calling one that was refused — since every
function in `jit_call_check.ax` compiles, so the "compiled into interpreted" boundary was
never crossed. It also covers mutual recursion and arguments past the fourth, which is where
the x64 ABI stops passing them in registers.

Lowering the string opcodes exposed a third bug, this one in the stack-depth analysis rather
than in the encoder: a call returning an aggregate was modelled as popping its destination
and pushing nothing, when `Ret` pushes the result whatever its type. Every `T x = f();` with
a struct or string therefore looked like it underflowed two instructions later, so the
refusal named `MEM_COPY` -- which was fine -- instead of the aggregate return that was
actually unsupported. Twelve test programs reported it that way. The message now names the
real cause, and a misleading diagnostic is worth fixing on its own: it cost more time to
find than the fix took.

`--stats` reports what happened:

```
jit: 4 compiled, 0 refused                                    (quicksort_max.ax)
jit: 4 compiled, 1 refused (first refusal: in 'printRun': unsupported opcode BUILTIN)
```

The refusal names the function and the instruction, so a function that quietly stayed
interpreted can be recognised from a single run.

Under `--jit` the instruction count in `--stats` is not comparable to an interpreted run:
a compiled body executes as one native block, so the count only covers what still ran as
bytecode. Compare wall clock, not instructions.

The backend targets x86-64 with no run-time CPU probe, because a 64-bit x86 CPU lacking
the handful of instructions used here does not exist. On any other architecture `--jit`
is an error rather than a silent fallback, since a silent fallback reads as "the JIT made
no difference" instead of "this platform has no JIT". The JIT tests are skipped on those
hosts for the same reason: the encoder test calls the bytes it emits, and the differential
tests would fail on the flag rather than on anything the programs do.

Correctness rests on differential testing: fourteen programs are run interpreted and then
compiled, and the two outputs must match byte for byte (`tests/compare_jit.cmake`). A
fifteenth test covers the encoder on its own, without the VM.

Two properties of those tests are worth spelling out, because both were wrong at first and
both made the suite look stronger than it was:

- **Agreeing is not passing.** A program whose own checks all fail exits non-zero in *both*
  modes, so the two runs agree perfectly and a comparison-only test passes. The harness now
  also requires the interpreted run to exit zero, which is what lets a failing assertion
  fail the test. `EXPECT_RC` overrides it for a program meant to fail.
- **A minimum compiled count.** A program where every function is refused still agrees with
  the interpreter, so the tests written to exercise compiled code assert a floor on how many
  functions were compiled. `jit_mixed_call_check.ax` pins the exact number, since one of its
  functions exists only to be refused.

## Can it do anything real?

Yes — the standard library and the extra modules are written in Axiom and tested:

| Module | Contents |
|---|---|
| `stdlib/` | `algorithm` (73 functions), `map` (57), `hash` (52), `sync` (39), `time` (34), `str` (26), `set` (25), `vec` (22), plus `astl`, `random`, `test` |
| `extralib/` | `json`, `xml`, `yaml`, `toml`, `ini`, `csv`, `http`, `net`, `inflate`, `sha256`, `glob`, `cli`, `value` |

`extralib/xml.ax` is an XML parser in pure Axiom. `extralib/inflate.ax` is a DEFLATE
decoder. `extralib/http.ax` is an HTTP/1.1 client with chunked decoding. Each declares its
identity and version:

```axiom
module "json" version "0.1.0";
```

## Tests

```bash
ctest --test-dir build --output-on-failure
```

75 tests. Three kinds matter:

- **`*_check.ax` programs** that assert and report pass/fail, so a failure names the
  behaviour rather than a byte offset.
- **Differential tests** that run each script at `-O0`, `-O3` and `-O4` and require
  byte-identical output. Single-level tests cannot catch an optimizer that computes the
  wrong answer only after folding — the run at `-O0` succeeds and hides it.
- **Diagnostic tests** that require the compiler to *reject* a program with a specific
  message, because a silently accepted program that should have been an error is the
  failure mode that costs the most.

`tools/verify_tutorial.py` extracts every complete program from `TUTORIAL.md`, runs it,
and compares the output against what the document claims.

## Repository layout

```
src/          Lexer, Parser, Sema, IR, IRGen, Optimize, VM, Driver
include/      Headers, mirroring src/
stdlib/       Standard library (.ax), native declarations (.axd), C sources (.c)
extralib/     Modules built on the standard library
tests/        Script tests, C++ unit tests, differential harnesses, fixtures
examples/     Runnable programs
tools/        verify_tutorial.py and the acvm wrapper
editors/      A TextMate grammar for syntax highlighting, and sample files
brand/        The logo as SVG, for anyone who needs it at another size
.gitattributes  Line endings, and telling GitHub that .ax/.axd are Axiom
```

## Status and known limitations

The implementation follows the Axiom language specification, with a deliberate and
documented list of deviations.
The ones most likely to be noticed:

- `vector T` is not implemented; `vec.ax` provides an equivalent container instead.
- `intN` is signed, so `int127` tops out at 2^126−1. `int128` is supported as an
  extension so the specification's own example is valid. `unsigned int128` is refused:
  its maximum does not fit the VM's signed 128-bit cell.
- `doubleA.B` is accepted syntactically but stored as binary64.
- Aggregate `<` is rejected. `==` / `!=` compare storage bytes.
- The JIT (`--jit`) compiles a subset. String literals, globals and aggregates are not
  lowered, so a function containing any of them stays interpreted in full — including the
  arithmetic around it. The interpreter is always the fallback and always the definition
  of correct behaviour.

Not implemented at all: tagged variants (`union` + automatic tag + `match`), operator
overloading, and constraints on generic type parameters (`T: Comparable`).

## License

MIT. See [`LICENSE`](LICENSE).
