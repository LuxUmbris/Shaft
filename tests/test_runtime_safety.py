import os
import pathlib
import shutil
import subprocess
import tempfile
import unittest


REPOSITORY = pathlib.Path(__file__).resolve().parents[1]
COMPILER = pathlib.Path(os.environ.get("SHAFTC", REPOSITORY / "build" / "shaftc"))
CLANG = shutil.which("clang")


@unittest.skipUnless(os.name == "posix" and CLANG and COMPILER.is_file(), "requires Linux clang and shaftc")
class RuntimeSafetyTests(unittest.TestCase):
    def test_linux_runtime_has_a_valid_noreturn_exit_contract(self):
        result = subprocess.run(
            [
                CLANG,
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-Wno-unused-function",
                "-fsyntax-only",
                str(REPOSITORY / "std" / "runtime" / "linux.c"),
            ],
            check=False,
            text=True,
            capture_output=True,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_allocator_rejects_size_max_without_wrapping(self):
        with tempfile.TemporaryDirectory() as directory:
            work = pathlib.Path(directory)
            source = work / "allocator-overflow.c"
            binary = work / "allocator-overflow"
            source.write_text(
                "#include <stdint.h>\n"
                "#include <stddef.h>\n"
                "int __shaft_entry(int argc, char **argv) { return 0; }\n"
                "void *__shaft_alloc(size_t);\n"
                "int main(void) { return __shaft_alloc(SIZE_MAX) != NULL; }\n"
            )
            subprocess.run(
                [
                    CLANG,
                    "-D_start=shaft_runtime_start",
                    "-Wno-main-return-type",
                    "-O0",
                    "-ffreestanding",
                    str(REPOSITORY / "std" / "runtime" / "linux.c"),
                    str(source),
                    "-o",
                    str(binary),
                ],
                check=True,
                text=True,
                capture_output=True,
            )
            result = subprocess.run([str(binary)], check=False)
            self.assertEqual(result.returncode, 0)

    def test_entry_bridge_exits_cleanly_when_argv_exhausts_heap(self):
        with tempfile.TemporaryDirectory() as directory:
            work = pathlib.Path(directory)
            source = work / "entry.shaft"
            binary = work / "entry"
            source.write_text("def main(String[] args)\n{\n}\n")
            compiled = subprocess.run([str(COMPILER), "-o", str(binary), str(source)], text=True, capture_output=True)
            self.assertEqual(compiled.returncode, 0, compiled.stderr)
            executed = subprocess.run([str(binary)] + ["x"] * 43_690, check=False, timeout=30)
            self.assertEqual(executed.returncode, 70)
    def test_allocator_reclaims_released_blocks(self):
        with tempfile.TemporaryDirectory() as directory:
            work = pathlib.Path(directory)
            source = work / "allocator-free.c"
            binary = work / "allocator-free"
            source.write_text(
                "#include <stddef.h>\n"
                "int __shaft_entry(int argc, char **argv) { return 0; }\n"
                "void *__shaft_alloc(size_t);\n"
                "void __shaft_free(void *);\n"
                "int main(void) {\n"
                "  void *first = __shaft_alloc(900000);\n"
                "  if (!first) return 1;\n"
                "  __shaft_free(first);\n"
                "  return __shaft_alloc(900000) ? 0 : 2;\n"
                "}\n"
            )
            subprocess.run(
                [
                    CLANG, "-D_start=shaft_runtime_start", "-Wno-main-return-type", "-O0", "-ffreestanding",
                    str(REPOSITORY / "std" / "runtime" / "linux.c"), str(source), "-o", str(binary),
                ],
                check=True, text=True, capture_output=True,
            )
            self.assertEqual(subprocess.run([str(binary)], check=False).returncode, 0)
    def test_allocator_retains_shared_allocation_until_last_release(self):
        with tempfile.TemporaryDirectory() as directory:
            work = pathlib.Path(directory)
            source = work / "allocator-retain.c"
            binary = work / "allocator-retain"
            source.write_text(
                "#include <stddef.h>\n"
                "int __shaft_entry(int argc, char **argv) { return 0; }\n"
                "void *__shaft_alloc(size_t);\n"
                "void __shaft_retain(void *);\n"
                "void __shaft_free(void *);\n"
                "int main(void) {\n"
                "  void *value = __shaft_alloc(900000);\n"
                "  if (!value) return 1;\n"
                "  __shaft_retain(value);\n"
                "  __shaft_free(value);\n"
                "  if (__shaft_alloc(900000)) return 2;\n"
                "  __shaft_free(value);\n"
                "  return __shaft_alloc(900000) ? 0 : 3;\n"
                "}\n"
            )
            subprocess.run(
                [
                    CLANG, "-D_start=shaft_runtime_start", "-Wno-main-return-type", "-O0", "-ffreestanding",
                    str(REPOSITORY / "std" / "runtime" / "linux.c"), str(source), "-o", str(binary),
                ],
                check=True, text=True, capture_output=True,
            )
            self.assertEqual(subprocess.run([str(binary)], check=False).returncode, 0)


if __name__ == "__main__":
    unittest.main()
