import subprocess
import tempfile
import unittest
from pathlib import Path


REPOSITORY = Path(__file__).resolve().parents[1]
SHAFTC = REPOSITORY / "build" / "shaftc"


@unittest.skipUnless(SHAFTC.is_file(), "build shaftc before running this test")
class StdPrintfTests(unittest.TestCase):
    def test_printf_formats_a_string_slice(self):
        with tempfile.TemporaryDirectory(prefix="shaftc-printf-") as directory:
            source = Path(directory) / "printf.shaft"
            binary = Path(directory) / "printf"
            source.write_text(
                "def main(String[] args)\n"
                "{\n"
                "    reserve mut str value;\n"
                "    value.data = \"world\";\n"
                "    value.length = 5;\n"
                "    reserve i64 written = printf(\"hello, {str}!\\n\", value);\n"
                "}\n",
                encoding="utf-8",
            )
            compilation = subprocess.run(
                [str(SHAFTC), "-o", str(binary), str(source)],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(compilation.returncode, 0, compilation.stdout + compilation.stderr)
            execution = subprocess.run([str(binary)], capture_output=True, check=False)
            self.assertEqual(execution.returncode, 0, execution.stderr)
            self.assertEqual(execution.stdout, b"hello, world!\n")
    def test_printf_formats_usize_as_unsigned_native_width(self):
        with tempfile.TemporaryDirectory(prefix="shaftc-printf-") as directory:
            source = Path(directory) / "printf-usize.shaft"
            binary = Path(directory) / "printf-usize"
            source.write_text(
                "def main(String[] args)\n"
                "{\n"
                "    usize maximum = 18446744073709551615;\n"
                "    printf(\"{usize}\\n\", maximum);\n"
                "}\n",
                encoding="utf-8",
            )
            compilation = subprocess.run(
                [str(SHAFTC), "-o", str(binary), str(source)],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(compilation.returncode, 0, compilation.stdout + compilation.stderr)
            execution = subprocess.run([str(binary)], capture_output=True, check=False)
            self.assertEqual(execution.returncode, 0, execution.stderr)
            self.assertEqual(execution.stdout, b"18446744073709551615\n")


if __name__ == "__main__":
    unittest.main()
