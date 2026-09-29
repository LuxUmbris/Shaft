import os
import subprocess
import tempfile
import unittest
from pathlib import Path


REPOSITORY = Path(__file__).resolve().parents[1]
SHAFTC = Path(os.environ.get("SHAFTC", REPOSITORY / "build" / "shaftc"))


@unittest.skipUnless(SHAFTC.is_file() and os.name == "posix", "requires Linux shaftc")
class StdProcessTests(unittest.TestCase):
    def compile_and_run(self, source_text):
        with tempfile.TemporaryDirectory(prefix="shaftc-std-process-") as directory:
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
            return subprocess.run([str(binary)], capture_output=True, timeout=2, check=False)

    def test_command_output_captures_streams_and_exit_status(self):
        execution = self.compile_and_run(
            "def main()\n"
            "{\n"
            "    reserve Process::Command command = Process::Command::new(\"/bin/sh\");\n"
            "    command.arg(\"-c\");\n"
            "    command.arg(\"printf stdout; printf stderr >&2; exit 7\");\n"
            "    reserve Process::Output output = command.output();\n"
            "    if (output.status != 7) { exit(1); }\n"
            "    if (output.stdout.length != 6 || output.stdout.data[0] != 115 || output.stdout.data[5] != 116) { exit(2); }\n"
            "    if (output.stderr.length != 6 || output.stderr.data[0] != 115 || output.stderr.data[5] != 114) { exit(3); }\n"
            "}\n"
        )
        self.assertEqual(execution.returncode, 0, execution.stderr)
        self.assertEqual(execution.stdout, b"")
        self.assertEqual(execution.stderr, b"")

    def test_command_status_and_child_wait_report_exit_status(self):
        execution = self.compile_and_run(
            "def main()\n"
            "{\n"
            "    reserve Process::Command status_command = Process::Command::new(\"/bin/sh\");\n"
            "    status_command.arg(\"-c\");\n"
            "    status_command.arg(\"exit 19\");\n"
            "    if (status_command.status() != 19) { exit(1); }\n"
            "    reserve Process::Command child_command = Process::Command::new(\"/bin/sh\");\n"
            "    child_command.arg(\"-c\");\n"
            "    child_command.arg(\"exit 23\");\n"
            "    reserve Process::Child child = child_command.spawn();\n"
            "    if (child.pid <= 0 || child.wait() != 23) { exit(2); }\n"
            "}\n"
        )
        self.assertEqual(execution.returncode, 0, execution.stderr)


if __name__ == "__main__":
    unittest.main()
