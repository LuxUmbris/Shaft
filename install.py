#!/usr/bin/env python3
"""Install a host-compatible Shaft build and its bundled resources.

Usage:
    python3 install.py [BUILD_DIR] [--prefix PREFIX] [--force] [--vscode] [--vim] [--neovim]

When BUILD_DIR is omitted, the installer scans this repository for a directory
containing a usable `shaftc` binary. Build directory names are not significant.
"""

from __future__ import annotations

import argparse
import os
import platform
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parent
RUNTIME_NAMES = ("linux.shaft", "darwin.shaft", "macos.shaft", "windows.shaft")
PATH_BLOCK_BEGIN = "# >>> Shaft compiler PATH >>>"
PATH_BLOCK_END = "# <<< Shaft compiler PATH <<<"


class InstallError(RuntimeError):
    """An installation precondition or verification failed."""


@dataclass(frozen=True)
class Target:
    system: str
    architecture: str

    def __str__(self) -> str:
        return f"{self.system}/{self.architecture}"


def normalize_system(value: str) -> str:
    names = {"linux": "linux", "darwin": "darwin", "macos": "darwin", "windows": "windows", "win32": "windows", "cygwin": "windows", "msys": "windows"}
    system = names.get(value.strip().lower())
    if not system:
        raise InstallError(f"unsupported host OS: {value}")
    return system


def normalize_architecture(value: str) -> str:
    names = {
        "x86_64": "x86_64", "amd64": "x86_64", "x64": "x86_64",
        "aarch64": "aarch64", "arm64": "aarch64",
        "x86": "x86", "i386": "x86", "i686": "x86",
    }
    architecture = names.get(value.strip().lower())
    if not architecture:
        raise InstallError(f"unsupported host architecture: {value}")
    return architecture


def host_target() -> Target:
    return Target(normalize_system(platform.system()), normalize_architecture(platform.machine()))


def binary_name(target: Target) -> str:
    return "shaftc.exe" if target.system == "windows" else "shaftc"


def language_server_name(target: Target) -> str:
    return "shaftls.exe" if target.system == "windows" else "shaftls"


def inspect_binary_bytes(data: bytes) -> Target:
    if data.startswith(b"\x7fELF"):
        if len(data) < 20:
            raise InstallError("truncated ELF binary")
        little_endian = data[5] == 1
        byte_order = "<" if little_endian else ">"
        machine = struct.unpack_from(f"{byte_order}H", data, 18)[0]
        architectures = {3: "x86", 62: "x86_64", 183: "aarch64"}
        architecture = architectures.get(machine)
        if not architecture:
            raise InstallError(f"unsupported ELF architecture code: {machine}")
        return Target("linux", architecture)

    if data.startswith(b"MZ"):
        if len(data) < 0x40:
            raise InstallError("truncated PE binary")
        offset = struct.unpack_from("<I", data, 0x3C)[0]
        if len(data) < offset + 6 or data[offset:offset + 4] != b"PE\0\0":
            raise InstallError("invalid PE binary")
        machine = struct.unpack_from("<H", data, offset + 4)[0]
        architectures = {0x014C: "x86", 0x8664: "x86_64", 0xAA64: "aarch64"}
        architecture = architectures.get(machine)
        if not architecture:
            raise InstallError(f"unsupported PE architecture code: {machine:#x}")
        return Target("windows", architecture)

    macho_magic = data[:4]
    magic = {b"\xcf\xfa\xed\xfe": ("<", False), b"\xfe\xed\xfa\xcf": (">", False), b"\xca\xfe\xba\xbe": (">", True), b"\xbe\xba\xfe\xca": ("<", True)}
    if macho_magic in magic:
        byte_order, fat = magic[macho_magic]
        if fat:
            if len(data) < 16:
                raise InstallError("truncated universal Mach-O binary")
            architecture_code = struct.unpack_from(f"{byte_order}I", data, 8)[0]
        else:
            if len(data) < 8:
                raise InstallError("truncated Mach-O binary")
            architecture_code = struct.unpack_from(f"{byte_order}I", data, 4)[0]
        architectures = {7: "x86", 0x01000007: "x86_64", 0x0100000C: "aarch64"}
        architecture = architectures.get(architecture_code)
        if not architecture:
            raise InstallError(f"unsupported Mach-O architecture code: {architecture_code:#x}")
        return Target("darwin", architecture)

    raise InstallError("unrecognized executable format; expected ELF, Mach-O, or PE")


