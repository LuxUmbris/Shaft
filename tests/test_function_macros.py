import subprocess
import tempfile
import unittest
from pathlib import Path


REPOSITORY = Path(__file__).resolve().parents[1]
SHAFTC = REPOSITORY / "build" / "shaftc"


@unittest.skipUnless(SHAFTC.is_file(), "build shaftc before running this test")
class FunctionMacroTests(unittest.TestCase):
    def compile_source(self, directory, source_text, *arguments):
        source = Path(directory) / "program.shaft"
        source.write_text(source_text, encoding="utf-8")
        return subprocess.run(
            [str(SHAFTC), *arguments, str(source)],
            capture_output=True,
            text=True,
            check=False,
        )

    def test_function_macro_substitutes_nested_argument_tokens(self):
        with tempfile.TemporaryDirectory(prefix="shaftc-function-macro-") as directory:
            binary = Path(directory) / "program"
            result = self.compile_source(
                directory,
                "using call!(callee, value) -> callee(value);\n"
                "cdef increment(i32 value) -> i32\n"
                "{\n"
                "    return value + 1;\n"
                "}\n"
                "cdef __shaft_entry(i32 argc, *i8 argv) -> i32\n"
                "{\n"
                "    return call!(increment, (argc + 40));\n"
                "}\n",
                "--no-std",
                "-o",
                str(binary),
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual(subprocess.run([str(binary)], check=False).returncode, 42)

    def test_function_macro_overloads_select_by_arity(self):
        with tempfile.TemporaryDirectory(prefix="shaftc-function-macro-overload-") as directory:
            binary = Path(directory) / "program"
            result = self.compile_source(
                directory,
                "using choose!(value) -> value;\n"
                "using choose!(left, right) -> left + right;\n"
                "cdef __shaft_entry(i32 argc, *i8 argv) -> i32\n"
                "{\n"
                "    return choose!(40, 2);\n"
                "}\n",
                "--no-std",
                "-o",
                str(binary),
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual(subprocess.run([str(binary)], check=False).returncode, 42)

    def test_function_macro_recursion_is_rejected_at_declaration_time(self):
        with tempfile.TemporaryDirectory(prefix="shaftc-function-macro-recursion-") as directory:
            result = self.compile_source(
                directory,
                "using recurse!() -> recurse!();\n"
                "cdef __shaft_entry(i32 argc, *i8 argv) -> i32\n"
                "{\n"
                "    return recurse!();\n"
                "}\n",
                "--no-std",
                "--check-only",
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("recursive function-like using macro alias 'recurse'", result.stderr)

    def test_std_global_output_macros_cross_the_module_boundary(self):
        with tempfile.TemporaryDirectory(prefix="shaftc-global-std-macros-") as directory:
            binary = Path(directory) / "program"
            result = self.compile_source(
                directory,
                "def main()\n"
                "{\n"
                "    mut *str values = __shaft_alloc(16);\n"
                "    values[0].data = \"world\";\n"
                "    values[0].length = 5;\n"
                "    reserve i64 first = print!(\"prefix \");\n"
                "    reserve i64 second = printf(\"hello, {str}\", values[0]);\n"
                "    reserve i64 third = println!(\"!\");\n"
                "    reserve i64 fourth = printf(\"again, {str}\\n\", values[0]);\n"
                "}\n",
                "-o",
                str(binary),
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            execution = subprocess.run([str(binary)], capture_output=True, check=False)
            self.assertEqual(execution.returncode, 0, execution.stderr)
            self.assertEqual(execution.stdout, b"prefix hello, world!\nagain, world\n")

    def test_std_output_functions_remain_available_alongside_global_std_macros(self):
        with tempfile.TemporaryDirectory(prefix="shaftc-std-macros-") as directory:
            binary = Path(directory) / "program"
            result = self.compile_source(
                directory,
                "def main(String[] args)\n"
                "{\n"
                "    mut *str values = __shaft_alloc(16);\n"
                "    values[0].data = \"world\";\n"
                "    values[0].length = 5;\n"
                "    reserve i64 formatted = printf(\"prefix \");\n"
                "    reserve i64 line = printf(\"hello, {str}\\n\", values[0]);\n"
                "    reserve i64 plain = printf(\"done\\n\");\n"
                "}\n",
                "-o",
                str(binary),
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            execution = subprocess.run([str(binary)], capture_output=True, check=False)
            self.assertEqual(execution.returncode, 0, execution.stderr)
            self.assertEqual(execution.stdout, b"prefix hello, world\ndone\n")

    def test_private_std_macro_does_not_cross_the_module_boundary(self):
        with tempfile.TemporaryDirectory(prefix="shaftc-private-using-") as directory:
            stdlib = Path(directory) / "private-stdlib.shaft"
            stdlib.write_text("using increment!(value) -> value + 1;\n", encoding="utf-8")
            result = self.compile_source(
                directory,
                "cdef __shaft_entry(i32 argc, *i8 argv) -> i32\n"
                "{\n"
                "    return increment!(41);\n"
                "}\n",
                "--std",
                str(stdlib),
                "--check-only",
            )
            self.assertNotEqual(result.returncode, 0)

    def test_global_using_macro_crosses_the_module_boundary(self):
        with tempfile.TemporaryDirectory(prefix="shaftc-global-using-") as directory:
            stdlib = Path(directory) / "global-stdlib.shaft"
            binary = Path(directory) / "program"
            stdlib.write_text("using global increment!(value) -> value + 1;\n", encoding="utf-8")
            result = self.compile_source(
                directory,
                "cdef __shaft_entry(i32 argc, *i8 argv) -> i32\n"
                "{\n"
                "    return increment!(41);\n"
                "}\n",
                "--std",
                str(stdlib),
                "-o",
                str(binary),
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual(subprocess.run([str(binary)], check=False).returncode, 42)

    def test_async_def_is_a_cooperative_callable_and_asyc_is_not_a_keyword(self):
        with tempfile.TemporaryDirectory(prefix="shaftc-async-keyword-") as directory:
            valid = self.compile_source(
                directory,
                "def async worker(i32 value) -> i32 result\n"
                "{\n"
                "    tunnel value -> i32 result;\n"
                "}\n",
                "--no-std",
                "--check-only",
            )
            self.assertEqual(valid.returncode, 0, valid.stdout + valid.stderr)

            legacy = self.compile_source(
                directory,
                "def asyc worker(i32 value) -> i32 result\n"
                "{\n"
                "    tunnel value -> i32 result;\n"
                "}\n",
                "--no-std",
                "--check-only",
            )
            self.assertNotEqual(legacy.returncode, 0)
            self.assertIn("Expected '(' after function name", legacy.stderr)


if __name__ == "__main__":
    unittest.main()
