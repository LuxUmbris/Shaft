import os
import subprocess
import tempfile
import unittest
from pathlib import Path


REPOSITORY = Path(__file__).resolve().parents[1]
SHAFTC = Path(os.environ.get("SHAFTC", REPOSITORY / "build" / "shaftc"))


@unittest.skipUnless(SHAFTC.is_file() and os.name == "posix", "requires Linux shaftc")
class StdRngAppendTests(unittest.TestCase):
    def compile_and_run(self, source_text):
        with tempfile.TemporaryDirectory(prefix="shaftc-rng-append-") as directory:
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

    def test_rng_methods_mutate_the_actual_receiver(self):
        self.compile_and_run(
            "def main()\n"
            "{\n"
            "    mut RNG rng;\n"
            "    rng.set_seed(12345);\n"
            "    reserve i32 integer = rng.get_int(10, 20);\n"
            "    if (integer < 10 || integer > 20) { exit(1); }\n"
            "    reserve f32 fraction = rng.get_float(1.0, 2.0);\n"
            "    if (fraction < 1.0 || fraction >= 2.0) { exit(2); }\n"
            "}\n"
        )

    def test_string_append_cstr_calls_methods_on_the_actual_receiver(self):
        self.compile_and_run(
            "def main()\n"
            "{\n"
            "    reserve String text = String::from_str(\"a\");\n"
            "    reserve bool appended = text.append_cstr(\"bc\");\n"
            "    if (!appended || text.length != 3 || text.data[0] != 97 || text.data[1] != 98 || text.data[2] != 99 || text.data[3] != 0) { exit(1); }\n"
            "}\n"
        )