def inspect_binary(path: Path) -> Target:
    try:
        with path.open("rb") as binary:
            return inspect_binary_bytes(binary.read(4096))
    except OSError as error:
        raise InstallError(f"cannot inspect compiler binary '{path}': {error}") from error


def binary_in(directory: Path, target: Target | None = None) -> Path | None:
    names = [binary_name(target)] if target else ["shaftc", "shaftc.exe"]
    for name in names:
        candidate = directory / name
        if candidate.is_file():
            return candidate
    return None


def discover_build_directory(root: Path, expected_binary: str, expected_target: Target | None = None) -> Path:
    direct = root / expected_binary
    if direct.is_file():
        if expected_target is not None:
            validate_binary(direct, expected_target)
        return root
    candidates: list[tuple[Path, Path]] = []
    for child in root.iterdir():
        binary = child / expected_binary
        if not child.is_dir() or not binary.is_file():
            continue
        if expected_target is not None:
            try:
                validate_binary(binary, expected_target)
            except InstallError:
                continue
        candidates.append((child, binary))
    if not candidates:
        target_suffix = f" compatible with {expected_target}" if expected_target else ""
        raise InstallError(f"no build directory containing '{expected_binary}'{target_suffix} was found under {root}; pass BUILD_DIR explicitly")
    return max(candidates, key=lambda candidate: (candidate[1].stat().st_mtime_ns, candidate[0].name))[0]


def validate_binary(binary: Path, expected: Target) -> None:
    actual = inspect_binary(binary)
    if actual != expected:
        raise InstallError(f"compiler '{binary}' targets {actual}, but this host is {expected}")


def copy_file(source: Path, destination: Path, executable: bool = False) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary = destination.with_name(f".{destination.name}.tmp-{os.getpid()}")
    shutil.copy2(source, temporary)
    if executable:
        temporary.chmod(temporary.stat().st_mode | 0o111)
    os.replace(temporary, destination)


def install_editor_files(source_root: Path, destination_root: Path, files: tuple[str, ...]) -> list[Path]:
    sources = [source_root / relative for relative in files]
    missing = [str(path) for path in sources if not path.is_file()]
    if missing:
        raise InstallError("editor integration resources are incomplete: " + ", ".join(missing))
    installed: list[Path] = []
    for relative, source in zip(files, sources):
        destination = destination_root / relative
        copy_file(source, destination)
        installed.append(destination)
    return installed


def install_vim(source_root: Path, vim_home: Path) -> list[Path]:
    return install_editor_files(source_root / "editors" / "vim", vim_home, (
        "ftdetect/shaft.vim", "syntax/shaft.vim", "plugin/shaft_lsp.vim",
    ))


def install_neovim(source_root: Path, config_home: Path) -> list[Path]:
    return install_editor_files(source_root / "editors" / "neovim", config_home, (
        "lua/shaft/init.lua", "plugin/shaft.lua",
    ))


def neovim_config_directory(target: Target, environment: dict[str, str] | None = None) -> Path:
    environment = environment or os.environ
    home = Path(environment.get("HOME") or Path.home())
    if target.system == "windows":
        return Path(environment.get("LOCALAPPDATA") or home / "AppData" / "Local") / "nvim"
    return Path(environment.get("XDG_CONFIG_HOME") or home / ".config") / "nvim"


def vim_runtime_directory(target: Target, environment: dict[str, str] | None = None) -> Path:
    environment = environment or os.environ
    home = Path(environment.get("HOME") or Path.home())
    return home / ("vimfiles" if target.system == "windows" else ".vim")


def install_vscode(source_root: Path, code_command: str = "code") -> None:
    extension_root = source_root / "editors" / "vscode-shaft"
    packager = extension_root / "scripts" / "package-vsix.cjs"
    if not packager.is_file():
        raise InstallError(f"VS Code packager is missing: {packager}")
    node = shutil.which("node")
    code = shutil.which(code_command)
    if not node:
        raise InstallError("--vscode requires node on PATH to package the extension")
    if not code:
        raise InstallError(f"--vscode requires '{code_command}' on PATH")
    with tempfile.TemporaryDirectory(prefix="shaft-vscode-") as temporary:
        archive = Path(temporary) / "shaft.vsix"
        try:
            subprocess.run([node, str(packager), str(archive)], cwd=extension_root, check=True)
            subprocess.run([code, "--install-extension", str(archive), "--force"], check=True)
        except subprocess.CalledProcessError as error:
            raise InstallError(f"VS Code extension installation failed with exit status {error.returncode}") from error


