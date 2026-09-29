import os
import pty
import select
import subprocess
import tempfile
import time
import unittest
from pathlib import Path


REPOSITORY = Path(__file__).resolve().parents[1]
SHAFTC = Path(os.environ.get("SHAFTC", REPOSITORY / "build" / "shaftc"))


def read_until(descriptor: int, expected: bytes, timeout: float) -> bytes:
    deadline = time.monotonic() + timeout
    received = bytearray()
    while expected not in received:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError(f"did not receive {expected!r}; received {bytes(received)!r}")
        readable, _, _ = select.select([descriptor], [], [], remaining)
        if not readable:
            continue
        chunk = os.read(descriptor, 4096)
        if not chunk:
            break
        received.extend(chunk)
    return bytes(received)


@unittest.skipUnless(SHAFTC.is_file() and os.name == "posix", "requires Linux shaftc")
class StdReadKeyTests(unittest.TestCase):
    def test_process_read_key_is_nonblocking_and_decodes_enter_arrow_control_and_alt_keys(self):
        source_text = """def main()
{
    reserve Key initial = Process::read_key();
    if (initial != Key::None)
    {
        exit(1);
    }
    print("ready\\n");

    mut Key key = Key::None;
    while (key == Key::None)
    {
        key = Process::read_key();
    }
    if (key != Key::Enter)
    {
        exit(2);
    }

    key = Key::None;
    while (key == Key::None)
    {
        key = Process::read_key();
    }
    if (key != Key::ArrowUp)
    {
        exit(3);
    }

    key = Key::None;
    while (key == Key::None)
    {
        key = Process::read_key();
    }
    if (key != Key::ControlX)
    {
        exit(4);
    }

    key = Key::None;
    while (key == Key::None)
    {
        key = Process::read_key();
    }
    if (key != Key::AltY)
    {
        exit(5);
    }
}
"""
        with tempfile.TemporaryDirectory(prefix="shaftc-read-key-") as directory:
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

            master, slave = pty.openpty()
            try:
                process = subprocess.Popen(
                    [str(binary)],
                    stdin=slave,
                    stdout=slave,
                    stderr=slave,
                    close_fds=True,
                )
                os.close(slave)
                slave = -1
                self.assertIn(b"ready\n", read_until(master, b"ready\n", 1))
                os.write(master, b"\n")
                os.write(master, b"\x1b[A")
                os.write(master, b"\x18")
                os.write(master, b"\x1by")
                self.assertEqual(process.wait(timeout=2), 0)
            finally:
                if slave != -1:
                    os.close(slave)
                os.close(master)


if __name__ == "__main__":
    unittest.main()
