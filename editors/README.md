# Editor support

Axiom ships one derived artifact for editors: a **TextMate grammar**. It is a plain
[JSON grammar](https://macromates.com/manual/en/language_grammars) with no dependency on
any particular editor, so it can be consumed by any host that reads the format, and it can
be translated to another form by a tool.

```
editors/
  textmate/axiom.tmLanguage.json   the grammar (scope source.axiom)
  linguist/axiom.yml               the language definition for GitHub's detection
  samples/selftest.ax              one token class per section, for checking a colour scheme
  samples/decl.axd                 a declaration file, which highlights differently
```

## Using it

Point your editor's syntax support at `textmate/axiom.tmLanguage.json` and associate it
with the `.ax` extension. `samples/selftest.ax` covers every token class in one file,
so a colour scheme can be checked by eye in a single view.

## `.axd` files

Declaration files use the same grammar, because they are the same language. They carry two
extra requirements, neither of which is syntax: the file must begin with
`library "name";`, and module resolution prefers `name.axd` over `name.ax`.

## On GitHub

`.gitattributes` maps `.ax` and `.axd` to the language `Axiom`, so the repository page's
language bar reports it instead of classifying the files as "Other".

That mapping does not, on its own, produce colours in GitHub's code view. The highlighter
uses whatever grammar GitHub has associated with the language, and a language absent from
its database has none. `linguist/axiom.yml` is the definition to submit to
[github/linguist](https://github.com/github-linguist/linguist) to close that gap; its
comments explain each field and what to expect before and after the merge.

The one field that must stay in step is `tm_scope`, which has to equal the grammar's
`scopeName`. The check below enforces that pair, because a mismatch registers the language
with no highlighting at all — which looks worse than not being registered.

## Keeping it honest

The grammar is a *derived* artifact: when the lexer gains a keyword, nothing about the
grammar breaks. It simply stops highlighting that word, silently. That had already
happened twice — `union` and `module` were both accepted by the compiler and unhighlighted
here, and `module` was stale within an hour of being added.

`tests/check_grammar_keywords.py` closes that gap. It reads the keyword table out of
`src/Lex/Lexer.cpp` and requires every entry to be named in some rule of the grammar:

```
python tests/check_grammar_keywords.py
```

It runs as part of `ctest`, so a keyword added to the lexer without a matching grammar
rule fails the build's test step rather than going unnoticed.

The list is read from the lexer rather than kept as a copy here, because a copy is the
thing that drifts.
