#!/usr/bin/env python3
"""Regression tests for the portable Shaft installer."""

from __future__ import annotations

import importlib.util
import json
import os
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("shaft_install", ROOT / "install.py")
assert SPEC and SPEC.loader
installer = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = installer
SPEC.loader.exec_module(installer)
UNINSTALL_SPEC = importlib.util.spec_from_file_location("shaft_uninstall", ROOT / "uninstall.py")
assert UNINSTALL_SPEC and UNINSTALL_SPEC.loader
uninstaller = importlib.util.module_from_spec(UNINSTALL_SPEC)
sys.modules[UNINSTALL_SPEC.name] = uninstaller
UNINSTALL_SPEC.loader.exec_module(uninstaller)


def elf(machine: int) -> bytes:
    return b"\x7fELF" + bytes([2, 1, 1]) + b"\0" * 9 + struct.pack("<H", 2) + struct.pack("<H", machine) + b"\0" * 40


def pe(machine: int) -> bytes:
    data = bytearray(128)
    data[:2] = b"MZ"
    struct.pack_into("<I", data, 0x3C, 64)
    data[64:68] = b"PE\0\0"
    struct.pack_into("<H", data, 68, machine)
    return bytes(data)


class InstallerTests(unittest.TestCase):
    def test_normalizes_host_names(self) -> None:
        self.assertEqual(installer.normalize_system("Darwin"), "darwin")
        self.assertEqual(installer.normalize_system("Windows"), "windows")
        self.assertEqual(installer.normalize_architecture("AMD64"), "x86_64")
        self.assertEqual(installer.normalize_architecture("arm64"), "aarch64")

    def test_detects_elf_and_windows_pe_targets(self) -> None:
        self.assertEqual(installer.inspect_binary_bytes(elf(62)), installer.Target("linux", "x86_64"))
        self.assertEqual(installer.inspect_binary_bytes(elf(183)), installer.Target("linux", "aarch64"))
        self.assertEqual(installer.inspect_binary_bytes(pe(0x8664)), installer.Target("windows", "x86_64"))

    def test_detects_universal_macos_target(self) -> None:
        universal = b"\xca\xfe\xba\xbe" + struct.pack(">I", 1) + struct.pack(">I", 0x0100000C) + b"\0" * 24
        self.assertEqual(installer.inspect_binary_bytes(universal), installer.Target("darwin", "aarch64"))

    def test_discovers_a_build_directory_without_a_fixed_name(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            ignored = root / "build-debug"
            ignored.mkdir()
            candidate = root / "arbitrary-output-name"
            candidate.mkdir()
            (candidate / "shaftc").write_bytes(elf(62))
            self.assertEqual(installer.discover_build_directory(root, "shaftc"), candidate)

    def test_discovers_most_recent_compatible_build_when_names_do_not_matter(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            incompatible = root / "windows-output"
            older = root / "older-output"
            selected = root / "latest-output"
            for directory, contents in ((incompatible, pe(0x8664)), (older, elf(62)), (selected, elf(62))):
                directory.mkdir()
                (directory / "shaftc").write_bytes(contents)
            older_timestamp = 1_700_000_000
            os.utime(older / "shaftc", (older_timestamp, older_timestamp))
            os.utime(selected / "shaftc", (older_timestamp + 1, older_timestamp + 1))
            self.assertEqual(installer.discover_build_directory(root, "shaftc", installer.Target("linux", "x86_64")), selected)

    def test_rejects_wrong_platform_before_installing(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            build = Path(temporary) / "artifact"
            build.mkdir()
            binary = build / "shaftc"
            binary.write_bytes(pe(0x8664))
            with self.assertRaisesRegex(installer.InstallError, "targets windows/x86_64"):
                installer.validate_binary(binary, installer.Target("linux", "x86_64"))

    def test_registers_installed_compiler_for_the_vscode_language_server(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            prefix = root / "prefix"
            config_home = root / "config"
            compiler = prefix / "bin" / "shaftc"
            compiler.parent.mkdir(parents=True)
            compiler.write_bytes(elf(62))
            (prefix / "share" / "shaft" / "std").mkdir(parents=True)
            (prefix / "share" / "shaft" / "std" / "std.shaft").write_text("// std\n", encoding="utf-8")

            registration = installer.register_language_server_compiler(
                compiler, prefix, installer.Target("linux", "x86_64"), {"XDG_CONFIG_HOME": str(config_home)}
            )
            data = __import__("json").loads(registration.read_text(encoding="utf-8"))
            self.assertEqual(data["compilerPath"], str(compiler))
            self.assertEqual(data["stdlibPath"], str(prefix / "share" / "shaft" / "std" / "std.shaft"))
            self.assertEqual(data["resourcePath"], str(prefix / "share" / "shaft"))

    def test_adds_and_removes_only_the_managed_posix_path_block(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            profile = Path(temporary) / ".profile"
            profile.write_text("export PATH=/already:$PATH\n", encoding="utf-8")
            prefix = Path(temporary) / "prefix"

            installer.update_posix_path_file(profile, prefix / "bin")
            installer.update_posix_path_file(profile, prefix / "bin")
            configured = profile.read_text(encoding="utf-8")
            self.assertEqual(configured.count(installer.PATH_BLOCK_BEGIN), 1)
            self.assertIn(f'export PATH="{prefix / "bin"}:$PATH"', configured)
            self.assertIn("export PATH=/already:$PATH", configured)

            self.assertTrue(uninstaller.remove_posix_path_block(profile))
            self.assertEqual(profile.read_text(encoding="utf-8"), "export PATH=/already:$PATH\n")

    def test_removes_managed_path_blocks_from_all_selected_profiles(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            home = Path(temporary)
            profiles = installer.posix_path_profiles(installer.Target("linux", "x86_64"), {"HOME": str(home), "SHELL": "/bin/bash"})
            for profile in profiles:
                installer.update_posix_path_file(profile, home / "prefix" / "bin")

            removed = [uninstaller.remove_posix_path_block(profile) for profile in profiles]
            self.assertTrue(all(removed))
            self.assertTrue(all(installer.PATH_BLOCK_BEGIN not in profile.read_text(encoding="utf-8") for profile in profiles))

    def test_writes_fish_syntax_for_fish_startup_files(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            profile = Path(temporary) / "shaft.fish"
            installer.update_posix_path_file(profile, Path(temporary) / "prefix" / "bin", fish=True)
            self.assertIn("set -gx PATH", profile.read_text(encoding="utf-8"))

    def test_uninstaller_removes_only_shaft_files_and_registration(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            prefix = root / "prefix"
            compiler = prefix / "bin" / "shaftc"
            resources = prefix / "share" / "shaft"
            compiler.parent.mkdir(parents=True)
            compiler.write_bytes(elf(62))
            (resources / "std").mkdir(parents=True)
            (resources / "std" / "std.shaft").write_text("// std\n", encoding="utf-8")
            unrelated = prefix / "share" / "other" / "keep.txt"
            unrelated.parent.mkdir(parents=True)
            unrelated.write_text("keep\n", encoding="utf-8")
            config_home = root / "config"
            registration = installer.register_language_server_compiler(
                compiler, prefix, installer.Target("linux", "x86_64"), {"XDG_CONFIG_HOME": str(config_home)}
            )

            removed = uninstaller.uninstall(prefix, installer.Target("linux", "x86_64"), registration)
            self.assertIn(compiler, removed)
            self.assertFalse(compiler.exists())
            self.assertFalse(resources.exists())
            self.assertFalse(registration.exists())
            self.assertTrue(unrelated.is_file())

    def test_installs_binary_and_resources_to_a_custom_prefix(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "source"
            build = root / "not-a-platform-name"
            prefix = root / "prefix"
            build.mkdir()
            source.mkdir()
            (build / "shaftc").write_bytes(elf(62))
            (source / "std" / "runtime").mkdir(parents=True)
            (source / "std" / "std.shaft").write_text("// std\n", encoding="utf-8")
            for runtime in ("linux.c", "darwin.c", "windows.c"):
                (source / "std" / "runtime" / runtime).write_text("/* runtime */\n", encoding="utf-8")

            installer.install(build, source, prefix, installer.Target("linux", "x86_64"), force=False)
            self.assertEqual((prefix / "bin" / "shaftc").read_bytes(), elf(62))
            self.assertEqual((prefix / "share" / "shaft" / "std" / "std.shaft").read_text(encoding="utf-8"), "// std\n")
            self.assertTrue((prefix / "share" / "shaft" / "std" / "runtime" / "linux.c").is_file())


if __name__ == "__main__":
    unittest.main()
