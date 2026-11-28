#!/usr/bin/env python3
"""Compares Axiom against CPython on the benchmarks beside this file.

    python docs/bench/compare.py [path/to/acvm]

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
"""
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
    ("array", "b_array.ax", "p_array.py"),
    ("calls", "b_calls.ax", "p_calls.py"),
    ("quicksort", None, "qs.py"),  # None: use the real example, not a copy
]


def main() -> int:
    acvm = sys.argv[1] if len(sys.argv) > 1 else "acvm"
    python = [sys.executable, "-I"]
    runs = 11

    example = HERE.parent.parent / "examples" / "quicksort_max.ax"

    # Startup baselines: a program that does nothing, on both sides.
    (HERE / "_tiny.ax").write_text('function main():void { print(1); eof: pass; }\n')
    (HERE / "_tiny.py").write_text("print(1)\n")
    axiom_base = best_ms([acvm, "run", str(HERE / "_tiny.ax")], runs)
    python_base = best_ms(python + [str(HERE / "_tiny.py")], runs)
    print(f"startup: Axiom {axiom_base:.1f} ms (compile included), "
          f"CPython {python_base:.1f} ms\n")

    header = f"{'benchmark':12}{'interp':>10}{'--jit':>10}{'CPython':>10}{'jit vs CPython':>16}"
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
        print(f"{name:12}{interp:>9.1f}ms{jit:>9.1f}ms{cpy:>9.1f}ms{verdict:>16}")

    (HERE / "_tiny.ax").unlink()
    (HERE / "_tiny.py").unlink()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
