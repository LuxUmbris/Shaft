import os
import subprocess
import tempfile
import unittest
from pathlib import Path


REPOSITORY = Path(__file__).resolve().parents[1]
SHAFTC = REPOSITORY / "build" / "shaftc"


@unittest.skipUnless(SHAFTC.is_file() and os.name == "posix", "requires Linux shaftc")
class StdContractsTests(unittest.TestCase):
    def test_hash_map_get_returns_presence_and_optional_generic_value(self):
        with tempfile.TemporaryDirectory(prefix="shaftc-hash-map-get-") as directory:
            work = Path(directory)
            source = work / "hash-map-get.shaft"
            binary = work / "hash-map-get"
            source.write_text(
                "def main(String[] args)\n"
                "{\n"
                "    reserve mut Collections::Pair<i32, bool> payload; payload.key = 42; payload.value = true;\n"
                "    reserve mut Collections::HashMap<i32, Collections::Pair<i32, bool>> map;\n"
                "    Collections::hash_map_init::<i32, Collections::Pair<i32, bool>>(ref map, 1);\n"
                "    Collections::hash_map_insert::<i32, Collections::Pair<i32, bool>>(ref map, 7, payload);\n"
                "    { reserve bool found; reserve ?Collections::Pair<i32, bool> value; Collections::hash_map_get::<i32, Collections::Pair<i32, bool>>(ref map, 7); valid value { if (!found || value.key != 42 || !value.value) { exit(1); } } else { exit(2); } }\n"
                "    { reserve bool found; reserve ?Collections::Pair<i32, bool> value; Collections::hash_map_get::<i32, Collections::Pair<i32, bool>>(ref map, 8); if (found) { exit(3); } valid value { exit(4); } }\n"
                "}\n",
                encoding="utf-8",
            )
            compilation = subprocess.run([str(SHAFTC), "-o", str(binary), str(source)], capture_output=True, text=True, check=False)
            self.assertEqual(compilation.returncode, 0, compilation.stdout + compilation.stderr)
            execution = subprocess.run([str(binary)], capture_output=True, check=False)
            self.assertEqual(execution.returncode, 0, execution.stderr)


if __name__ == "__main__":
    unittest.main()
