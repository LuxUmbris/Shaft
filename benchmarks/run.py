#!/usr/bin/env python3
"""Run repeatable Shaft, Clang, and (when installed) rustc benchmarks."""

import argparse
import hashlib
import json
import os
import pathlib
import platform
import shutil
import statistics
import subprocess
import sys
import tempfile
import time


ROOT = pathlib.Path(__file__).resolve().parents[1]
DEFAULT_SHAFTC = ROOT / "build" / "shaftc"


def command_version(command):
    path = shutil.which(command) if isinstance(command, str) else str(command)
    if not path or not pathlib.Path(path).is_file():
        return None
    result = subprocess.run([path, "--version"], text=True, capture_output=True, check=False)
    text = (result.stdout or result.stderr).strip()
    return {"path": path, "version": text, "status": "ok" if result.returncode == 0 else "error"}


def host_metadata():
    cpu_model = None
    cpuinfo = pathlib.Path("/proc/cpuinfo")
    if cpuinfo.is_file():
        for line in cpuinfo.read_text().splitlines():
            if line.startswith("model name"):
                cpu_model = line.split(":", 1)[1].strip()
                break
    return {
        "platform": platform.platform(),
        "machine": platform.machine(),
        "python": sys.version.split()[0],
        "cpu_model": cpu_model,
    }


def write_sources(directory, updates):
    shaft_lines = [
        "cdef __shaft_entry(i32 argc, *i8 argv) -> i32",
        "{",
        "    mut i32 value = argc;",
    ]
    c_lines = ["#include <stdint.h>", "int main(int argc, char **argv) {", "    uint32_t value = (uint32_t)argc;"]
    rust_lines = [
        "#![no_std]",
        "#![no_main]",
        "#[panic_handler]",
        "fn panic(_: &core::panic::PanicInfo) -> ! { loop {} }",
        "#[no_mangle]",
        "pub extern \"C\" fn __shaft_entry(argc: i32, _: *const *const u8) -> i32 {",
        "    let mut value = argc as u32;",
    ]
    for _ in range(updates):
        shaft_lines.append("    value = value * 1664525 + 1013904223;")
        c_lines.append("    value = value * UINT32_C(1664525) + UINT32_C(1013904223);")
        rust_lines.append("    value = value.wrapping_mul(1_664_525).wrapping_add(1_013_904_223);")
    shaft_lines.extend(["    return value;", "}", ""])
    c_lines.extend(["    return (int)(value & 255u);", "}", ""])
    rust_lines.extend(["    core::hint::black_box(value);", "    value as i32", "}", ""])

    sources = {
        "shaft": directory / "compile-scale.shaft",
        "clang": directory / "compile-scale.c",
        "rust": directory / "compile-scale.rs",
    }
    for language, path in sources.items():
        lines = {"shaft": shaft_lines, "clang": c_lines, "rust": rust_lines}[language]
        path.write_text("\n".join(lines))
    return sources


def write_runtime_sources(directory, iterations):
    shaft = directory / "runtime-xorshift.shaft"
    c = directory / "runtime-xorshift.c"
    rust = directory / "runtime-xorshift.rs"
    shaft.write_text(
        "cdec __sys_write(i32 descriptor, *i8 buffer, u64 count) -> i64;\n"
        "cdef __shaft_entry(i32 argc, *i8 argv) -> i32\n{\n"
        "    mut i32 value = argc;\n    mut i32 counter = 0;\n"
        f"    while (counter < {iterations})\n    {{\n"
        "        value = value ^ (value << 13);\n"
        "        value = value ^ (value >> 17);\n"
        "        value = value ^ (value << 5);\n"
        "        counter = counter + 1;\n    }\n"
        "    __sys_write(value, argv, 0);\n    return 0;\n}\n"
    )
    c.write_text(
        "#include <stdint.h>\n#include <unistd.h>\n"
        "int main(int argc, char **argv) {\n"
        "    uint32_t value = (uint32_t)argc;\n    uint32_t counter = 0;\n"
        f"    while (counter < {iterations}u) {{\n"
        "        value ^= value << 13;\n        value ^= value >> 17;\n        value ^= value << 5;\n"
        "        counter++;\n    }\n"
        "    write((int)value, argv, 0);\n    return 0;\n}\n"
    )
    rust.write_text(
        "fn main() {\n"
        "    let mut value = std::hint::black_box(std::env::args().count() as u32);\n"
        f"    for _ in 0..{iterations} {{\n"
        "        value ^= value << 13;\n        value ^= value >> 17;\n        value ^= value << 5;\n"
        "    }\n"
        "    std::hint::black_box(value);\n}\n"
    )
    return {"shaft": shaft, "clang": c, "rust": rust}