def ensure_prefix(prefix: Path, target: Target, force: bool) -> None:
    if prefix.exists() and not prefix.is_dir():
        raise InstallError(f"installation prefix is not a directory: {prefix}")
    binaries = [prefix / "bin" / binary_name(target), prefix / "bin" / language_server_name(target)]
    existing = [binary for binary in binaries if binary.exists()]
    if existing and not force:
        raise InstallError(f"{existing[0]} already exists; use --force to replace this Shaft installation")


def install(build_dir: Path, source_root: Path, prefix: Path, target: Target, force: bool) -> Path:
    build_dir = build_dir.resolve()
    prefix = prefix.expanduser().resolve()
    compiler = binary_in(build_dir, target) or binary_in(build_dir)
    language_server = build_dir / language_server_name(target)
    if compiler is None:
        raise InstallError(f"build directory does not contain {binary_name(target)}: {build_dir}")
    if not language_server.is_file():
        raise InstallError(f"build directory does not contain {language_server_name(target)}: {build_dir}")
    validate_binary(compiler, target)
    validate_binary(language_server, target)

    standard_library = source_root / "std" / "std.shaft"
    runtime_dir = source_root / "std" / "runtime"
    required = [standard_library, *(runtime_dir / name for name in RUNTIME_NAMES)]
    missing = [str(path) for path in required if not path.is_file()]
    if missing:
        raise InstallError("source resources are incomplete: " + ", ".join(missing))

    ensure_prefix(prefix, target, force)
    installed_binary = prefix / "bin" / binary_name(target)
    installed_language_server = prefix / "bin" / language_server_name(target)
    copy_file(compiler, installed_binary, executable=True)
    copy_file(language_server, installed_language_server, executable=True)
    copy_file(standard_library, prefix / "share" / "shaft" / "std" / "std.shaft")
    for runtime in RUNTIME_NAMES:
        copy_file(runtime_dir / runtime, prefix / "share" / "shaft" / "std" / "runtime" / runtime)

    if not installed_binary.is_file() or not os.access(installed_binary, os.X_OK):
        raise InstallError(f"installed compiler is missing or not executable: {installed_binary}")
    if not installed_language_server.is_file() or not os.access(installed_language_server, os.X_OK):
        raise InstallError(f"installed language server is missing or not executable: {installed_language_server}")
    if not (prefix / "share" / "shaft" / "std" / "std.shaft").is_file():
        raise InstallError("installed standard library is missing")
    return installed_binary



def _atomic_write(destination: Path, content: str) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary = destination.with_name(f".{destination.name}.tmp-{os.getpid()}")
    temporary.write_text(content, encoding="utf-8")
    os.replace(temporary, destination)


def posix_path_block(bin_directory: Path, fish: bool = False) -> str:
    command = f"set -gx PATH {bin_directory} $PATH" if fish else f"export PATH=\"{bin_directory}:$PATH\""
    return f"{PATH_BLOCK_BEGIN}\n{command}\n{PATH_BLOCK_END}\n"


def update_posix_path_file(profile: Path, bin_directory: Path, fish: bool = False) -> bool:
    """Replace this installer's bounded PATH block without altering user content."""
    original = profile.read_text(encoding="utf-8") if profile.exists() else ""
    pattern = rf"(?:^|\n){re.escape(PATH_BLOCK_BEGIN)}\n.*?{re.escape(PATH_BLOCK_END)}\n?"
    preserved = re.sub(pattern, "\n", original, flags=re.DOTALL).rstrip("\n")
    updated = f"{preserved}\n" if preserved else ""
    updated += posix_path_block(bin_directory, fish)
    if updated == original:
        return False
    _atomic_write(profile, updated)
    return True


def posix_path_profiles(target: Target, environment: dict[str, str] | None = None) -> list[Path]:
    environment = environment or os.environ
    home = Path(environment.get("HOME") or Path.home())
    if target.system == "darwin":
        return [home / ".zprofile", home / ".zshrc", home / ".profile"]
    shell = Path(environment.get("SHELL", "")).name
    profiles = [home / ".profile"]
    if shell == "zsh":
        profiles.extend([home / ".zprofile", home / ".zshrc"])
    elif shell == "fish":
        profiles.append(home / ".config" / "fish" / "conf.d" / "shaft.fish")
    else:
        profiles.append(home / ".bashrc")
    return profiles


