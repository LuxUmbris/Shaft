#!/usr/bin/env python3
"""Benchmark the Computer Language Benchmarks Game mandelbrot kernel."""

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
BENCHMARK_FAMILY = "Computer Language Benchmarks Game"
BENCHMARK_NAME = "mandelbrot"


def default_shaftc():
    configured = os.environ.get("SHAFTC")
    if configured:
        return pathlib.Path(configured)
    candidates = [ROOT / "build" / "shaftc", *sorted(ROOT.glob("build*/shaftc"), key=lambda path: path.stat().st_mtime, reverse=True)]
    return next((path for path in candidates if path.is_file()), ROOT / "build" / "shaftc")


DEFAULT_SHAFTC = default_shaftc()


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


def mandelbrot_exit_code(size, max_iterations):
    """Return the escaped-iteration checksum used to prevent dead-code removal."""
    checksum = 0
    for y in range(size):
        for x in range(size):
            cr = 2.0 * x / size - 1.5
            ci = 2.0 * y / size - 1.0
            zr = 0.0
            zi = 0.0
            iteration = 0
            while iteration < max_iterations and zr * zr + zi * zi <= 4.0:
                zr, zi = zr * zr - zi * zi + cr, 2.0 * zr * zi + ci
                iteration += 1
            checksum += iteration
    return checksum & 255


