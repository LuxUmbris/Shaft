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
runtime = "vendor/runtime/linux.c"
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

The archive contains `bin/shaftc`, `share/shaft/std/std.shaft`, and Linux, Darwin, and Windows runtime sources. Linux is exercised end-to-end by the test suite; Darwin and Windows runtimes are compiled for their native targets during cross-target verification.

Run `ctest --test-dir build --output-on-failure` to verify every emit mode and the Linux exit-42 smoke binary.

## Install a local build

`install.py` installs a built compiler plus `std/std.shaft` and all platform runtime resources. It detects the host OS and architecture, inspects the executable header (ELF, Mach-O, or PE), and refuses to install a binary built for another target. The build directory name is irrelevant.

```sh
# Finds the one directory below the repository that contains a host-compatible shaftc.
python3 install.py

# Explicitly select any build-directory name.
python3 install.py build-linux-x86_64-release

# Install without administrator privileges (the default is ~/.local).
python3 install.py --prefix ~/.local

# Preview selection without writing files.
python3 install.py --dry-run
```

The installed layout is:

```text
PREFIX/bin/shaftc
PREFIX/share/shaft/std/std.shaft
PREFIX/share/shaft/std/runtime/{linux,darwin,windows}.c
```

`install.py` will not overwrite an existing `PREFIX/bin/shaftc` unless `--force` is supplied. When several compatible build directories are present, automatic discovery selects the most recently modified compiler binary; pass a directory explicitly to override that choice.

After a successful install, `install.py` also registers the compiler for the Shaft VS Code language server. The registration includes the compiler, standard-library, and resource paths, so VS Code discovers the installed compiler even when `PREFIX/bin` is not in VS Code's `PATH`.

`install.py` also persists `PREFIX/bin` in your user `PATH`: it adds a clearly marked bounded block to applicable shell startup files on Linux/macOS (Bash, Zsh, or Fish), or a single entry to the Windows user `Path` registry value. Open a new terminal (or source the affected profile) after installing. It never overwrites an existing PATH assignment.

Remove an installation with the matching prefix:

```sh
python3 uninstall.py --prefix ~/.local

# Preview exactly what would be removed.
python3 uninstall.py --prefix ~/.local --dry-run
```

`uninstall.py` removes only the installed Shaft compiler/resources, an LSP registration that belongs to that prefix, and PATH entries marked/created by `install.py`; unrelated files and user PATH entries are preserved. The registration file is:

```text
Linux:   $XDG_CONFIG_HOME/shaft/compiler.json (default: ~/.config/shaft/compiler.json)
macOS:   ~/Library/Application Support/Shaft/compiler.json
Windows: %APPDATA%/Shaft/compiler.json
```

An explicit `shaft.languageServer.compilerPath` setting still takes priority over the installer registration.

## VS Code and language server

The repository ships a dependency-free Shaft Language Server Protocol implementation and VS Code extension in [`editors/vscode-shaft`](editors/vscode-shaft). It provides syntax highlighting, snippets, formatting, structural diagnostics, live compiler error markers, semantic tokens, definitions, symbols, folding, completion, and hover cards with declarations and optional `///` documentation.

Use consecutive `///` comment lines immediately above a declaration to provide hover documentation. For example:

```shaft
/// Returns the canonical answer.
def answer() -> u64 result
{
    tunnel 42 -> u64 result;
}
```

For development, open that directory in VS Code and press `F5`. To validate and create an installable extension without marketplace tooling:

```
cd editors/vscode-shaft
npm run check
npm run package
```

This produces `shaft-0.1.0.vsix`, which can be installed with `code --install-extension shaft-0.1.0.vsix`.

## Repo branches
- `bootstrap` contains the bootstrap compiler.
- `main` contains the self-hosted compiler.
___
## License
[Apache 2.0](LICENSE)
