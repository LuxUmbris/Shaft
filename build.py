#!/usr/bin/env python3
import os
import sys
import subprocess
import platform

def run(cmd):
    print(">>", " ".join(cmd))
    subprocess.check_call(cmd)

OS = platform.system().lower()

if OS == "linux":
    TARGETS = {
        "x86_64": {
            "CC": "clang",
            "CXX": "clang++",
            "TRIPLE": "x86_64-linux-gnu"
        },
        "aarch64": {
            "CC": "aarch64-linux-gnu-gcc",
            "CXX": "aarch64-linux-gnu-g++",
            "TRIPLE": "aarch64-linux-gnu"
        },
        "riscv64": {
            "CC": "riscv64-linux-gnu-gcc",
            "CXX": "riscv64-linux-gnu-g++",
            "TRIPLE": "riscv64-linux-gnu"
        }
    }

elif OS == "darwin":
    TARGETS = {
        "x86_64": {
            "CC": "o64-clang",
            "CXX": "o64-clang++",
            "TRIPLE": "x86_64-apple-darwin"
        },
        "arm64": {
            "CC": "oa64-clang",
            "CXX": "oa64-clang++",
            "TRIPLE": "aarch64-apple-darwin"
        }
    }

elif OS == "windows":
    TARGETS = {
        "x86_64": {
            "CC": "x86_64-w64-mingw32-gcc",
            "CXX": "x86_64-w64-mingw32-g++",
            "TRIPLE": "x86_64-w64-mingw32"
        },
        "arm64": {
            "CC": "aarch64-w64-mingw32-gcc",
            "CXX": "aarch64-w64-mingw32-g++",
            "TRIPLE": "aarch64-w64-mingw32"
        }
    }

else:
    print(f"Unsupported OS: {OS}")
    sys.exit(1)

def build(target, mode):
    if target not in TARGETS:
        print(f"Unknown target: {target}")
        sys.exit(1)

    cfg = TARGETS[target]
    build_dir = f"build-{OS}-{target}-{mode}"
    os.makedirs(build_dir, exist_ok=True)

    cmake_cmd = [
        "cmake", "-S", ".", "-B", build_dir,
        f"-DCMAKE_BUILD_TYPE={mode.capitalize()}",
        f"-DCMAKE_C_COMPILER={cfg['CC']}",
        f"-DCMAKE_CXX_COMPILER={cfg['CXX']}",
        f"-DLLVM_DEFAULT_TARGET_TRIPLE={cfg['TRIPLE']}",
        "-DLLVM_ENABLE_PROJECTS=lld",
        "-DLLVM_TARGETS_TO_BUILD=X86;AArch64;RISCV",
        "-DLLVM_ENABLE_RTTI=ON",
        "-DLLVM_ENABLE_EH=ON",
        "-DLLVM_BUILD_LLVM_DYLIB=ON",
        "-DLLVM_LINK_LLVM_DYLIB=ON",
        "-DBUILD_SHARED_LIBS=ON"
    ]

    run(cmake_cmd)
    run(["cmake", "--build", build_dir, "-j", str(os.cpu_count())])

def main():
    if len(sys.argv) != 3:
        print("Usage: python3 build.py <target|all> <debug|release>")
        sys.exit(1)

    target = sys.argv[1]
    mode = sys.argv[2]

    if target == "all":
        for t in TARGETS:
            build(t, mode)
    else:
        build(target, mode)

if __name__ == "__main__":
    main()
