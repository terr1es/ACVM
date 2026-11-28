#!/bin/sh
# Runs the Axiom/CPython comparison. Thin wrapper around compare.py, which is where the
# measurement lives -- it subtracts process startup on both sides and runs CPython isolated.
#
#     ./docs/bench/run.sh [path/to/acvm]
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
exec python "$HERE/compare.py" "${1:-${ACVM:-acvm}}"