def write_mandelbrot_sources(directory, size, max_iterations):
    shaft = directory / "mandelbrot.shaft"
    c = directory / "mandelbrot.c"
    rust = directory / "mandelbrot.rs"
    shaft.write_text(
        "def main(String[] args)\n"
        "{\n"
        "    mut i32 checksum = 0;\n"
        "    mut i32 y = 0;\n"
        f"    while (y < {size})\n"
        "    {\n"
        "        mut i32 x = 0;\n"
        f"        while (x < {size})\n"
        "        {\n"
        f"            f64 cr = 2.0 * x / {size}.0 - 1.5;\n"
        f"            f64 ci = 2.0 * y / {size}.0 - 1.0;\n"
        "            mut f64 zr = 0.0;\n"
        "            mut f64 zi = 0.0;\n"
        "            mut i32 iteration = 0;\n"
        f"            while (iteration < {max_iterations} && zr * zr + zi * zi <= 4.0)\n"
        "            {\n"
        "                f64 next = zr * zr - zi * zi + cr;\n"
        "                zi = 2.0 * zr * zi + ci;\n"
        "                zr = next;\n"
        "                iteration = iteration + 1;\n"
        "            }\n"
        "            checksum = checksum + iteration;\n"
        "            x = x + 1;\n"
        "        }\n"
        "        y = y + 1;\n"
        "    }\n"
        "    exit(checksum & 255);\n"
        "}\n",
        encoding="utf-8",
    )
    c.write_text(
        "int main(void)\n"
        "{\n"
        "    int checksum = 0;\n"
        f"    for (int y = 0; y < {size}; ++y) {{\n"
        f"        for (int x = 0; x < {size}; ++x) {{\n"
        f"            double cr = 2.0 * x / {size}.0 - 1.5;\n"
        f"            double ci = 2.0 * y / {size}.0 - 1.0;\n"
        "            double zr = 0.0;\n"
        "            double zi = 0.0;\n"
        "            int iteration = 0;\n"
        f"            while (iteration < {max_iterations} && zr * zr + zi * zi <= 4.0) {{\n"
        "                double next = zr * zr - zi * zi + cr;\n"
        "                zi = 2.0 * zr * zi + ci;\n"
        "                zr = next;\n"
        "                ++iteration;\n"
        "            }\n"
        "            checksum += iteration;\n"
        "        }\n"
        "    }\n"
        "    return checksum & 255;\n"
        "}\n",
        encoding="utf-8",
    )
    rust.write_text(
        "fn main() {\n"
        "    let mut checksum: i32 = 0;\n"
        f"    for y in 0..{size} {{\n"
        f"        for x in 0..{size} {{\n"
        f"            let cr = 2.0 * x as f64 / {size}.0 - 1.5;\n"
        f"            let ci = 2.0 * y as f64 / {size}.0 - 1.0;\n"
        "            let mut zr = 0.0;\n"
        "            let mut zi = 0.0;\n"
        "            let mut iteration = 0;\n"
        f"            while iteration < {max_iterations} && zr * zr + zi * zi <= 4.0 {{\n"
        "                let next = zr * zr - zi * zi + cr;\n"
        "                zi = 2.0 * zr * zi + ci;\n"
        "                zr = next;\n"
        "                iteration += 1;\n"
        "            }\n"
        "            checksum += iteration;\n"
        "        }\n"
        "    }\n"
        "    std::process::exit(checksum & 255);\n"
        "}\n",
        encoding="utf-8",
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


def run_executables_interleaved(paths, expected_exit_code, iterations, warmups=1):
    names = sorted(paths)
    exit_codes = {}
    for name in names:
        for _ in range(warmups):
            result = subprocess.run([str(paths[name])], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)
            if result.returncode != expected_exit_code:
                raise RuntimeError(
                    f"mandelbrot correctness check failed: {paths[name]}, "
                    f"exit={result.returncode}, expected={expected_exit_code}"
                )
            exit_codes[name] = result.returncode
    samples = {name: [] for name in names}
    order = []
    for round_index in range(iterations):
        round_names = names[round_index % len(names):] + names[:round_index % len(names)]
        for name in round_names:
            start = time.perf_counter_ns()
            result = subprocess.run([str(paths[name])], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)
            elapsed = (time.perf_counter_ns() - start) / 1_000_000
            if result.returncode != expected_exit_code:
                raise RuntimeError(
                    f"mandelbrot runtime failed: {paths[name]}, "
                    f"exit={result.returncode}, expected={expected_exit_code}"
                )
            samples[name].append(elapsed)
            order.append(name)
    return {
        name: {
            **summarize_samples([str(paths[name])], warmups, samples[name]),
            "binary_bytes": paths[name].stat().st_size,
            "exit_code": exit_codes[name],
        }
        for name in names
    }, order


def unavailable(reason):
    return {"status": "unavailable", "reason": reason}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--shaftc", default=str(DEFAULT_SHAFTC))
    parser.add_argument("--iterations", type=int, default=7)
    parser.add_argument("--size", type=int, default=512, help="square image width used by the mandelbrot kernel")
    parser.add_argument("--max-iterations", type=int, default=50, help="escape iterations per pixel")
    parser.add_argument("--skip-runtime", action="store_true")
    parser.add_argument("--no-pin", action="store_true", help="do not pin benchmark children to one CPU")
    parser.add_argument("--output", type=pathlib.Path, required=True)
    args = parser.parse_args()
    if args.iterations < 1 or args.size < 1 or args.max_iterations < 1:
        parser.error("iteration counts and mandelbrot parameters must be positive")

    shaftc = pathlib.Path(args.shaftc).resolve()
    if not shaftc.is_file():
        parser.error(f"shaft compiler is not a file: {shaftc}")
    clang = shutil.which("clang")
    if not clang:
        parser.error("clang was not found on PATH")
    rustc = shutil.which("rustc")
    expected_exit_code = mandelbrot_exit_code(args.size, args.max_iterations)
    affinity = pin_process_to_one_cpu(not args.no_pin)
    report = {
        "schema_version": 3,
        "host": host_metadata(),
        "tools": {"shaft": command_version(shaftc), "clang": command_version("clang"), "rust": command_version("rustc")},
        "protocol": {
            "warmups": 1,
            "iterations": args.iterations,
            "affinity": affinity,
            "runtime_sample_order": "round-robin interleaved",
            "shaft_flags": ["-O2"],
            "clang_flags": ["-O2", "-march=native"],
            "rust_flags": ["-C", "opt-level=2", "-C", "target-cpu=native", "-C", "panic=abort"],
        },
        "benchmarks": {},
    }

    with tempfile.TemporaryDirectory(prefix="shaft-bench-") as temp:
        directory = pathlib.Path(temp)
        sources = write_mandelbrot_sources(directory, args.size, args.max_iterations)
        mandelbrot = {
            "family": BENCHMARK_FAMILY,
            "name": BENCHMARK_NAME,
            "variant": "escape-count kernel with a checked exit-code checksum",
            "parameters": {"size": args.size, "max_iterations": args.max_iterations},
            "expected_exit_code": expected_exit_code,
            "sources": {name: source_metadata(path) for name, path in sources.items()},
            "compile": {"implementations": {}},
        }
        compile_commands = {
            "shaft": [str(shaftc), "-O2", "--emit", "llvm", "-o", str(directory / "mandelbrot-shaft.ll"), str(sources["shaft"])],
            "clang": [clang, "-O2", "-march=native", "-S", "-emit-llvm", "-o", str(directory / "mandelbrot-clang.ll"), str(sources["clang"])],
        }
        for name, command in compile_commands.items():
            mandelbrot["compile"]["implementations"][name] = {"status": "ok", **run_timed(command, args.iterations)}
        if rustc:
            command = [rustc, "-C", "opt-level=2", "-C", "target-cpu=native", "-C", "panic=abort", "--emit=llvm-ir", "-o", str(directory / "mandelbrot-rust.ll"), str(sources["rust"])]
            mandelbrot["compile"]["implementations"]["rust"] = {"status": "ok", **run_timed(command, args.iterations)}
        else:
            mandelbrot["compile"]["implementations"]["rust"] = unavailable("rustc was not found on PATH")

        if not args.skip_runtime:
            output_paths = {name: directory / f"mandelbrot-{name}" for name in sources}
            mandelbrot["build"] = {"implementations": {}}
            build_commands = {
                "shaft": [str(shaftc), "-O2", "-o", str(output_paths["shaft"]), str(sources["shaft"])],
                "clang": [clang, "-O2", "-march=native", "-o", str(output_paths["clang"]), str(sources["clang"])],
            }
            for name, command in build_commands.items():
                mandelbrot["build"]["implementations"][name] = {"status": "ok", **run_timed(command, args.iterations)}
            if rustc:
                command = [rustc, "-C", "opt-level=2", "-C", "target-cpu=native", "-C", "panic=abort", "-o", str(output_paths["rust"]), str(sources["rust"])]
                mandelbrot["build"]["implementations"]["rust"] = {"status": "ok", **run_timed(command, args.iterations)}
            else:
                mandelbrot["build"]["implementations"]["rust"] = unavailable("rustc was not found on PATH")
            available_paths = {
                name: output_paths[name]
                for name, entry in mandelbrot["build"]["implementations"].items()
                if entry["status"] == "ok"
            }
            runtime_samples, sample_order = run_executables_interleaved(
                available_paths, expected_exit_code, args.iterations
            )
            mandelbrot["runtime"] = {
                "implementations": {name: {"status": "ok", **result} for name, result in runtime_samples.items()},
                "sample_order": sample_order,
            }
            for name, entry in mandelbrot["build"]["implementations"].items():
                if entry["status"] != "ok":
                    mandelbrot["runtime"]["implementations"][name] = entry

        report["benchmarks"][BENCHMARK_NAME] = mandelbrot

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
    print(args.output)


if __name__ == "__main__":
    main()
