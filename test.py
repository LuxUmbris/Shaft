#!/usr/bin/env python3
"""Run every Shaft repository regression from one entry point."""

import argparse
import contextlib
import os
import pathlib
import platform
import shutil
import subprocess
import sys


ROOT = pathlib.Path(__file__).resolve().parent
PHASES = (
    "python-unit",
    "shaftls-protocol",
    "vscode",
    "shaftc-smoke",
    "runtime-cross-compile",
)


def discover_shaftc(explicit):
    candidates = [
        explicit,
        os.environ.get("SHAFTC"),
        ROOT / "build" / "shaftc",
        *sorted(ROOT.glob("build*/shaftc"), key=lambda path: path.stat().st_mtime, reverse=True),
    ]
    for candidate in candidates:
        if candidate:
            path = pathlib.Path(candidate).expanduser().resolve()
            if path.is_file():
                return path
    raise RuntimeError("could not find shaftc; pass --shaftc /absolute/path/to/shaftc")


def discover_llvm_as(explicit):
    if explicit:
        path = pathlib.Path(explicit).expanduser().resolve()
        if path.is_file():
            return path
        raise RuntimeError(f"llvm-as is not a file: {path}")
    for name in ("llvm-as-18", "llvm-as"):
        path = shutil.which(name)
        if path:
            return pathlib.Path(path).resolve()
    raise RuntimeError("could not find llvm-as; pass --llvm-as /absolute/path/to/llvm-as")


@contextlib.contextmanager
def compiler_alias(shaftc):
    """Support legacy tests that still resolve the compiler as build/shaftc."""
    alias = ROOT / "build" / "shaftc"
    created_directory = False
    created_alias = False
    if alias.exists() or alias.is_symlink():
        if alias.resolve() != shaftc:
            raise RuntimeError(
                f"{alias} points to {alias.resolve()}, not the requested compiler {shaftc}; "
                "rebuild it or pass that compiler explicitly"
            )
    else:
        alias.parent.mkdir(exist_ok=True)
        created_directory = not any(alias.parent.iterdir())
        alias.symlink_to(shaftc)
        created_alias = True
    try:
        yield
    finally:
        if created_alias:
            alias.unlink()
        if created_directory:
            alias.parent.rmdir()


def run(label, command, environment):
    print(f"==> {label}", flush=True)
    subprocess.run(command, cwd=ROOT, env=environment, check=True)


def run_smoke(shaftc, llvm_as, build_dir, environment):
    if platform.system() != "Linux":
        print("==> shaftc-smoke (skipped: Linux-only executable validation)", flush=True)
        return
    sources = {
        "SMOKE_SOURCE": "smoke.shaft",
        "CLASS_INDEX_INIT_SOURCE": "class-index-init.shaft",
        "INVALID_CLASS_INDEX_SOURCE": "invalid-class-index.shaft",
        "ORDERED_USING_MACRO_SOURCE": "ordered-using-macro.shaft",
        "FORWARD_USING_MACRO_SOURCE": "forward-using-macro.shaft",
        "DUPLICATE_USING_MACRO_SOURCE": "duplicate-using-macro.shaft",
        "RECURSIVE_USING_MACRO_SOURCE": "recursive-using-macro.shaft",
        "INVALID_STRING_USING_MACRO_SOURCE": "invalid-string-using-macro.shaft",
        "NESTED_TEMPLATE_SOURCE": "nested-template.shaft",
        "MALFORMED_NESTED_TEMPLATE_SOURCE": "malformed-nested-template.shaft",
        "SELF_TYPE_METHOD_SOURCE": "self-type-method.shaft",
        "QUALIFIED_TYPES_SOURCE": "qualified-types.shaft",
        "STD_HASH_COLLECTIONS_SOURCE": "std-hash-collections.shaft",
        "STD_ENTRY_SOURCE": "std-entry.shaft",
        "UNKNOWN_CALL_SOURCE": "unknown-call.shaft",
        "DUPLICATE_DEFINITION_SOURCE": "duplicate-definition.shaft",
    }
    command = [
        "cmake",
        f"-DSHAFTC={shaftc}",
        f"-DSTDLIB_SOURCE={ROOT / 'std' / 'std.shaft'}",
        f"-DLLVM_AS={llvm_as}",
        f"-DWORK_DIR={build_dir / 'tests' / 'shaftc-smoke'}",
    ]
    command.extend(f"-D{key}={ROOT / 'tests' / value}" for key, value in sources.items())
    command.extend(["-P", str(ROOT / "tests" / "run_smoke.cmake")])
    run("shaftc-smoke", command, environment)


def run_all(args):
    shaftc = discover_shaftc(args.shaftc)
    build_dir = pathlib.Path(args.build_dir).expanduser().resolve() if args.build_dir else shaftc.parent
    environment = {**os.environ, "SHAFTC": str(shaftc)}
    requested = set(args.phase or PHASES)
    llvm_as = discover_llvm_as(args.llvm_as) if "shaftc-smoke" in requested and platform.system() == "Linux" else None

    with compiler_alias(shaftc):
        if "python-unit" in requested:
            run("python-unit", [sys.executable, "-m", "unittest", "discover", "-s", "tests", "-p", "test_*.py"], environment)
        if "shaftls-protocol" in requested:
            run("shaftls-protocol", [sys.executable, "-m", "unittest", "shaftls.tests.test_protocol"], environment)
        if "vscode" in requested:
            run("vscode", ["npm", "--prefix", "editors/vscode-shaft", "run", "check"], environment)
        if "shaftc-smoke" in requested:
            run_smoke(shaftc, llvm_as, build_dir, environment)
        if "runtime-cross-compile" in requested:
            run(
                "runtime-cross-compile",
                [
                    "cmake",
                    f"-DSHAFTC={shaftc}",
                    f"-DRUNTIME_DIR={ROOT / 'std' / 'runtime'}",
                    f"-DWORK_DIR={build_dir / 'tests' / 'runtime-cross-compile'}",
                    "-P",
                    str(ROOT / "tests" / "check_runtimes.cmake"),
                ],
                environment,
            )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--shaftc", help="absolute path to the compiler under test")
    parser.add_argument("--llvm-as", help="absolute path to llvm-as for the smoke phase")
    parser.add_argument("--build-dir", help="directory for CMake-script test artifacts")
    parser.add_argument("--phase", choices=PHASES, action="append", help="run only a named phase; may be repeated")
    parser.add_argument("--list", action="store_true", help="list test phases without requiring toolchains")
    args = parser.parse_args()
    if args.list:
        print("\n".join(PHASES))
        return
    try:
        run_all(args)
    except (RuntimeError, subprocess.CalledProcessError) as error:
        print(f"test.py: {error}", file=sys.stderr)
        raise SystemExit(1) from error


if __name__ == "__main__":
    main()
