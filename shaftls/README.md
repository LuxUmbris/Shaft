# shaftls

`shaftls` is the dependency-free Shaft language server. It uses JSON-RPC 2.0 over stdio with `Content-Length` framing. Standard output is reserved for protocol frames; it emits no logs there.

## Build and run

From the repository root:

```sh
./build-linux-x86_64-release/shaftc --build shaftls/Shaft.build
./shaftls/build/shaftls
```

The server currently provides:

- incremental document synchronization (`didOpen`, full and ranged `didChange`)
- semantic tokens for `import` and its quoted path, plus `@config`, `@asm`, and `@end`
- structural `publishDiagnostics` messages for unmatched closing braces and unclosed opening braces, ignoring braces in quoted strings and `//` comments
- LSP initialization, shutdown, and exit lifecycle handling

The LSP semantic-token legend is `keyword`, `type`, `function`, `number`, `string`, `comment`, `operator`; directive and import keywords use `keyword`, while import paths use `string`.

## Editors

- VS Code: `editors/vscode-shaft`
- Vim: `editors/vim`
- Neovim: `editors/neovim`

The Vim and Neovim integrations can point directly at `shaftls/build/shaftls`. VS Code retains its complete bundled JavaScript server by default; its TextMate grammar highlights the same imports and metaprogramming directives.

## Verify

```sh
SHAFTC=$PWD/build-linux-x86_64-release/shaftc \
  python3 -m unittest shaftls.tests.test_protocol -v
```
