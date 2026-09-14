import os
import subprocess
import tempfile
import unittest
from pathlib import Path


REPOSITORY = Path(__file__).resolve().parents[1]
SHAFTC = Path(os.environ.get("SHAFTC", REPOSITORY / "build" / "shaftc"))


@unittest.skipUnless(SHAFTC.is_file() and os.name == "posix", "requires Linux shaftc")
class StdTimeTests(unittest.TestCase):
    def test_sleep_blocks_for_the_requested_millisecond_duration(self):
        source_text = """def main()
{
    reserve TimePoint before = Time::now();
    Time::sleep(25);
    reserve TimePoint after = Time::now();
    i64 elapsed = (after.seconds - before.seconds) * 1000000000 + after.nanoseconds - before.nanoseconds;
    if (elapsed < 10000000) { exit(1); }
}
"""
        with tempfile.TemporaryDirectory(prefix="shaftc-std-time-") as directory:
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


if __name__ == "__main__":
    unittest.main()
