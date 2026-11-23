"""Feed Axiom sources through the TextMate grammar and report unscoped identifiers.

A position-aware mini scanner: at each offset it tries every single-line `match`
rule in document order (first match wins, like TextMate), splits `captures`, and
skips over comments and string bodies so their contents are not re-scanned.
"""
import io
import json
import os
import re
import sys
from collections import Counter

HERE = os.path.dirname(os.path.abspath(__file__))
GRAMMAR = os.path.join(HERE, "syntaxes", "axiom.tmLanguage.json")


def flatten(patterns, repo, depth=0):
    out = []
    if depth > 6:
        return out
    for item in patterns:
        if "match" in item:
            out.append(item)
        if "include" in item:
            key = item["include"].lstrip("#")
            if key in repo:
                out.extend(flatten(repo[key]["patterns"], repo, depth + 1))
    return out


def scan_line(line, rules):
    tokens = []
    i, n = 0, len(line)
    while i < n:
        if line.startswith("//", i):
            break
        if line.startswith("/*", i):
            end = line.find("*/", i + 2)
            i = n if end < 0 else end + 2
            continue
        if line[i] in "\"'":
            quote = line[i]
            j = i + 1
            while j < n:
                if line[j] == "\\":
                    j += 2
                    continue
                if line[j] == quote:
                    j += 1
                    break
                j += 1
            i = j
            continue

        hit = None
        for rule in rules:
            m = re.compile(rule["match"]).match(line, i)
            if m and m.group(0):
                hit = (rule, m)
                break

        if hit is None:
            m = re.compile(r"[A-Za-z_][A-Za-z0-9_]*").match(line, i)
            if m:
                tokens.append((m.group(0), None))
                i += len(m.group(0))
            else:
                i += 1
            continue

        rule, m = hit
        caps = rule.get("captures") or {}
        if caps:
            for key in sorted(caps, key=int):
                idx = int(key)
                if idx > 0 and m.group(idx):
                    tokens.append((m.group(idx), caps[key].get("name")))
        else:
            tokens.append((m.group(0), rule.get("name")))
        i += len(m.group(0))
    return tokens


def main(paths):
    grammar = json.load(io.open(GRAMMAR, encoding="utf-8"))
    rules = flatten(grammar["patterns"], grammar["repository"])

    for path in paths:
        unscoped = Counter()
        scoped = Counter()
        for line in io.open(path, encoding="utf-8", errors="replace").read().split("\n"):
            for text, scope in scan_line(line, rules):
                if not re.match(r"^[A-Za-z_]", text):
                    continue
                if scope is None:
                    unscoped[text] += 1
                else:
                    scoped[scope] += 1

        print("== %s ==" % os.path.basename(path))
        print("   scoped: %s" % dict(scoped.most_common()))
        if unscoped:
            print("   unscoped: %s" % dict(unscoped.most_common(12)))
        else:
            print("   unscoped: none")
        print()


if __name__ == "__main__":
    if sys.argv[1:]:
        # Relative paths are relative to the current directory, not to this file,
        # so that `check_grammar.py examples/hello.ax` behaves like any CLI tool.
        targets = [os.path.normpath(t) for t in sys.argv[1:]]
    else:
        targets = [os.path.normpath(os.path.join(HERE, "..", "stdlib", "plugin", "ext.axd"))]
    main(targets)
