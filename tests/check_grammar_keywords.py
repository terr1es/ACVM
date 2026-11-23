#!/usr/bin/env python3
"""Fail when the TextMate grammar does not know every keyword the lexer accepts.

Why this exists: a syntax definition is a derived artifact. When the lexer gains a
keyword, nothing about the grammar breaks -- it just stops highlighting it, silently.
That had already happened twice before anyone noticed: `union` and `module` were both
accepted by the compiler and unhighlighted in the grammar, and `module` was added the
same evening and was stale within the hour.

A grammar is also easy to check mechanically, which is exactly why leaving it unchecked
was a poor trade: the check is a set comparison.

The second half of the check is the grammar's `scopeName`, which must equal the
`tm_scope` in the language definition that will be submitted to github/linguist. That
pairing is the only link between the language and its grammar: if they disagree the
language is registered with no highlighting at all, which looks worse than being
unregistered.

The keyword list is read from the lexer's own table rather than from a copy, because a
copy is the thing that drifts. Keywords that are deliberately not highlighted as
`keyword.*` (they live in `constant.language` or `support.function`) are still expected
to appear somewhere in the grammar.

Usage:

    python tests/check_grammar_keywords.py
    python tests/check_grammar_keywords.py --grammar path --lexer path

A non-zero exit code means the grammar is behind the language.
"""

import argparse
import json
import os
import re
import sys

# Keywords the lexer spells differently from the grammar's `match` text on purpose.
# Nothing here yet; the list exists so an exception has an obvious home and does not
# get expressed as a looser comparison.
ALIASES = {}


def lexer_keywords(path):
    """Every keyword string in the lexer's keyword table, in source order."""
    source = open(path, encoding="utf-8").read()
    return re.findall(r'\{"([a-z_]+)",\s*TokenKind::Kw', source)


def grammar_patterns(path):
    """All `match` regular expressions in the grammar, concatenated."""
    data = json.load(open(path, encoding="utf-8"))
    matches = []

    def walk(node):
        if isinstance(node, dict):
            for key, value in node.items():
                if key == "match" and isinstance(value, str):
                    matches.append(value)
                else:
                    walk(value)
        elif isinstance(node, list):
            for item in node:
                walk(item)

    walk(data)
    return data, matches


def check_scope(grammar_path, language_path):
    """The grammar's scopeName must equal the language definition's tm_scope.

    Returns a list of problems. An absent language definition is not a failure: the
    grammar is useful on its own, and the definition is what a submission to
    github/linguist is built from.
    """
    if not os.path.exists(language_path):
        return []

    data = json.load(open(grammar_path, encoding="utf-8"))
    scope = data.get("scopeName")

    # Deliberately not a YAML parser: the only field needed is a single scalar, and adding
    # a dependency for it would make this check refuse to run where PyYAML is absent.
    text = open(language_path, encoding="utf-8").read()
    match = re.search(r"^[ \t]*tm_scope:[ \t]*(\S+)[ \t]*$", text, re.MULTILINE)
    if match is None:
        return [f"{os.path.basename(language_path)}: no tm_scope entry"]

    declared = match.group(1).strip("'\"")
    if declared != scope:
        return [
            f"tm_scope is '{declared}' but the grammar's scopeName is '{scope}'",
            "these must match, or the language is registered with no highlighting",
        ]
    return []


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.dirname(here)

    parser = argparse.ArgumentParser()
    parser.add_argument("--lexer", default=os.path.join(root, "src", "Lex", "Lexer.cpp"))
    parser.add_argument(
        "--grammar", default=os.path.join(root, "editors", "textmate", "axiom.tmLanguage.json")
    )
    args = parser.parse_args()

    if not os.path.exists(args.lexer):
        print(f"lexer not found: {args.lexer}", file=sys.stderr)
        return 2
    if not os.path.exists(args.grammar):
        print(f"grammar not found: {args.grammar}", file=sys.stderr)
        return 2

    keywords = lexer_keywords(args.lexer)
    if not keywords:
        # A pattern change in the lexer would make this silently pass on an empty list,
        # which is worse than failing.
        print("no keywords found in the lexer -- has the table's shape changed?", file=sys.stderr)
        return 2

    data, matches = grammar_patterns(args.grammar)

    # Split each rule's regex into its identifier-like tokens and compare as sets.
    #
    # Searching the raw pattern text for the keyword would be wrong in a way that is easy
    # to miss: the grammar stores `\\blibrary\\b`, so a word-boundary search for
    # `library` fails against the literal `b` sitting right next to it. Splitting on
    # non-identifier characters takes `\\blibrary\\b` to {b, library} and asks the only
    # question that matters -- is this word named anywhere in a rule.
    vocabulary = set()
    for pattern in matches:
        # Strip the zero-width assertions first. `\\blibrary\\b` is one token after a
        # naive split -- the `b` of `\\b` sits flush against `library` and fuses into
        # `blibrary` -- so the boundary marker has to go before splitting on
        # non-identifier characters.
        stripped = pattern.replace("\\b", " ")
        vocabulary.update(
            token for token in re.split(r"[^A-Za-z0-9_]+", stripped) if token
        )

    missing = []
    for keyword in keywords:
        needle = ALIASES.get(keyword, keyword)
        if needle not in vocabulary:
            missing.append(keyword)

    if missing:
        print(f"grammar is behind the language: {len(missing)} keyword(s) not highlighted")
        for keyword in missing:
            print(f"  {keyword}")
        print(
            "\nAdd each to a rule in the grammar's `repository`. Group them by role:\n"
            "  declarations -> #typedecl / #keywords\n"
            "  control flow -> #keywords (keyword.control)\n"
            "  literals     -> #constants (constant.language)"
        )
        return 1

    scope_problems = check_scope(
        args.grammar, os.path.join(root, "editors", "linguist", "axiom.yml")
    )
    if scope_problems:
        print("the language definition and the grammar disagree:", file=sys.stderr)
        for problem in scope_problems:
            print(f"  {problem}", file=sys.stderr)
        return 1

    print(
        f"{os.path.relpath(args.grammar, root)}: all {len(keywords)} lexer keywords are highlighted"
        f" ({len(matches)} match rules, scope {data.get('scopeName')})"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
