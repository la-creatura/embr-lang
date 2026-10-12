# embr language support (VS Code / TextMate)

`syntaxes/embr.tmLanguage.json` is a TextMate grammar in json form. vs code reads it directly.
other tools that take textmate grammars may want it too (GitHub's linguist, JetBrains' TextMate bundle support),
but sublime text does not read the json form, it has its own file in `../sublime/embr.sublime-syntax`

17 reserved words, `#` line comments and non-nesting `#[ ... ]#` block comments,
double-quoted strings whose only valid escapes are `\n \t \r \0 \" \\` 
anything else is highlighted as illegal, matching the lexer error,
ints and floats,the `->` return arrow, `...`, compound assignment,
and type annotations (`x: int`, `-> num`, `local int x`).
`true`/`false`/`nil` are not keywords in embr but are coloured as constants anyway.

## try it (VS Code)

```bash
# from the repo root, symlink the folder into your extensions directory, then reload the window
ln -s "$PWD/tools/editor/vscode" ~/.vscode/extensions/embr-language
```

or `npx @vscode/vsce package` in this directory and install the resulting `.vsix`.

## language server

the extension starts `embr lsp` for `.embr` files, so you get errors, go-to-definition, references, rename, hover (with the doc line of
plugin and module functions), completion and the outline. it needs the `embr` program: put it on your PATH or set `embr.path` in the
settings. before the first use run `npm install` in this folder (it installs `vscode-languageclient`), and `npx @vscode/vsce package`
makes a `.vsix`. the command "embr: restart the language server" starts it again after you rebuild embr.

any other editor with a generic lsp client can run `embr lsp` the same way (see the wiki's syntax page).

## test

`tools/editor/check_grammar.py` loads the grammar, checks every pattern compiles,
and tokenizes every `.embr` file in the repo plus the keyword list from the reference to make sure nothing drifts
(it is deliberately a regex smoke test, not a full TextMate engine)
