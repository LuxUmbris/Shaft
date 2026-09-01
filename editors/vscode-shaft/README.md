# Shaft for Visual Studio Code

A dependency-free VS Code extension and stdio Language Server Protocol (LSP) server for Shaft.

## Features

- `.shaft` language mode with bracket matching, comment toggling, auto-closing pairs, indentation rules, and TextMate syntax highlighting.
- Semantic tokens for declarations, qualified names, primitive types, functions, literals, comments, and operators.
- Structural diagnostics while typing for unmatched or unclosed braces. Strings and `//` comments are ignored when balancing delimiters.
- Compiler diagnostics from `shaftc` while typing. All compiler errors and warnings that include a source location are translated into VS Code Problems entries and editor markers, including parser errors such as missing semicolons and semantic errors such as unknown functions.
- Completion for keywords, `Collections` bootstrap APIs, and indexed workspace declarations.
- Hover documentation for core Shaft concepts and resolved declarations.
- Go to Definition for local and indexed workspace declarations, including qualified names such as `Collections::HashMap` and `Core::answer`.
- Document outline, workspace symbols, folding ranges, and indentation formatting.
- Snippets for entry points, tunnel functions, namespaces, classes, `HashMap`, and `HashSet`.

## Install for development

1. Open `/home/vince/dev/repos/Shaft-bootstrap/editors/vscode-shaft` in VS Code.
2. Press `F5` to open an Extension Development Host.
3. Open a `.shaft` file in the new window.

The extension uses Node supplied by VS Code. It has no `node_modules` directory and no network-time dependency.

## Configuration

| Setting | Default | Meaning |
| --- | --- | --- |
| `shaft.languageServer.compilerPath` | empty | Optional compiler executable or absolute path. When empty, uses `install.py` registration, then `shaftc` on `PATH`. |
| `shaft.languageServer.diagnostics` | `onChange` | `onSave`, `onChange`, or `off`. Compiler errors are marked as you type by default. |
| `shaft.languageServer.stdlibPath` | empty | Optional `std.shaft` path passed with `--std`. |
| `shaft.languageServer.resourcePath` | empty | Optional Shaft resource root passed with `--resources`. |
| `shaft.languageServer.maxProblems` | `100` | Per-document diagnostics limit. |

When the workspace is the Shaft bootstrap repository, the server discovers `std/std.shaft` automatically for compiler diagnostics and corrects compiler ranges for the prepended standard-library source.

## Commands

- **Shaft: Restart Language Server**
- **Shaft: Check Active File**
- **Shaft: Open Syntax Specification**

## Quality boundary

The LSP performs fast structural analysis itself and delegates compiler semantic diagnostics to `shaftc`. Its workspace index recognizes top-level `namespace`, `class`, `struct`, `enum`, and function declarations. Compiler-aware references, rename, type inference, code actions, and full AST formatting should be added when the bootstrap compiler exposes stable machine-readable diagnostics and symbol/type queries.

## Test

```sh
npm run check
```

This syntax-checks the extension and server, tests the language analysis core, and launches the real server over stdio to validate initialize, completion, definition, formatting, and semantic-token protocol behavior.
