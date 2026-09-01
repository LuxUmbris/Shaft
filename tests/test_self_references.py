import os
import subprocess
import tempfile
import unittest
from pathlib import Path


REPOSITORY = Path(__file__).resolve().parents[1]
SHAFTC = REPOSITORY / "build" / "shaftc"


@unittest.skipUnless(SHAFTC.is_file() and os.name == "posix", "requires Linux shaftc")
class SelfReferenceMethodTests(unittest.TestCase):
    def test_self_reference_void_method_and_unsigned_dereference_arithmetic(self):
        with tempfile.TemporaryDirectory(prefix="shaftc-self-reference-") as directory:
            work = Path(directory)
            source = work / "self-reference.shaft"
            binary = work / "self-reference"
            source.write_text(
                "class Meter\n"
                "{\n"
                "    mut u8 value;\n"
                "\n"
                "    def set(&mut self, u8 next)\n"
                "    {\n"
                "        &mut u8 slot = ref self.value;\n"
                "        *slot = next;\n"
                "    }\n"
                "\n"
                "    def scaled(&self) -> u8 result\n"
                "    {\n"
                "        mut u8 current = 0;\n"
                "        { &u8 slot = ref self.value; current = *slot; }\n"
                "        mut u32 attenuation = 255 - current;\n"
                "        mut u32 product = 10 * attenuation;\n"
                "        tunnel product / 255 -> u8 result;\n"
                "    }\n"
                "}\n"
                "\n"
                "def set_to_42(&mut u8 value)\n"
                "{\n"
                "    *value = 42;\n"
                "}\n"
                "\n"
                "def main()\n"
                "{\n"
                "    mut Meter meter;\n"
                "    meter.set(128);\n"
                "    if (meter.scaled() != 4) { exit(1); }\n"
                "    mut u8 external = 0;\n"
                "    set_to_42(ref external);\n"
                "    if (external != 42) { exit(2); }\n"
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
            self.assertEqual(execution.stdout, b"")


if __name__ == "__main__":
    unittest.main()
