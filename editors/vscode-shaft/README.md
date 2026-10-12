# Shaft for Visual Studio Code

A dependency-free VS Code extension that starts the bundled Node.js Shaft Language Server Protocol server.

## Features

- `.shaft` language mode with bracket matching, comment toggling, auto-closing pairs, indentation rules, and TextMate syntax highlighting.
- Semantic tokens from `shaftls` for Shaft declarations, keywords, types, functions, variables, literals, strings, operators, and comments; `import`, `@config`, `@asm`, and `@end`; plus raw-assembly instructions, registers, named operands, immediates, and comments.
- Live parser/compiler diagnostics with VS Code error underlines and hover messages; braces in strings and `//` comments are ignored by structural analysis.
- Completion for language keywords, project symbols, variables, functions, namespaces, and struct fields, plus symbol hover and definition navigation.
- Optional local Ollama suggestions (enabled by default) using `spark-48k`.
- Incremental live-document synchronization for open and edited `.shaft` buffers.
- Shaft snippets and syntax highlighting for declarations, macros, imports, types, and meta-programming.

The LSP and the Neovim plugin share the same bundled server implementation. Compiler diagnostics require `shaftc` on PATH or an explicit compiler setting. Ollama suggestions require the local Ollama API and configured model; ordinary syntax completion does not depend on Ollama.

## Install

Install the local extension through the Shaft installer:

```sh
python3 install.py --vscode
```

The command packages a temporary VSIX and invokes `code --install-extension … --force`; it requires `code`, `node`, and `zip` on `PATH`. Node.js must also be available to VS Code at runtime.

## Install for development

1. Open `/home/vince/dev/repos/Shaft-bootstrap/editors/vscode-shaft` in VS Code.
2. Press `F5` to open an Extension Development Host.
3. Open a `.shaft` file in the new window.

The extension uses Node supplied by VS Code. It has no `node_modules` directory and no network-time dependency.

## Configuration

| Setting | Default | Meaning |
| --- | --- | --- |
| `shaft.languageServer.serverPath` | empty | Optional language-server executable override. By default the extension starts its bundled server with `node`. |
| `shaft.languageServer.compilerPath` | empty | Optional `shaftc` path for live compiler diagnostics; otherwise resolved from the installer registration or PATH. |
| `shaft.languageServer.ollamaEnabled` | `true` | Enable local Ollama suggestions. |
| `shaft.languageServer.ollamaModel` | `spark-48k` | Ollama model tag for AI-assisted completions. |
| `shaft.languageServer.ollamaUrl` | `http://127.0.0.1:11434` | Local Ollama API base URL. |

The installer places both `shaftc` and `shaftls` in the prefix `bin` directory, so the default works after opening a new terminal/session.

## Commands

- **Shaft: Restart Language Server**
- **Shaft: Open Syntax Specification**

## Quality boundary

Compiler-backed diagnostics are emitted by `shaftc`; source-based analysis provides responsive highlighting, symbol navigation, and completion. Ollama is optional and local-only.

## Test

```sh
npm run check
```

This syntax-checks the extension assets, tests command selection and grammar behavior, and packages the VSIX. The Shaftls protocol suite is run from the repository root as documented in `shaftls/README.md`.
