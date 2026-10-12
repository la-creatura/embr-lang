# tree-sitter grammar for embr

`grammar.js` describes embr for editors that use tree-sitter (Neovim, Helix, Zed). it follows `include/embr/core/parser.h`: no
statement separator (newlines mean nothing), blocks close with `end`, `elif` and `else` belong to one `if`, `name(x) = expr` is
a function, `a, b = f()` unpacks.

```bash
npm install                 # tree-sitter-cli
npx tree-sitter generate    # makes src/parser.c (not kept in the repo)
npx tree-sitter parse ../../../examples/text.embr
```

`queries/highlights.scm` colours it. `check.sh` builds the grammar in a scratch folder and parses every `.embr` file in the repo, so
a change to the language that the grammar misses shows up as a syntax error there.

what it does not do: it does not check types or names (`embr check` does), and it accepts a few things the real parser rejects
(`break` outside a loop, a `const fn` with a try block).
