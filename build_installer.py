#!/usr/bin/env python3
"""Build a relocatable Shaft compiler package for the current host OS."""

from __future__ import annotations

import argparse
import platform
import shutil
import subprocess
import tarfile
from pathlib import Path
from zipfile import ZIP_DEFLATED, ZipFile

ROOT = Path(__file__).resolve().parent


def run(command: list[str]) -> None:
    print("+", " ".join(command))
    subprocess.run(command, check=True)


def archive_directory(package_root: Path, output_dir: Path, system: str, machine: str) -> Path:
    archive_base = output_dir / f"shaftc-{system.lower()}-{machine.lower()}"
    if system == "Windows":
        archive = archive_base.with_suffix(".zip")
        with ZipFile(archive, "w", ZIP_DEFLATED) as zip_file:
            for file_path in package_root.rglob("*"):
                if file_path.is_file():
                    zip_file.write(file_path, file_path.relative_to(package_root.parent))
        return archive

    archive = archive_base.with_suffix(".tar.gz")
    with tarfile.open(archive, "w:gz") as tar_file:
        tar_file.add(package_root, arcname=package_root.name)
    return archive


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=ROOT / "dist")
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build-installer")
    parser.add_argument("--debug", action="store_true")
    parser.add_argument("--keep-stage", action="store_true")
    arguments = parser.parse_args()

    system = platform.system()
    if system not in {"Linux", "Darwin", "Windows"}:
        raise SystemExit(f"unsupported host OS: {system}")

    build_type = "Debug" if arguments.debug else "Release"
    build_dir = arguments.build_dir.resolve()
    stage_dir = build_dir / "stage"
    package_root = stage_dir / "shaftc"
    arguments.output.mkdir(parents=True, exist_ok=True)

    run([
        "cmake", "-S", str(ROOT), "-B", str(build_dir),
        f"-DCMAKE_BUILD_TYPE={build_type}",
        "-DBUILD_TESTING=OFF",
    ])
    run(["cmake", "--build", str(build_dir), "--config", build_type, "--parallel"])
    shutil.rmtree(stage_dir, ignore_errors=True)
    run(["cmake", "--install", str(build_dir), "--config", build_type, "--prefix", str(package_root)])

    binary = package_root / "bin" / ("shaftc.exe" if system == "Windows" else "shaftc")
    stdlib = package_root / "share" / "shaft" / "std" / "std.shaft"
    runtimes = [
        package_root / "share" / "shaft" / "std" / "runtime" / "linux.c",
        package_root / "share" / "shaft" / "std" / "runtime" / "darwin.c",
        package_root / "share" / "shaft" / "std" / "runtime" / "windows.c",
    ]
    required = [binary, stdlib, *runtimes]
    missing = [str(path) for path in required if not path.is_file()]
    if missing:
        raise SystemExit("installer staging is incomplete: " + ", ".join(missing))

    archive = archive_directory(package_root, arguments.output.resolve(), system, platform.machine())
    print(f"created {archive}")
    if not arguments.keep_stage:
        shutil.rmtree(stage_dir)


if __name__ == "__main__":
    main()
