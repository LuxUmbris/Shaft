import os
import pathlib
import subprocess
import tempfile
import unittest


REPOSITORY = pathlib.Path(__file__).resolve().parents[1]
COMPILER = pathlib.Path(os.environ.get("SHAFTC", REPOSITORY / "build" / "shaftc"))
RUNTIME = REPOSITORY / "std" / "runtime" / "linux.shaft"


@unittest.skipUnless(os.name == "posix" and COMPILER.is_file(), "requires a Linux shaftc build")
class RuntimeSafetyTests(unittest.TestCase):
    def test_linux_runtime_source_type_checks_and_emits_an_object(self):
        with tempfile.TemporaryDirectory() as directory:
            output = pathlib.Path(directory) / "runtime.o"
            result = subprocess.run(
                [str(COMPILER), "--no-std", "--emit", "object", "-o", str(output), str(RUNTIME)],
                check=False,
                text=True,
                capture_output=True,
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertTrue(output.is_file())

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

    def test_standard_program_runs_against_the_source_runtime(self):
        with tempfile.TemporaryDirectory() as directory:
            work = pathlib.Path(directory)
            source = work / "program.shaft"
            binary = work / "program"
            source.write_text("def main()\n{\n}\n")
            compiled = subprocess.run([str(COMPILER), "-o", str(binary), str(source)], text=True, capture_output=True)
            self.assertEqual(compiled.returncode, 0, compiled.stderr)
            self.assertEqual(subprocess.run([str(binary)], check=False).returncode, 0)


if __name__ == "__main__":
    unittest.main()
