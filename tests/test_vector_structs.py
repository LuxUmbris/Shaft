import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SHAFTC = ROOT / "build" / "shaftc"


class VectorStructTests(unittest.TestCase):
    def test_vector_of_structs_supports_member_access_after_push(self):
        with tempfile.TemporaryDirectory(prefix="shaftc-vector-struct-") as directory:
            work = Path(directory)
            source = work / "vector-struct.shaft"
            binary = work / "vector-struct"
            source.write_text(
                "struct Edge { mut u32 parent; mut u8 weight; }\n"
                "def main(String[] args) {\n"
                "  mut Vector<Edge> edges;\n"
                "  vector_init::<Edge>(ref edges, 1);\n"
                "  mut Edge edge; edge.parent = 300; edge.weight = 128;\n"
                "  edges.push(edge);\n"
                "  if (edges[0].parent != 300 || edges[0].weight != 128) { exit(1); }\n"
                "}\n",
                encoding="utf-8",
            )
            compiled = subprocess.run([str(SHAFTC), "-o", str(binary), str(source)], text=True, capture_output=True)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            executed = subprocess.run([str(binary)], text=True, capture_output=True)
            self.assertEqual(executed.returncode, 0, executed.stdout + executed.stderr)


if __name__ == "__main__":
    unittest.main()
