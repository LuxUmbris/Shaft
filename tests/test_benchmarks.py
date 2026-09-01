import json
import pathlib
import subprocess
import sys
import tempfile
import unittest


REPOSITORY = pathlib.Path(__file__).resolve().parents[1]
RUNNER = REPOSITORY / "benchmarks" / "run.py"


class BenchmarkRunnerTests(unittest.TestCase):
    def test_runner_records_compile_metadata_and_missing_rust_toolchain(self):
        with tempfile.TemporaryDirectory() as directory:
            output = pathlib.Path(directory) / "results.json"
            result = subprocess.run(
                [
                    sys.executable,
                    str(RUNNER),
                    "--iterations",
                    "1",
                    "--skip-runtime",
                    "--output",
                    str(output),
                ],
                cwd=REPOSITORY,
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            report = json.loads(output.read_text())
            self.assertEqual(report["schema_version"], 2)
            self.assertIn("shaft", report["tools"])
            self.assertIn("clang", report["tools"])
            self.assertIn("compile_scale", report["benchmarks"])
            implementations = report["benchmarks"]["compile_scale"]["implementations"]
            self.assertIn("affinity", report["protocol"])
            self.assertIn("--native", report["protocol"]["shaft_flags"])
            self.assertIn("mean_ms", implementations["shaft"])
            self.assertIn("maximum_ms", implementations["clang"])
            self.assertEqual(implementations["shaft"]["status"], "ok")
            self.assertEqual(implementations["clang"]["status"], "ok")
            self.assertIn(implementations["rust"]["status"], {"ok", "unavailable"})


if __name__ == "__main__":
    unittest.main()
