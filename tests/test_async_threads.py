import subprocess
import tempfile
import unittest
from pathlib import Path


REPOSITORY = Path(__file__).resolve().parents[1]
SHAFTC = REPOSITORY / "build" / "shaftc"


@unittest.skipUnless(SHAFTC.is_file(), "build shaftc before running this test")
class AsyncThreadTests(unittest.TestCase):
    def test_state_start_await_and_named_task_execute_natively(self):
        with tempfile.TemporaryDirectory(prefix="shaftc-async-thread-") as directory:
            source = Path(directory) / "async-thread.shaft"
            binary = Path(directory) / "async-thread"
            source.write_text(
                "def async bump(*i32 value)\n"
                "{\n"
                "    *value = 41;\n"
                "}\n"
                "cdef __shaft_entry(i32 argc, *i8 argv) -> i32\n"
                "{\n"
                "    mut i32 result = 0;\n"
                "    State bump(&result) state;\n"
                "    start state;\n"
                "    await state;\n"
                "    worker\n"
                "    {\n"
                "        result = result + 1;\n"
                "    }\n"
                "    return result;\n"
                "}\n",
                encoding="utf-8",
            )
            compilation = subprocess.run(
                [str(SHAFTC), "--no-std", "-o", str(binary), str(source)],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(compilation.returncode, 0, compilation.stdout + compilation.stderr)
            execution = subprocess.run([str(binary)], capture_output=True, check=False)
            self.assertEqual(execution.returncode, 42, execution.stderr)


    def test_thread_binding_is_a_cooperative_state_handle(self):
        with tempfile.TemporaryDirectory(prefix="shaftc-thread-") as directory:
            source = Path(directory) / "thread.shaft"
            binary = Path(directory) / "thread"
            source.write_text(
                "def async bump(*i32 value)\n"
                "{\n"
                "    *value = 41;\n"
                "}\n"
                "cdef __shaft_entry(i32 argc, *i8 argv) -> i32\n"
                "{\n"
                "    mut i32 result = 0;\n"
                "    Thread bump(&result) worker;\n"
                "    start worker;\n"
                "    await worker;\n"
                "    return result + 1;\n"
                "}\n",
                encoding="utf-8",
            )
            compilation = subprocess.run(
                [str(SHAFTC), "--no-std", "-o", str(binary), str(source)],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(compilation.returncode, 0, compilation.stdout + compilation.stderr)
            execution = subprocess.run([str(binary)], capture_output=True, check=False)
            self.assertEqual(execution.returncode, 42, execution.stderr)

if __name__ == "__main__":
    unittest.main()
