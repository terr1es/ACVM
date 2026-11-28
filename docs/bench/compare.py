#!/usr/bin/env python3
"""Compares Axiom against CPython and C++ on the benchmarks beside this file.

    python docs/bench/compare.py [path/to/acvm] [--build]

`--build` compiles the C++ quicksort and an empty program for its startup baseline. Without
it the C++ columns are omitted rather than filled from a previous run.

Two things this does that a plain `time` does not, both of which changed the
answer when they were added.

**It subtracts process startup.** A run of a 40 ms program is mostly the
process. Both sides are timed on a program that does nothing and the difference
is removed, so the ratio describes the work rather than the loader.

**It runs CPython isolated (`-I`).** Without that flag a `sitecustomize` on this
machine imports `platform` and costs 320 ms before the first line of the
program runs -- which made an earlier version of this comparison report Axiom
as 8.8x ahead on quicksort when the honest figure is 1.9x. `-I` also skips the
user site directory, so the numbers describe the interpreter rather than
whatever is installed beside it. CPython's own startup is still counted, since
a program cannot run without it.

**C++ is compiled beside this file, not quoted from elsewhere.** `qs.cpp` is
the same algorithm as `examples/quicksort_max.ax` -- same PRNG, same threshold,
same partition scheme, same recursion order -- and prints the same inversion
counts, which is what makes the two times comparable. `./compare.py --build`
compiles it; without the flag the column is omitted rather than filled from
memory. Numbers from a different machine are not comparable and are not put in
the same table.

Only one size is measured: 50 000 elements. Axiom has no command-line argument
mechanism, so `quicksort_max.ax` is fixed at its tuned size, and a multi-size
table would mean three engines at three sizes measured three different ways.
"""
import os
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent


def best_ms(cmd, runs):
    """Fastest of `runs` wall-clock samples, in milliseconds."""
    fastest = float("inf")
    for _ in range(runs):
        start = time.perf_counter()
        result = subprocess.run(cmd, capture_output=True)
        elapsed = time.perf_counter() - start
        if result.returncode != 0 or b"runtime error" in result.stderr:
            raise SystemExit(f"failed: {' '.join(map(str, cmd))}\n{result.stderr.decode()}")
        fastest = min(fastest, elapsed)
    return fastest * 1000.0


BENCHMARKS = [
    # name, Axiom source, CPython source
    ("empty", "b_empty.ax", "p_empty.py"),
    ("arith", "b_arith.ax", "p_arith.py"),
    ("branch", "b_branch.ax", "p_branch.py"),
    ("array", "b_array.ax", "p_array.py"),
    ("strings", "b_strings.ax", "p_strings.py"),
    ("hashmap", "b_hashmap.ax", "p_hashmap.py"),
    ("double", "b_double.ax", "p_double.py"),
    ("calls", "b_calls.ax", "p_calls.py"),
    ("quicksort", None, "qs.py"),  # None: use the real example, not a copy
]


def native_startup_ms():
    """Process-creation cost for a native binary.

    Measured from an empty program, not from the benchmark binary: subtracting a binary from
    itself would remove the run rather than the loader. 16 ms on this machine, which is most of
    a 20 ms C++ quicksort -- quoting the raw wall clock would have made C++ look four times
    slower than it is.
    """
    empty = HERE / "_empty.exe"
    if not empty.exists():
        return 0.0
    return best_ms([str(empty)], 21)


def build_cxx() -> bool:
    """Compiles the C++ side. Returns False when there is no compiler to compile with."""
    compiler = os.environ.get("CXX", "clang++")
    sources = [("qs.exe", "-O2"), ("qs0.exe", "-O0")]
    try:
        for output, level in sources:
            result = subprocess.run(
                [compiler, level, "-std=c++20", str(HERE / "qs.cpp"), "-o", str(HERE / output)],
                capture_output=True)
            if result.returncode != 0:
                print(f"could not build {output}: {result.stderr.decode()[:200]}")
                return False
        (HERE / "_empty.cpp").write_text("int main(){return 0;}\n")
        result = subprocess.run(
            [compiler, "-O2", "-std=c++20", str(HERE / "_empty.cpp"),
             "-o", str(HERE / "_empty.exe")],
            capture_output=True)
        if result.returncode != 0:
            print("could not build the startup baseline")
            return False
    except FileNotFoundError:
        print(f"no C++ compiler named {compiler!r} (set CXX to change that)")
        return False
    return True


def main() -> int:
    arguments = [a for a in sys.argv[1:] if not a.startswith("--")]
    if "--build" in sys.argv[1:]:
        if not build_cxx():
            print("continuing without the C++ columns\n")
    acvm = arguments[0] if arguments else "acvm"
    python = [sys.executable, "-I"]
    runs = 11

    example = HERE.parent.parent / "examples" / "quicksort_max.ax"

    # C++ is optional: it needs a compiler, and a comparison that silently quotes a number
    # from somewhere else is worse than one that omits the column.
    cxx = {}
    if (HERE / "qs.exe").exists():
        cxx["-O2"] = [str(HERE / "qs.exe")]
    if (HERE / "qs0.exe").exists():
        cxx["-O0"] = [str(HERE / "qs0.exe")]

    # Startup baselines: a program that does nothing, on both sides.
    (HERE / "_tiny.ax").write_text('function main():void { print(1); eof: pass; }\n')
    (HERE / "_tiny.py").write_text("print(1)\n")
    axiom_base = best_ms([acvm, "run", str(HERE / "_tiny.ax")], runs)
    python_base = best_ms(python + [str(HERE / "_tiny.py")], runs)
    native_base = native_startup_ms() if cxx else 0.0
    print(f"startup: Axiom {axiom_base:.1f} ms (compile included), "
          f"CPython {python_base:.1f} ms\n")

    header = f"{'benchmark':12}{'interp':>10}{'--jit':>10}{'CPython':>10}{'jit vs CPython':>16}"
    if cxx:
        header += "".join(f"{'C++ ' + k:>11}" for k in cxx)
    print(header)
    print("-" * len(header))
    for name, axiom_file, py_file in BENCHMARKS:
        source = str(example) if axiom_file is None else str(HERE / axiom_file)
        interp = max(best_ms([acvm, "run", source], runs) - axiom_base, 0.001)
        jit = max(best_ms([acvm, "run", source, "--jit"], runs) - axiom_base, 0.001)
        cpy = max(best_ms(python + [str(HERE / py_file)], runs) - python_base, 0.001)
        if jit < cpy:
            verdict = f"{cpy / jit:.2f}x faster"
        else:
            verdict = f"{jit / cpy:.2f}x slower"
        row = f"{name:12}{interp:>9.1f}ms{jit:>9.1f}ms{cpy:>9.1f}ms{verdict:>16}"
        if cxx:
            for label, command in cxx.items():
                if name != "quicksort":
                    row += f"{'-':>11}"
                else:
                    row += f"{max(best_ms(command, runs) - native_base, 0.001):>10.1f}ms"
        print(row)

    (HERE / "_tiny.ax").unlink()
    (HERE / "_tiny.py").unlink()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
