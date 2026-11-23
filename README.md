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
| 50 000 | 109.6 ms | 33.5 ms | 2.83 ms | 1.90 ms |
| 200 000 | 482 ms | 156 ms | 10.84 ms | 7.47 ms |
| 1 000 000 | 2658 ms | 1082 ms | 53.1 ms | 37.0 ms |

- 39–50× slower than unoptimized C++, 58–73× slower than `-O2`
- 2.5–3.3× slower than CPython 3.13

The CPython comparison is the interesting one. Counting with `sys.settrace`, CPython
executes **179 bytecodes per element** and ACVM executes **175 instructions** — the same
amount of work. The difference is the cost of a single dispatch: 3.3 ns for CPython,
12.5 ns for ACVM. That 3.8× is the 32-byte `Value` copy, memory access through a page
table, and a `MemType` lookup on every instruction.

In other words: the algorithm and the instruction count are already at the floor. What
remains is the cost of *implementing* an instruction. Closing that gap means changing the
representation of `Value` and the memory model, not adding more fused instructions.

These numbers were measured on one machine (llvm-mingw, Windows x86-64) and are reported
as ratios rather than absolute times for that reason.

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
vscode-axiom/ Editor support: syntax highlighting and a TextMate grammar
brand/        Icon sources and the generator that produces the .ico files
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

Not implemented at all: tagged variants (`union` + automatic tag + `match`), operator
overloading, and constraints on generic type parameters (`T: Comparable`).

## License

MIT. See [`LICENSE`](LICENSE).