def add_windows_user_path(bin_directory: Path) -> bool:
    try:
        import winreg
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, r"Environment", 0, winreg.KEY_READ | winreg.KEY_WRITE) as key:
            try:
                current, value_type = winreg.QueryValueEx(key, "Path")
            except FileNotFoundError:
                current, value_type = "", winreg.REG_EXPAND_SZ
            entries = [entry for entry in current.split(";") if entry]
            if str(bin_directory) in entries:
                return False
            winreg.SetValueEx(key, "Path", 0, value_type, ";".join([str(bin_directory), *entries]))
        return True
    except OSError as error:
        raise InstallError(f"could not update the Windows user PATH: {error}") from error


def register_path(prefix: Path, target: Target, environment: dict[str, str] | None = None) -> list[Path]:
    """Persist the compiler bin directory in the user's command search path."""
    bin_directory = prefix / "bin"
    if target.system == "windows":
        add_windows_user_path(bin_directory)
        return []
    profiles = posix_path_profiles(target, environment)
    for profile in profiles:
        update_posix_path_file(profile, bin_directory, profile.suffix == ".fish")
    return profiles


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("build_dir", nargs="?", type=Path, help="Build directory containing shaftc/shaftc.exe; auto-discovered when omitted.")
    parser.add_argument("--prefix", type=Path, default=Path.home() / ".local", help="Installation prefix (default: ~/.local).")
    parser.add_argument("--force", action="store_true", help="Replace an existing compiler in the prefix.")
    parser.add_argument("--vscode", action="store_true", help="Package and install the Shaft VS Code extension with code.")
    parser.add_argument("--vim", action="store_true", help="Install Shaft Vim runtime files into ~/.vim (or ~/vimfiles on Windows).")
    parser.add_argument("--neovim", action="store_true", help="Install and auto-enable Shaftls in the Neovim configuration directory.")
    parser.add_argument("--dry-run", action="store_true", help="Validate selection and print the planned install without changing files.")
    return parser.parse_args()


def main() -> int:
    arguments = parse_arguments()
    try:
        target = host_target()
        expected_binary = binary_name(target)
        build_dir = arguments.build_dir.resolve() if arguments.build_dir else discover_build_directory(ROOT, expected_binary, target)
        compiler = binary_in(build_dir, target) or binary_in(build_dir)
        language_server = build_dir / language_server_name(target)
        if compiler is None:
            raise InstallError(f"build directory does not contain {expected_binary}: {build_dir}")
        if not language_server.is_file():
            raise InstallError(f"build directory does not contain {language_server_name(target)}: {build_dir}")
        validate_binary(compiler, target)
        validate_binary(language_server, target)
        prefix = arguments.prefix.expanduser().resolve()
        print(f"Host target: {target}")
        print(f"Build directory: {build_dir}")
        print(f"Compiler: {compiler}")
        print(f"Language server: {language_server}")
        print(f"Install prefix: {prefix}")
        if arguments.vim:
            print(f"Vim runtime: {vim_runtime_directory(target)}")
        if arguments.neovim:
            print(f"Neovim configuration: {neovim_config_directory(target)}")
        if arguments.vscode:
            print("VS Code extension: code --install-extension <packaged Shaft VSIX> --force")
        if arguments.dry_run:
            print("Dry run successful; no files were changed.")
            return 0
        installed = install(build_dir, ROOT, prefix, target, arguments.force)
        profiles = register_path(prefix, target)
        vim_files = install_vim(ROOT, vim_runtime_directory(target)) if arguments.vim else []
        neovim_files = install_neovim(ROOT, neovim_config_directory(target)) if arguments.neovim else []
        if arguments.vscode:
            install_vscode(ROOT)
        print(f"Installed {installed}")
        print(f"Installed {prefix / 'bin' / language_server_name(target)}")
        print(f"Resources: {prefix / 'share' / 'shaft'}")
        for path in vim_files:
            print(f"Installed Vim runtime: {path}")
        for path in neovim_files:
            print(f"Installed Neovim runtime: {path}")
        if arguments.vscode:
            print("Installed Shaft VS Code extension.")

        if profiles:
            print("PATH updated in: " + ", ".join(str(profile) for profile in profiles))
            print("Open a new terminal or source the updated shell profile to use shaftc immediately.")
        elif target.system == "windows":
            print("Windows user PATH updated. Open a new terminal to use shaftc immediately.")
        return 0
    except InstallError as error:
        print(f"install.py: error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
