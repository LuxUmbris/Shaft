#!/usr/bin/env python3
"""Remove a Shaft installation created by install.py.

Usage:
    python3 uninstall.py [--prefix PREFIX] [--dry-run]

Only Shaft-owned files and PATH entries marked by install.py are removed.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))
import install


class UninstallError(RuntimeError):
    """An uninstall precondition failed."""


def remove_posix_path_block(profile: Path) -> bool:
    """Remove every bounded PATH block created by this installer from one file."""
    if not profile.exists():
        return False
    original = profile.read_text(encoding="utf-8")
    pattern = rf"(?:^|\n){re.escape(install.PATH_BLOCK_BEGIN)}\n.*?{re.escape(install.PATH_BLOCK_END)}\n?"
    updated = re.sub(pattern, "\n", original, flags=re.DOTALL)
    updated = re.sub(r"\n{3,}", "\n\n", updated).lstrip("\n")
    if updated == original:
        return False
    install._atomic_write(profile, updated)
    return True


def remove_windows_user_path(bin_directory: Path) -> bool:
    try:
        import winreg
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, r"Environment", 0, winreg.KEY_READ | winreg.KEY_WRITE) as key:
            try:
                current, value_type = winreg.QueryValueEx(key, "Path")
            except FileNotFoundError:
                return False
            entries = [entry for entry in current.split(";") if entry and Path(entry) != bin_directory]
            updated = ";".join(entries)
            if updated == current:
                return False
            winreg.SetValueEx(key, "Path", 0, value_type, updated)
        return True
    except OSError as error:
        raise UninstallError(f"could not update the Windows user PATH: {error}") from error


def registration_belongs_to_prefix(registration: Path, prefix: Path) -> bool:
    try:
        data = json.loads(registration.read_text(encoding="utf-8"))
        compiler = Path(data.get("compilerPath", "")).expanduser().resolve()
        return compiler == (prefix / "bin" / install.binary_name(install.host_target())).resolve()
    except (OSError, json.JSONDecodeError):
        return False


def uninstall(prefix: Path, target: install.Target, registration: Path) -> list[Path]:
    """Remove compiler, resources, and matching registration; return deleted paths."""
    prefix = prefix.expanduser().resolve()
    deleted: list[Path] = []
    compiler = prefix / "bin" / install.binary_name(target)
    resources = prefix / "share" / "shaft"
    if compiler.is_file():
        compiler.unlink()
        deleted.append(compiler)
    if resources.is_dir():
        shutil.rmtree(resources)
        deleted.append(resources)
    if registration.is_file() and registration_belongs_to_prefix(registration, prefix):
        registration.unlink()
        deleted.append(registration)
    return deleted


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--prefix", type=Path, default=Path.home() / ".local", help="Installation prefix (default: ~/.local).")
    parser.add_argument("--dry-run", action="store_true", help="Print planned removals without changing files.")
    return parser.parse_args()


def main() -> int:
    arguments = parse_arguments()
    try:
        target = install.host_target()
        prefix = arguments.prefix.expanduser().resolve()
        registration = install.language_server_config_path(target)
        compiler = prefix / "bin" / install.binary_name(target)
        resources = prefix / "share" / "shaft"
        profiles = [] if target.system == "windows" else install.posix_path_profiles(target)
        print(f"Host target: {target}")
        print(f"Install prefix: {prefix}")
        if arguments.dry_run:
            for path in (compiler, resources):
                if path.exists(): print(f"Would remove: {path}")
            if registration.is_file() and registration_belongs_to_prefix(registration, prefix): print(f"Would remove: {registration}")
            for profile in profiles:
                if profile.exists() and install.PATH_BLOCK_BEGIN in profile.read_text(encoding="utf-8"):
                    print(f"Would remove Shaft PATH block from: {profile}")
            print("Dry run successful; no files were changed.")
            return 0

        removed = uninstall(prefix, target, registration)
        if target.system == "windows":
            path_changed = remove_windows_user_path(prefix / "bin")
        else:
            changes = [remove_posix_path_block(profile) for profile in profiles]
            path_changed = any(changes)
        for path in removed:
            print(f"Removed {path}")
        if path_changed:
            print("Removed Shaft PATH configuration.")
        if not removed and not path_changed:
            print("No Shaft installation files or PATH configuration were found.")
        return 0
    except (UninstallError, install.InstallError) as error:
        print(f"uninstall.py: error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
