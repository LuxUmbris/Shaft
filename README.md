# Shaft Programming Language

Shaft is a language designed to match Rust's safety while remaining readable and easy to use. Its syntax is described in [syntax.md](syntax.md).

This branch contains the C++/LLVM bootstrap compiler.

```
def main(String[] args) {}
```

## Build and run

LLVM 18 development files, CMake 3.20+, Python 3.11 and a C++17 compiler are required.

```
python3 build.py <architecture> <debug|release>
python3 install.py
# Installs shaftc and shaftls into the selected prefix's bin directory.
# Optional editor integrations:
python3 install.py --vscode --vim --neovim
```

### `Shaft.build` project builds

`shaftc --build` reads `Shaft.build` from the current directory. Pass a different file with `shaftc --build path/to/Project.build` (or `--build=path/to/Project.build`). `entry` selects the root Shaft script. The compiler follows relative `import "path.shaft";` directives from that root, resolves each module once, rejects dependency cycles, and compiles the complete dependency graph as separate source modules. Build-file paths are resolved relative to the build file, not the current directory, and missing output directories are created automatically.

The repository includes a usable `Shaft.build` for the descriptor-driven quiz:

```sh
shaftc --build
printf '4\nP\n' | ./build/interactive-quiz examples/interactive-quiz.descriptor
```

`Shaft.build` is a deliberately small TOML-like format. It supports quoted strings, arrays of quoted strings, `true`/`false`, `#` comments, and the `[package]` and `[build]` tables. Unknown tables and keys are errors so configuration typos do not silently change a build.

```toml
[package]
name = "my-app"       # accepted project metadata
version = "0.1.0"     # accepted project metadata

[build]
entry = "src/main.shaft"        # required root module/script
output = "build/my-app"         # optional; default follows entry name
emit = "binary"                 # binary | llvm | object | asm | staticlib | dynamiclib
optimization = "O2"             # O0 through O3; "0" through "3" also work
no_std = false
native = false
hosted = false                    # enable the host C runtime for C-library linkage
link_directories = ["vendor/lib"] # config-relative C-library search directories
links = ["raylib", "m"]           # names passed to the host linker as -l<name>
target = "x86_64-unknown-linux-gnu"
stdlib = "vendor/std.shaft"
runtime = "vendor/runtime/linux.shaft"
resources = "vendor/resources"
check_only = false
verbose = false
```

`entry`, `output`, `stdlib`, `runtime`, `resources`, and each `link_directories` entry are resolved relative to the build file. `links` is an array of C-library names and is forwarded as `-l<name>`; it and `link_directories` require `hosted = true`.

By default, `shaftc` emits a freestanding binary using the runtime for its host OS. Select another artifact with `--emit`:

```
./build/shaftc --emit llvm -o program.ll program.shaft
./build/shaftc --emit object -o program.o program.shaft
./build/shaftc --emit asm -o program.s program.shaft
./build/shaftc --emit staticlib -o libprogram.a program.shaft
./build/shaftc --emit dynamiclib -o libprogram.so program.shaft
```

`std/std.shaft` is prepended automatically. Use `--no-std` for a bare compilation, or `--std PATH`, `--runtime PATH`, and `--resources PATH` to override bundled resources. Installed compilers discover their `share/shaft` directory relative to the executable; `SHAFT_HOME` is an alternative resource root.

### Compiler flags

```sh
# Print the compiler identity without providing an input file.
shaftc --version

# Run lexer, parser, and checker only; no LLVM module or output artifact is produced.
shaftc --check-only program.shaft

# Generate optimized output (the default is -O2); use -O0 for shortest compile latency.
shaftc -O3 -o program program.shaft

# Tune native object, assembly, or binary output for the current CPU only.
shaftc --native -o program program.shaft

# Select an LLVM target triple. Both forms are accepted.
shaftc --target x86_64-unknown-linux-gnu --emit llvm -o program.ll program.shaft
shaftc --target=x86_64-unknown-linux-gnu --emit object -o program.o program.shaft

# Report the selected target and compilation stages on stderr
shaftc --verbose --check-only program.shaft

# Checks and dumps AST
shaftc —-check-only —-dump-ast program.shaft 
```

`--target` is validated by LLVM. It sets the target triple in emitted LLVM IR and selects the LLVM target machine for object, assembly, and native artifact emission. `--native` selects the current machine's LLVM CPU/features for optimization and native emission; it is intentionally rejected with an explicit cross target, because host ISA features are not portable. `--check-only` still reports lexer, parser, and checker errors, making it suitable for editor and CI validation.

### Source configuration and inline assembly

Source can select target- or package-specific text before it reaches the parser:

```shaft
@config.build.target = "x86_64-unknown-linux-gnu"
@asm
    nop
@end
@end
```

`@config.<build|package>.<field> = value ... @end` is a lexer macro: matching blocks remain and nonmatching blocks are deleted before import discovery and parsing. `build.target` reflects `--target` (or the host triple); `package.name` and `package.version` come from `Shaft.build`. `@asm ... @end` is a function-body, side-effecting raw LLVM inline-assembly statement. `@asm(value)` passes a binding as a read-only register operand; `@asm(mut value)` writes its final register value back to a mutable binding; and `$value` names that operand in the assembly body. `cdef naked` emits no compiler prologue/epilogue and must return from its own assembly. See `syntax.md` for the complete supported configuration fields and nesting rules.

## Reproducible performance benchmarks

