# Axiom for VS Code

Syntax highlighting for the Axiom language (`.ax` sources and `.axd` declaration files).
Pure TextMate grammar; there is no language server.

Both extensions share one language id, `axiom`, because they are one grammar. `.axd` only
carries extra requirements: the file must begin with `library "name";`, and module
resolution prefers it over a same-named `.ax`.

## File icon

`.ax` and `.axd` files show the Axiom logo in the explorer, switching with the theme:

| Theme | Icon | Ink |
| --- | --- | --- |
| Light | `icons/axiom-light.png` | near-black `#0C0C0E` |
| Dark | `icons/axiom-dark.png` | white `#FFFFFF` |

64×64 PNG, transparent background, rendered from the same analytic geometry rather than
scaled — so the edges stay sharp.

> **Note:** `contributes.languages[].icon` is the *fallback* icon for a file icon theme.
> If the active theme already provides an icon for `.ax`, the theme wins; this one is used
> only when the theme has none. Some themes disable the fallback entirely with
> `"showLanguageModeIcons": false`.

## What is covered

| Category | Included |
| --- | --- |
| Comments | `//`, `/* */`, with `TODO` / `FIXME` markers highlighted |
| Literals | integers with every width suffix, floats, strings with escapes, `char`, `true` / `false`, `null` |
| Types | `int`, `intN`, `unsigned intN`, `float` / `double`, `char`, `bool`, `string`, `void`, `ptr` |
| Declarations | `function`, `struct`, `union`, `enum`, `global`, `const`, `comptime`, `module`, `library` |
| Control flow | `if` / `else`, `while` / `for` / `loop`, `switch` / `case` / `default`, `break` / `continue` |
| Expressions | operators, `as`, `#` address-of, `@` dereference, `sizeof` / `alignof` |
| Concurrency | `process`, `event`, `yield`, `parallel` |
| Special | `eof` and the `import` forms |

## Samples

`samples/selftest.ax` exercises one token class per section. Open it to check a colour
scheme by eye.

## Development

`check_grammar.py` runs the grammar over the sample files and reports which tokens were
left unscoped — an unscoped token is a highlighting gap, and the report says which one.
