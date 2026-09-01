import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


REPOSITORY = Path(__file__).resolve().parents[1]


class CompilerSanitizerTests(unittest.TestCase):
    def test_successful_llvm_emission_has_no_sanitizer_leaks(self):
        if os.name != "posix" or not shutil.which("cmake"):
            self.skipTest("requires CMake on a POSIX host")

        with tempfile.TemporaryDirectory(prefix="shaftc-sanitize-") as directory:
            work = Path(directory)
            build = work / "build"
            source = work / "program.shaft"
            output = work / "program.ll"
            source.write_text(
                "cdef __shaft_entry(i32 argc, *i8 argv) -> i32\n"
                "{\n"
                "    i32 value = 40;\n"
                "    return value + 2;\n"
                "}\n",
                encoding="utf-8",
            )
            sanitizer = "-fsanitize=address,undefined -fno-omit-frame-pointer"
            subprocess.run(
                [
                    "cmake",
                    "-S",
                    str(REPOSITORY),
                    "-B",
                    str(build),
                    "-DCMAKE_BUILD_TYPE=Debug",
                    f"-DCMAKE_CXX_FLAGS={sanitizer}",
                    f"-DCMAKE_EXE_LINKER_FLAGS={sanitizer}",
                ],
                check=True,
                capture_output=True,
                text=True,
            )
            subprocess.run(
                ["cmake", "--build", str(build), "--parallel"],
                check=True,
                capture_output=True,
                text=True,
            )
            result = subprocess.run(
                [str(build / "shaftc"), "--no-std", "--emit", "llvm", "-o", str(output), str(source)],
                env={**os.environ, "ASAN_OPTIONS": "detect_leaks=1"},
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertTrue(output.is_file())


if __name__ == "__main__":
    unittest.main()
