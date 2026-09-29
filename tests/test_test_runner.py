import pathlib
import subprocess
import sys
import unittest


REPOSITORY = pathlib.Path(__file__).resolve().parents[1]
RUNNER = REPOSITORY / "test.py"


class RepositoryTestRunnerTests(unittest.TestCase):
    def test_list_exposes_every_repository_test_phase(self):
        result = subprocess.run(
            [sys.executable, str(RUNNER), "--list"],
            cwd=REPOSITORY,
            text=True,
            capture_output=True,
            check=False,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(
            result.stdout.splitlines(),
            [
                "python-unit",
                "shaftls-protocol",
                "vscode",
                "shaftc-smoke",
                "runtime-cross-compile",
            ],
        )


if __name__ == "__main__":
    unittest.main()
