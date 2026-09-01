import subprocess
import tempfile
import unittest
from pathlib import Path


REPOSITORY = Path(__file__).resolve().parents[1]
SHAFTC = REPOSITORY / "build" / "shaftc"


class CompilerScaleTests(unittest.TestCase):
    def test_large_source_without_using_macros_is_not_limited_by_macro_expansion_cap(self):
        self.assertTrue(SHAFTC.is_file(), "build shaftc before running this test")
        with tempfile.TemporaryDirectory(prefix="shaftc-large-source-") as directory:
            source = Path(directory) / "large.shaft"
            # Five tokens per declaration; this deliberately exceeds the 1M
            # macro-expansion safety cap without containing any using macros.
            declarations = "".join(f"i32 item{index} = {index};\n" for index in range(200_001))
            source.write_text(
                "cdef __shaft_entry(i32 argc, *i8 argv) -> i32\n{\n"
                + declarations
                + "return 0;\n}\n",
                encoding="utf-8",
            )
            result = subprocess.run(
                [str(SHAFTC), "--no-std", "--check-only", str(source)],
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_using_macro_expansion_cap_still_applies_to_generated_tokens(self):
        self.assertTrue(SHAFTC.is_file(), "build shaftc before running this test")
        with tempfile.TemporaryDirectory(prefix="shaftc-macro-cap-") as directory:
            source = Path(directory) / "macro-cap.shaft"
            source.write_text(
                "using alias -> 1;\n"
                + "alias " * 1_000_001,
                encoding="utf-8",
            )
            result = subprocess.run(
                [str(SHAFTC), "--no-std", "--check-only", str(source)],
                capture_output=True,
                text=True,
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("using macro expansion exceeds the token safety limit", result.stderr)


if __name__ == "__main__":
    unittest.main()
