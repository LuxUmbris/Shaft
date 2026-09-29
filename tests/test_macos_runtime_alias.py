#!/usr/bin/env python3
import os
import pathlib
import subprocess
import tempfile
import unittest


REPOSITORY = pathlib.Path(__file__).resolve().parents[1]
COMPILER = pathlib.Path(os.environ.get("SHAFTC", REPOSITORY / "build" / "shaftc"))


@unittest.skipUnless(COMPILER.is_file(), "shaftc build artifact is required")
class MacOSRuntimeAliasTests(unittest.TestCase):
    def test_macos_target_normalizes_to_the_shared_darwin_runtime_target(self):
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / "app.shaft"
            output = pathlib.Path(directory) / "app.ll"
            source.write_text("cdef main() -> i32 { return 0; }\n", encoding="utf-8")

            result = subprocess.run(
                [
                    str(COMPILER), "--no-std", "--target", "x86_64-apple-macos",
                    "--emit", "llvm", "-o", str(output), str(source),
                ],
                text=True,
                capture_output=True,
                check=False,
            )

            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('target triple = "x86_64-apple-darwin"', output.read_text(encoding="utf-8"))

    def test_macos_does_not_have_a_separate_runtime_resource(self):
        self.assertFalse((REPOSITORY / "std" / "runtime" / "macos.shaft").exists())
        self.assertTrue((REPOSITORY / "std" / "runtime" / "darwin.shaft").is_file())


if __name__ == "__main__":
    unittest.main()
