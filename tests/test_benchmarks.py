import json
import pathlib
import subprocess
import sys
import tempfile
import unittest


REPOSITORY = pathlib.Path(__file__).resolve().parents[1]
RUNNER = REPOSITORY / "benchmarks" / "run.py"


class BenchmarkRunnerTests(unittest.TestCase):
    def test_runner_records_computer_language_benchmarks_game_mandelbrot_metadata(self):
        with tempfile.TemporaryDirectory() as directory:
            output = pathlib.Path(directory) / "results.json"
            result = subprocess.run(
                [
                    sys.executable,
                    str(RUNNER),
                    "--iterations",
                    "1",
                    "--size",
                    "32",
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
            self.assertEqual(report["schema_version"], 3)
            self.assertIn("shaft", report["tools"])
            self.assertIn("clang", report["tools"])
            self.assertIn("mandelbrot", report["benchmarks"])
            mandelbrot = report["benchmarks"]["mandelbrot"]
            self.assertEqual(mandelbrot["family"], "Computer Language Benchmarks Game")
            self.assertEqual(mandelbrot["name"], "mandelbrot")
            self.assertIn("expected_exit_code", mandelbrot)
            implementations = mandelbrot["compile"]["implementations"]
            self.assertIn("affinity", report["protocol"])
            self.assertIn("-O2", report["protocol"]["shaft_flags"])
            self.assertIn("mean_ms", implementations["shaft"])
            self.assertIn("maximum_ms", implementations["clang"])
            self.assertEqual(implementations["shaft"]["status"], "ok")
            self.assertEqual(implementations["clang"]["status"], "ok")
            self.assertIn(implementations["rust"]["status"], {"ok", "unavailable"})
            runtime = mandelbrot["runtime"]["implementations"]
            for implementation in runtime.values():
                if implementation["status"] == "ok":
                    self.assertEqual(implementation["exit_code"], mandelbrot["expected_exit_code"])


if __name__ == "__main__":
    unittest.main()
