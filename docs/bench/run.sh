#!/bin/sh
# Runs the dispatch benchmarks and prints Axiom beside CPython.
#
#     ./docs/bench/run.sh [path/to/acvm]
#
# Each number is the minimum of five runs, with the two languages alternating so that drift
# in machine load cancels. Wall clock on this machine moves by a few percent between
# sessions; instruction counts (from `acvm run x.ax --stats`) do not, and are the number to
# compare when judging a change to the instruction stream.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
ACVM="${1:-${ACVM:-acvm}}"
PYTHON="${PYTHON:-python}"

printf '%-10s %10s %10s %8s\n' bench axiom cpython ratio
for bench in empty arith calls array; do
    a=$(python - "$ACVM" "$HERE/b_$bench.ax" <<'PY'
import subprocess, sys, time
cmd, path = sys.argv[1], sys.argv[2]
best = min((lambda t0: (subprocess.run(cmd.split() + ["run", path], capture_output=True), time.perf_counter() - t0)[1])(time.perf_counter()) for _ in range(5))
print(f"{best:.3f}")
PY
)
    p=$(python - "$PYTHON" "$HERE/p_$bench.py" <<'PY'
import subprocess, sys, time
cmd, path = sys.argv[1], sys.argv[2]
best = 1e9
for _ in range(5):
    t0 = time.perf_counter()
    subprocess.run(cmd.split() + [path], capture_output=True)
    best = min(best, time.perf_counter() - t0)
print(f"{best:.3f}")
PY
)
    printf '%-10s %10s %10s %7sx\n' "$bench" "$a" "$p" \
        "$(python -c "print(f'{$a/$p:.2f}')")"
done