def source_metadata(path):
    content = path.read_bytes()
    return {"path": path.name, "bytes": len(content), "sha256": hashlib.sha256(content).hexdigest()}


def summarize_samples(command, warmups, durations):
    return {
        "command": command,
        "warmups": warmups,
        "samples_ms": durations,
        "median_ms": statistics.median(durations),
        "mean_ms": statistics.fmean(durations),
        "minimum_ms": min(durations),
        "maximum_ms": max(durations),
    }


def run_timed(command, iterations, warmups=1):
    for _ in range(warmups):
        warmup = subprocess.run(command, text=True, capture_output=True, check=False)
        if warmup.returncode:
            raise RuntimeError(f"command failed: {' '.join(command)}\n{warmup.stderr}")
    durations = []
    for _ in range(iterations):
        start = time.perf_counter_ns()
        result = subprocess.run(command, text=True, capture_output=True, check=False)
        elapsed = (time.perf_counter_ns() - start) / 1_000_000
        if result.returncode:
            raise RuntimeError(f"command failed: {' '.join(command)}\n{result.stderr}")
        durations.append(elapsed)
    return summarize_samples(command, warmups, durations)


def pin_process_to_one_cpu(enabled):
    if not enabled or not hasattr(os, "sched_getaffinity") or not hasattr(os, "sched_setaffinity"):
        return {"status": "not-pinned", "reason": "affinity disabled or unsupported"}
    try:
        available = sorted(os.sched_getaffinity(0))
        if not available:
            return {"status": "not-pinned", "reason": "no eligible CPUs"}
        cpu = available[0]
        os.sched_setaffinity(0, {cpu})
        return {"status": "pinned", "cpu": cpu, "eligible_cpus": available}
    except OSError as error:
        return {"status": "not-pinned", "reason": str(error)}


