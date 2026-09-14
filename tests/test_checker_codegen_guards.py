import os
import subprocess
import tempfile
import unittest
from pathlib import Path


REPOSITORY = Path(__file__).resolve().parents[1]
SHAFTC = Path(os.environ.get("SHAFTC", REPOSITORY / "build" / "shaftc"))


@unittest.skipUnless(SHAFTC.is_file(), "shaftc build artifact is required")
class CheckerCodegenGuardTests(unittest.TestCase):
    def check_source(self, source_text):
        with tempfile.TemporaryDirectory(prefix="shaftc-checker-guard-") as directory:
            source = Path(directory) / "fixture.shaft"
            source.write_text(source_text, encoding="utf-8")
            return subprocess.run(
                [str(SHAFTC), "--no-std", "--check-only", str(source)],
                capture_output=True,
                text=True,
                check=False,
            )

    def test_checker_rejects_tunnel_outputs_on_main(self):
        result = self.check_source(
            "def main() -> i32 result { tunnel 0 -> i32 result; }\n"
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("main cannot declare tunnel outputs", result.stderr)

    def test_checker_rejects_generic_declarations_without_definitions(self):
        result = self.check_source(
            "dec identity(i32 value)<T> -> i32 result;\n"
            "cdef main() -> i32 { return 0; }\n"
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("generic function declarations require a definition", result.stderr)

    def test_checker_requires_match_default_to_be_final(self):
        result = self.check_source(
            "cdef main() -> i32 { match (1) { default { } case 1 { } } return 0; }\n"
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("match default must be the final arm", result.stderr)

    def test_checker_rejects_foreach_over_a_scalar(self):
        result = self.check_source(
            "cdef main() -> i32 { i32 value = 1; foreach (i32 item : value) { } return 0; }\n"
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("foreach requires a fixed-size array, T[length] runtime array, or Vector<T>", result.stderr)