`benchmarks/run.py` generates equivalent Shaft, C, and Rust workloads in a temporary directory, records source hashes/sizes, exact commands, tool versions, host metadata, warm-ups, samples, medians, binary sizes, and toolchain availability in JSON. It never installs a compiler. Run it after building `shaftc`:

```sh
python3 benchmarks/run.py --iterations 7  —runtime-iterations 7 --output /tmp/shaft-benchmark-results.json
```

The compile workload compares Shaft `--no-std -O2 --emit llvm`, Clang `-O2 -march=native -S -emit-llvm`, and—when present—rustc `-C opt-level=2 -C target-cpu=native --emit=llvm-ir`. The runtime workload uses the same xorshift recurrence and reports a deliberate `unavailable` Rust result when `rustc` is not on `PATH`, rather than fabricating a comparison.

## Installer package

Build a release package for the current host OS with:

```
python3 build_installer.py
```

The archive contains `bin/shaftc`, `bin/shaftls`, `share/shaft/std/std.shaft`, and Linux, Darwin, and Windows runtime sources. Linux is exercised end-to-end by the test suite; Darwin and Windows runtimes are compiled for their native targets during cross-target verification.

Run `ctest --test-dir build --output-on-failure` to verify every emit mode and the Linux exit-42 smoke binary.

## Install a local build

`install.py` installs a built `shaftc` compiler, its matching native `shaftls` server, `std/std.shaft`, and all platform runtime resources. It detects the host OS and architecture, inspects the executable headers (ELF, Mach-O, or PE), and refuses to install binaries built for another target. The build directory name is irrelevant.

```sh
# Finds the one directory below the repository that contains a host-compatible Shaft build.
python3 install.py

# Explicitly select any build-directory name.
python3 install.py build-linux-x86_64-release

# Install without administrator privileges (the default is ~/.local).
python3 install.py --prefix ~/.local

# Install the VS Code extension and the Vim and Neovim integrations as well.
python3 install.py --vscode --vim --neovim

# Preview selection and editor destinations without writing files.
python3 install.py --vscode --vim --neovim --dry-run
```

The installed layout is:

```text
PREFIX/bin/shaftc
PREFIX/bin/shaftls
PREFIX/share/shaft/std/std.shaft
PREFIX/share/shaft/std/runtime/{linux,darwin,macos,windows}.shaft
```

`install.py` will not overwrite an existing `PREFIX/bin/shaftc` or `PREFIX/bin/shaftls` unless `--force` is supplied. When several compatible build directories are present, automatic discovery selects the most recently modified compiler binary; pass a directory explicitly to override that choice.

`install.py` also persists `PREFIX/bin` in your user `PATH`: it adds a clearly marked bounded block to applicable shell startup files on Linux/macOS (Bash, Zsh, or Fish), or a single entry to the Windows user `Path` registry value. Open a new terminal (or source the affected profile) after installing. It never overwrites an existing PATH assignment.

Remove an installation with the matching prefix:

```sh
python3 uninstall.py --prefix ~/.local

# Preview exactly what would be removed.
python3 uninstall.py --prefix ~/.local --dry-run
```

`uninstall.py` removes only the installed Shaft compiler, language server, resources, and PATH entries marked/created by `install.py`; unrelated files and user PATH entries are preserved.

## Editor integrations and language server

`shaftls` is a dependency-free native Language Server Protocol executable. `install.py` installs it alongside `shaftc`, so the default editor commands resolve `shaftls` from `PATH`.

```sh
# Install every shipped integration after building Shaft.
python3 install.py --vscode --vim --neovim
```

- `--vscode` packages a temporary VSIX and runs `code --install-extension … --force`. It requires `code`, `node`, and `zip` on `PATH`. The installed extension is `shaft-lang.shaft`.
- `--vim` copies only `ftdetect/shaft.vim`, `syntax/shaft.vim`, and `plugin/shaft_lsp.vim` into `~/.vim` (`~/vimfiles` on Windows).
- `--neovim` copies only `lua/shaft/init.lua` and `plugin/shaft.lua` into `~/.config/nvim` by default, or `$XDG_CONFIG_HOME/nvim`. Its plugin enables the built-in LSP client for Shaft buffers.

The VS Code extension can use `shaft.languageServer.serverPath` to select an explicit `shaftls` executable. When that setting is empty, it uses `shaftls` from `PATH`. Vim honors `g:shaftls_cmd`; Neovim accepts `require('shaft').setup({ cmd = '/path/to/shaftls' })` if an override is needed.

The native server currently provides lifecycle/document synchronization; semantic tokens for `import`, import paths, `@config`, `@asm`, and `@end`; and raw-assembly instructions, registers, named operands, numeric immediates, and comments, plus structural diagnostics for unmatched or unclosed braces. Syntax highlighting and snippets are provided by the editor assets. It does **not** yet provide compiler-derived parser/checker/import diagnostics, completion, hover, definition, symbols, or formatting; the VS Code extension intentionally does not register providers for those methods.

For extension development, open `editors/vscode-shaft` in VS Code and press `F5`. Validate and package without marketplace tooling:

```sh
cd editors/vscode-shaft
npm run check
npm run package
```

`uninstall.py` removes only the installed `shaftc`, `shaftls`, resources, and marked PATH entries. It does not remove editor files or the VS Code extension from a user profile.

## Repo branches
- `bootstrap` contains the bootstrap compiler.
- `main` contains the self-hosted compiler.
___
## License
[Apache 2.0](LICENSE)
