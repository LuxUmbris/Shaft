import os
import subprocess
import tempfile
import time
import unittest
from pathlib import Path


REPOSITORY = Path(__file__).resolve().parents[1]
SHAFTC = Path(os.environ.get("SHAFTC", REPOSITORY / "build" / "shaftc"))


@unittest.skipUnless(SHAFTC.is_file() and os.name == "posix", "requires Linux shaftc")
class NativeThreadTests(unittest.TestCase):
    def test_thread_start_runs_the_worker_concurrently_and_await_joins_it(self):
        source_text = """def worker(*i32 output)
{
    Time::sleep(40);
    *output = 42;
}

def main()
{
    mut i32 output = 0;
    Thread worker(&output) task;
    start task;
    if (output != 0) { exit(1); }
    await task;
    if (output != 42) { exit(2); }
}
"""
        with tempfile.TemporaryDirectory(prefix="shaftc-native-thread-") as directory:
            work = Path(directory)
            source = work / "program.shaft"
            binary = work / "program"
            source.write_text(source_text, encoding="utf-8")
            compilation = subprocess.run(
                [str(SHAFTC), "-o", str(binary), str(source)],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(compilation.returncode, 0, compilation.stdout + compilation.stderr)
            execution = subprocess.run([str(binary)], capture_output=True, timeout=2, check=False)
            self.assertEqual(execution.returncode, 0, execution.stderr)

    def test_concurrent_thread_allocations_preserve_runtime_allocator_metadata(self):
        source_text = """def allocate(*i32 output)
{
    String value = String::with_capacity(128);
    value.append("thread-safe");
    *output = value.length;
}

def main()
{
    mut i32 first = 0;
    mut i32 second = 0;
    Thread allocate(&first) firstTask;
    Thread allocate(&second) secondTask;
    start firstTask;
    start secondTask;
    await firstTask;
    await secondTask;
    if (first != 11 || second != 11) { exit(1); }
}
"""
        with tempfile.TemporaryDirectory(prefix="shaftc-native-thread-allocator-") as directory:
            work = Path(directory)
            source = work / "program.shaft"
            binary = work / "program"
            source.write_text(source_text, encoding="utf-8")
            compilation = subprocess.run(
                [str(SHAFTC), "-o", str(binary), str(source)],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(compilation.returncode, 0, compilation.stdout + compilation.stderr)
            execution = subprocess.run([str(binary)], capture_output=True, timeout=2, check=False)
            self.assertEqual(execution.returncode, 0, execution.stderr)

    def test_scope_exit_joins_a_started_thread_before_local_cleanup(self):
        source_text = """def worker()
{
    Time::sleep(40);
}

cdef leave_scope()
{
    Thread worker() task;
    start task;
}

def main()
{
    leave_scope();
}
"""
        with tempfile.TemporaryDirectory(prefix="shaftc-native-thread-scope-") as directory:
            work = Path(directory)
            source = work / "program.shaft"
            binary = work / "program"
            source.write_text(source_text, encoding="utf-8")
            compilation = subprocess.run(
                [str(SHAFTC), "-o", str(binary), str(source)],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(compilation.returncode, 0, compilation.stdout + compilation.stderr)
            started = time.monotonic()
            execution = subprocess.run([str(binary)], capture_output=True, timeout=2, check=False)
            elapsed = time.monotonic() - started
            self.assertEqual(execution.returncode, 0, execution.stderr)
            self.assertGreaterEqual(elapsed, 0.025)


if __name__ == "__main__":
    unittest.main()
