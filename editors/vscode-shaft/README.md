# Shaft for Visual Studio Code

A dependency-free VS Code extension that starts the installed `shaftls` stdio Language Server Protocol server for Shaft.

## Features

- `.shaft` language mode with bracket matching, comment toggling, auto-closing pairs, indentation rules, and TextMate syntax highlighting.
- Semantic tokens from `shaftls` for `import`, import paths, and `@config`, `@asm`, and `@end` directives.
- Structural diagnostics from `shaftls` for unmatched/unclosed braces; braces in strings and `//` comments are ignored.
- Incremental live-document synchronization for open and edited `.shaft` buffers.
- Shaft snippets and syntax highlighting for declarations, macros, imports, types, and meta-programming.

`shaftls` does not yet advertise compiler-backed diagnostics, completion, definition, hover, formatting, or symbols; the extension intentionally does not register providers for those unsupported LSP methods.

## Install

Install the local extension through the Shaft installer:

```sh
python3 install.py --vscode
```

The command packages a temporary VSIX and invokes `code --install-extension … --force`; it requires `code`, `node`, and `zip` on `PATH`.

## Install for development

1. Open `/home/vince/dev/repos/Shaft-bootstrap/editors/vscode-shaft` in VS Code.
2. Press `F5` to open an Extension Development Host.
3. Open a `.shaft` file in the new window.

The extension uses Node supplied by VS Code. It has no `node_modules` directory and no network-time dependency.

## Configuration

| Setting | Default | Meaning |
| --- | --- | --- |
| `shaft.languageServer.serverPath` | empty | Absolute `shaftls` executable. When empty, uses `shaftls` on `PATH`. |

The installer places both `shaftc` and `shaftls` in the prefix `bin` directory, so the default works after opening a new terminal/session.

## Commands

- **Shaft: Restart Language Server**
- **Shaft: Open Syntax Specification**

## Quality boundary

`shaftls` is the production editor server for this extension. Its current source-based analysis deliberately covers only imports, meta-programming directives, and structural braces. Compiler-driven semantic diagnostics and navigation remain future Shaftls capabilities.

## Test

```sh
npm run check
```

This syntax-checks the extension assets, tests command selection and grammar behavior, and packages the VSIX. The Shaftls protocol suite is run from the repository root as documented in `shaftls/README.md`.
