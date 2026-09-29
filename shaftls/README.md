# shaftls

`shaftls` is the dependency-free Shaft language server. It uses JSON-RPC 2.0 over stdio with `Content-Length` framing. Standard output is reserved for protocol frames; it emits no logs there.

## Build and run

From the repository root:

```sh
./build-linux-x86_64-release/shaftc --build shaftls/Shaft.build
./shaftls/build/shaftls
```

The server currently provides:

- incremental document synchronization (`didOpen`, full and ranged `didChange`, and `didClose`, which clears published diagnostics)
- semantic tokens for Shaft keywords, built-in and declared custom types (including lowercase declarations), functions, variables, literals, operators, strings, and comments; `import` and its quoted path; complete `@config.`, `@!config.`, `@asm`, and `@end` directives; `in` and `matches` configuration conditions; a red `@error` directive; and raw-assembly instructions, registers, named operands, and numeric immediates. Prefixes inside ordinary identifiers such as `@configurable` and `@endless` are never treated as directives.
- live `publishDiagnostics` messages for unmatched closing braces, unclosed opening braces, and unterminated quoted strings; when structural checks succeed and compiler diagnostics are configured, the current in-memory document is also checked with `shaftc --check-only` and its first source diagnostic is translated to an LSP range. Braces in quoted strings and `//` comments are ignored.
- LSP initialization, shutdown, and exit lifecycle handling

The LSP semantic-token legend is `keyword`, `type`, `function`, `number`, `string`, `comment`, `operator`, `macro`, `variable`, `error`; directives, configuration conditions, and import keywords use `keyword`; `@error` uses `error`; import paths use `string`; normal Shaft source uses the matching source categories; and raw assembly uses `macro` for instructions, `variable` for registers/named operands, `number` for numeric immediates, and `comment` for `//` comments.

Compiler diagnostics require launcher-provided initialization options because the freestanding process runtime executes an explicit path and cannot search `PATH`, create a secure temporary directory, or clean up snapshots. Provide an absolute compiler executable and an isolated, writable snapshot path that the launcher owns and cleans up, for example `{"compilerPath":"/opt/shaft/bin/shaftc","diagnosticPath":"/run/user/1000/shaftls-42/live.shaft"}`. For source files with relative imports, provision the snapshot beside the source so compiler import resolution remains correct. Without both options, ShaftLS continues to publish its structural diagnostics only.

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
