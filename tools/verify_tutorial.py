#!/usr/bin/env python3
"""Extract every complete program from TUTORIAL.md and actually run it.

Why this exists: the examples in a tutorial are the first thing to rot. When the
language changes, the document does not fail to compile -- it just becomes quietly
wrong, and readers copy it. This turns "the programs in the document work" into a
check that runs, instead of a manual confirmation that happens once.

Rules:

  * Only blocks containing `function main(` are run. The rest are fragments -- type
    demonstrations, counter-examples -- that cannot be compiled on their own.
  * When a block is followed by a fenced block whose first line is `── output ──`,
    the program's output is compared line by line (trailing spaces ignored).
  * Without that marker, only a zero exit code is required.

Usage:

    python tools/verify_tutorial.py                 # uses ./build/bin/acvm
    python tools/verify_tutorial.py --acvm path     # explicit compiler
    python tools/verify_tutorial.py --doc OTHER.md  # check another document

A non-zero exit code means something failed, so it can go straight into CI.
"""

import argparse
import os
import re
import subprocess
import sys
import tempfile

OUTPUT_MARKER = "── output ──"


def extract_blocks(lines):
    """Returns [(line number, code, expected output or None)]."""
    blocks = []
    i = 0
    while i < len(lines):
        if lines[i].strip() != "```axiom":
            i += 1
            continue

        start = i + 1
        end = start
        while end < len(lines) and lines[end].strip() != "```":
            end += 1
        code = "\n".join(lines[start:end]) + "\n"

        # The expected output is a separate fenced block whose first line is
        # `── output ──`:
        #
        #     ```
        #     ── output ──
        #     hello, axiom
        #     ```
        #
        # So the fence comes first and the marker sits inside it. Looking for the
        # marker first silently skips every comparison.
        expected = None
        # A blank line usually separates the code block from the output block.
        after = end + 1
        while after < len(lines) and lines[after].strip() == "":
            after += 1
        if after < len(lines) and lines[after].strip() == "```":
            first = after + 1
            if first < len(lines) and OUTPUT_MARKER in lines[first]:
                close = first + 1
                while close < len(lines) and lines[close].strip() != "```":
                    close += 1
                # Drop the marker line itself; the rest is the program output.
                expected = "\n".join(lines[first + 1:close])
                end = close

        blocks.append((start, code, expected))
        i = end + 1
    return blocks


def normalize(text):
    """Strip trailing spaces per line and drop trailing blank lines.

    Output comparison should not be affected by trailing whitespace.
    """
    lines = [line.rstrip() for line in text.replace("\r\n", "\n").split("\n")]
    while lines and lines[-1] == "":
        lines.pop()
    return lines


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--acvm", default="./build/bin/acvm")
    parser.add_argument("--doc", default="TUTORIAL.md")
    parser.add_argument("--keep", action="store_true", help="keep the temp directory (debugging)")
    args = parser.parse_args()

    # Windows binaries end in .exe, while documentation writes the path without it.
    executable = args.acvm
    if not os.path.exists(executable) and os.path.exists(executable + ".exe"):
        executable = executable + ".exe"
    if not os.path.exists(executable):
        print(f"compiler not found: {args.acvm}", file=sys.stderr)
        return 2

    with open(args.doc, encoding="utf-8") as handle:
        lines = handle.read().splitlines()

    blocks = extract_blocks(lines)
    runnable = [b for b in blocks if "function main(" in b[1]]
    print(f"{args.doc}: {len(blocks)} axiom blocks, {len(runnable)} complete programs")

    work = tempfile.mkdtemp(prefix="axiom-tutorial-")
    failures = 0
    with_output = 0

    for index, (line_no, code, expected) in enumerate(runnable):
        path = os.path.join(work, f"example{index:02d}.ax")
        with open(path, "w", encoding="utf-8") as handle:
            handle.write(code)

        result = subprocess.run(
            [executable, "run", path],
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
        )
        actual = (result.stdout or "") + (result.stderr or "")

        problem = None
        if result.returncode != 0:
            problem = f"exit code {result.returncode}"
        elif expected is not None:
            with_output += 1
            if normalize(actual) != normalize(expected):
                problem = "output does not match the document"

        if problem:
            failures += 1
            print(f"\nFAIL  program at line {line_no} -- {problem}")
            print(f"  file: {path}")
            if expected is not None:
                print("  expected:")
                print("    " + "\n    ".join(normalize(expected)))
            print("  actual:")
            print("    " + "\n    ".join(normalize(actual)))

    print(f"\nresult: {len(runnable) - failures}/{len(runnable)} passed"
          f" ({with_output} compared their output)")

    if args.keep:
        print(f"temp directory: {work}")
    else:
        for name in os.listdir(work):
            os.remove(os.path.join(work, name))
        os.rmdir(work)

    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
