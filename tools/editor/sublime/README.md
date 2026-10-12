# embr syntax highlighting

`embr.sublime-syntax` is the sublime version of the vs code grammar in `../vscode/syntaxes/`.
it covers the same things (see `../vscode/README.md`). sublime does not read `.tmLanguage.json`,
so this file is kept by hand next to it.
to use it, copy or symlink the file into your sublime user folder, then reopen an `.embr` file:

```bash
# linux. on macos it is ~/Library/Application Support/Sublime Text/Packages/User
mkdir -p ~/.config/sublime-text/Packages/User
ln -s "$PWD/tools/editor/sublime/embr.sublime-syntax" ~/.config/sublime-text/Packages/User/
```

# embr lsp

the cli tool provides the command `embr lsp` to run the embr language server.
to use it in sublime, build or download the cli, install the LSP package, `Preferences > Package Settings > LSP > Settings` and put
```json
{
  "clients": {
    "embr": {
      "enabled": true,
      "command": ["/path/to/your/embr", "lsp"],
      "selector": "source.embr"
    }
  }
}
```