# EigenScript for VS Code

Syntax highlighting, bracket matching, comment toggling, and
indentation rules for `.eigs` files.

## Install (from this repo)

```bash
# Symlink (or copy) into your VS Code extensions directory:
ln -s "$(pwd)/editors/vscode" ~/.vscode/extensions/inauguralsystems.eigenscript-0.13.0
# then reload VS Code
```

Or package a `.vsix` with [vsce](https://github.com/microsoft/vscode-vsce):

```bash
cd editors/vscode
npx @vscode/vsce package
code --install-extension eigenscript-0.13.0.vsix
```

## Language server

The repo also ships an LSP server (`make lsp` builds `src/eigenlsp`)
with diagnostics, completion, hover, go-to-definition, and references.
Any LSP client can use it over stdio; a bundled VS Code client is
future work — for now, generic LSP bridge extensions work:

```jsonc
// example: with an LSP-bridge extension, point it at:
{ "command": ["/path/to/EigenScript/src/eigenlsp"], "languageId": "eigenscript" }
```

## What gets highlighted

- Keywords (`define`, `is`, `of`, control flow), interrogatives
  (`what`/`who`/`when`/`prev`/...), observer predicates
  (`converged`, `stable`, ...), f-strings with `{interpolation}`,
  numbers, comments, function definitions and call sites, operators
  (including `|>` and `=>`).
