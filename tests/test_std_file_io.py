import os
import subprocess
import tempfile
import unittest
from pathlib import Path


REPOSITORY = Path(__file__).resolve().parents[1]
SHAFTC = REPOSITORY / "build" / "shaftc"


@unittest.skipUnless(SHAFTC.is_file() and os.name == "posix", "requires Linux shaftc")
class StdFileIoTests(unittest.TestCase):
    def test_file_write_append_read_and_readline(self):
        with tempfile.TemporaryDirectory(prefix="shaftc-file-") as directory:
            work = Path(directory)
            path = work / "message.txt"
            source = work / "file-io.shaft"
            binary = work / "file-io"
            source.write_text(
                "def main(String[] args)\n"
                "{\n"
                f'    reserve i64 written = File::write("{path}", "hello");\n'
                "    if (written != 5)\n"
                "    {\n"
                "        exit(1);\n"
                "    }\n"
                f'    reserve i64 appended = File::append("{path}", " world");\n'
                "    if (appended != 6)\n"
                "    {\n"
                "        exit(2);\n"
                "    }\n"
                f'    reserve String content = File::read("{path}");\n'
                "    if (content.length != 11 || content.data[10] != 100)\n"
                "    {\n"
                "        exit(3);\n"
                "    }\n"
                "    reserve String direct = String::with_capacity(16);\n"
                f'    reserve i64 directRead = File::read_into("{path}", direct.data, direct.capacity);\n'
                "    if (directRead != 11 || direct.data[6] != 119)\n"
                "    {\n"
                "        exit(5);\n"
                "    }\n"
                "    reserve String line = readline();\n"
                "    if (line.length != 6 || line.data[0] != 32 || line.data[5] != 116)\n"
                "    {\n"
                "        exit(4);\n"
                "    }\n"
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
            execution = subprocess.run([str(binary)], input=b" input\nignored\n", capture_output=True, check=False)
            self.assertEqual(execution.returncode, 0, execution.stderr)
            self.assertEqual(execution.stdout, b"")
            self.assertEqual(path.read_bytes(), b"hello world")
    def test_readline_uses_buffered_runtime_reads(self):
        with tempfile.TemporaryDirectory(prefix="shaftc-readline-") as directory:
            work = Path(directory)
            source = work / "readline.shaft"
            binary = work / "readline"
            source.write_text(
                "def main(String[] args)\n"
                "{\n"
                "    mut u64 lineNumber = 0;\n"
                "    while (lineNumber < 128)\n"
                "    {\n"
                "        reserve String line = readline();\n"
                "        if (line.length != 64 || line.data[0] != 120)\n"
                "        {\n"
                "            exit(1);\n"
                "        }\n"
                "        lineNumber = lineNumber + 1;\n"
                "    }\n"
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
            execution = subprocess.run(
                ["strace", "-e", "trace=read", "-c", str(binary)],
                input=(b"x" * 64 + b"\n") * 128,
                capture_output=True,
                check=False,
            )
            self.assertEqual(execution.returncode, 0, execution.stderr)
            summary = execution.stderr.decode("utf-8", errors="replace")
            read_row = next((line for line in summary.splitlines() if line.rstrip().endswith(" read")), "")
            self.assertTrue(read_row, summary)
            read_calls = int(read_row.split()[-2])
            self.assertLessEqual(read_calls, 4, summary)

    def test_interactive_descriptor_quiz_counts_questions_and_points(self):
        example = REPOSITORY / "examples" / "interactive-quiz.shaft"
        descriptor = REPOSITORY / "examples" / "interactive-quiz.descriptor"
        with tempfile.TemporaryDirectory(prefix="shaftc-quiz-") as directory:
            binary = Path(directory) / "interactive-quiz"
            compilation = subprocess.run(
                [str(SHAFTC), "-o", str(binary), str(example)],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(compilation.returncode, 0, compilation.stdout + compilation.stderr)
            execution = subprocess.run(
                [str(binary), str(descriptor)],
                input=b"4\nP\n",
                capture_output=True,
                check=False,
            )
            self.assertEqual(execution.returncode, 8, execution.stderr)
            self.assertEqual(execution.stdout, b"Answer: Answer: ")


if __name__ == "__main__":
    unittest.main()