def run_executables_interleaved(paths, iterations, warmups=1):
    names = sorted(paths)
    for name in names:
        for _ in range(warmups):
            subprocess.run([str(paths[name])], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
    samples = {name: [] for name in names}
    order = []
    for round_index in range(iterations):
        round_names = names[round_index % len(names):] + names[:round_index % len(names)]
        for name in round_names:
            start = time.perf_counter_ns()
            result = subprocess.run([str(paths[name])], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)
            elapsed = (time.perf_counter_ns() - start) / 1_000_000
            if result.returncode:
                raise RuntimeError(f"runtime benchmark failed: {paths[name]}, exit={result.returncode}")
            samples[name].append(elapsed)
            order.append(name)
    return {
        name: {**summarize_samples([str(paths[name])], warmups, samples[name]), "binary_bytes": paths[name].stat().st_size}
        for name in names
    }, order


def unavailable(reason):
    return {"status": "unavailable", "reason": reason}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--shaftc", default=str(DEFAULT_SHAFTC))
    parser.add_argument("--iterations", type=int, default=7)
    parser.add_argument("--updates", type=int, default=10_000)
    parser.add_argument("--runtime-iterations", type=int, default=100_000_000)
    parser.add_argument("--skip-runtime", action="store_true")
    parser.add_argument("--no-pin", action="store_true", help="do not pin benchmark children to one CPU")
    parser.add_argument("--output", type=pathlib.Path, required=True)
    args = parser.parse_args()
    if args.iterations < 1 or args.updates < 1 or args.runtime_iterations < 1:
        parser.error("iteration counts must be positive")

    shaftc = pathlib.Path(args.shaftc).resolve()
    if not shaftc.is_file():
        parser.error(f"shaft compiler is not a file: {shaftc}")
    clang = shutil.which("clang")
    rustc = shutil.which("rustc")
    affinity = pin_process_to_one_cpu(not args.no_pin)
    report = {
        "schema_version": 2,
        "host": host_metadata(),
        "tools": {"shaft": command_version(shaftc), "clang": command_version("clang"), "rust": command_version("rustc")},
        "protocol": {
            "warmups": 1,
            "iterations": args.iterations,
            "affinity": affinity,
            "runtime_sample_order": "round-robin interleaved",
            "shaft_flags": ["--no-std", "--native", "-O2"],
            "clang_flags": ["-O2", "-march=native"],
            "rust_flags": ["-C", "opt-level=2", "-C", "target-cpu=native", "-C", "panic=abort"],
        },
        "benchmarks": {},
    }

    with tempfile.TemporaryDirectory(prefix="shaft-bench-") as temp:
        directory = pathlib.Path(temp)
        sources = write_sources(directory, args.updates)
        scale = {"sources": {name: source_metadata(path) for name, path in sources.items()}, "implementations": {}}
        commands = {
            "shaft": [str(shaftc), "--no-std", "--native", "-O2", "--emit", "llvm", "-o", str(directory / "scale-shaft.ll"), str(sources["shaft"])],
            "clang": [clang, "-O2", "-march=native", "-S", "-emit-llvm", "-o", str(directory / "scale-clang.ll"), str(sources["clang"])],
        }
        for name, command in commands.items():
            scale["implementations"][name] = {"status": "ok", **run_timed(command, args.iterations)}
        if rustc:
            command = [rustc, "-C", "opt-level=2", "-C", "target-cpu=native", "-C", "panic=abort", "--emit=llvm-ir", "-o", str(directory / "scale-rust.ll"), str(sources["rust"])]
            scale["implementations"]["rust"] = {"status": "ok", **run_timed(command, args.iterations)}
        else:
            scale["implementations"]["rust"] = unavailable("rustc was not found on PATH")
        report["benchmarks"]["compile_scale"] = scale

        if not args.skip_runtime:
            runtime_sources = write_runtime_sources(directory, args.runtime_iterations)
            runtime = {"sources": {name: source_metadata(path) for name, path in runtime_sources.items()}, "implementations": {}}
            output_paths = {name: directory / f"runtime-{name}" for name in runtime_sources}
            runtime_build = {"sources": {name: source_metadata(path) for name, path in runtime_sources.items()}, "implementations": {}}
            build_commands = {
                "shaft": [str(shaftc), "--no-std", "--native", "-O2", "-o", str(output_paths["shaft"]), str(runtime_sources["shaft"])],
                "clang": [clang, "-O2", "-march=native", "-o", str(output_paths["clang"]), str(runtime_sources["clang"])],
            }
            for name, command in build_commands.items():
                runtime_build["implementations"][name] = {"status": "ok", **run_timed(command, args.iterations)}
            if rustc:
                command = [rustc, "-C", "opt-level=2", "-C", "target-cpu=native", "-C", "panic=abort", "-o", str(output_paths["rust"]), str(runtime_sources["rust"])]
                runtime_build["implementations"]["rust"] = {"status": "ok", **run_timed(command, args.iterations)}
            else:
                runtime_build["implementations"]["rust"] = unavailable("rustc was not found on PATH")
            report["benchmarks"]["runtime_build"] = runtime_build

            available_paths = {name: output_paths[name] for name, entry in runtime_build["implementations"].items() if entry["status"] == "ok"}
            runtime_samples, sample_order = run_executables_interleaved(available_paths, args.iterations)
            runtime["implementations"] = {name: {"status": "ok", **result} for name, result in runtime_samples.items()}
            for name, entry in runtime_build["implementations"].items():
                if entry["status"] != "ok":
                    runtime["implementations"][name] = entry
            runtime["sample_order"] = sample_order
            report["benchmarks"]["runtime_xorshift"] = runtime

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
    print(args.output)


if __name__ == "__main__":
    main()
