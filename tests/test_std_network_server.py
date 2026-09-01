import os
import socket
import subprocess
import tempfile
import time
import unittest
from pathlib import Path


REPOSITORY = Path(__file__).resolve().parents[1]
SHAFTC = REPOSITORY / "build" / "shaftc"
LOOPBACK = 2130706433


def unused_port():
    probe = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    probe.bind(("127.0.0.1", 0))
    port = probe.getsockname()[1]
    probe.close()
    return port


@unittest.skipUnless(SHAFTC.is_file() and os.name == "posix", "requires Linux shaftc")
class StdNetworkServerTests(unittest.TestCase):
    def test_tcp_listener_accepts_request_and_sends_response(self):
        port = unused_port()
        with tempfile.TemporaryDirectory(prefix="shaftc-network-server-") as directory:
            work = Path(directory)
            source = work / "server.shaft"
            binary = work / "server"
            source.write_text(
                "def main(String[] args)\n"
                "{\n"
                f"    reserve Network::Socket listener = Network::tcp_listen_ipv4(Network::ipv4(127, 0, 0, 1), {port}, 1);\n"
                "    if (listener.descriptor < 0)\n"
                "    {\n"
                "        exit(1);\n"
                "    }\n"
                "    reserve Network::Socket client = Network::accept(&listener);\n"
                "    if (client.descriptor < 0)\n"
                "    {\n"
                "        exit(2);\n"
                "    }\n"
                "    *u8 request = __shaft_alloc(4);\n"
                "    reserve i64 received = Network::receive(&client, request, 4);\n"
                "    if (received != 4 || request[0] != 112 || request[3] != 103)\n"
                "    {\n"
                "        exit(3);\n"
                "    }\n"
                "    reserve i64 sent = Network::send(&client, \"pong\");\n"
                "    if (sent != 4)\n"
                "    {\n"
                "        exit(4);\n"
                "    }\n"
                "    Network::close(&client);\n"
                "    Network::close(&listener);\n"
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
            server = subprocess.Popen([str(binary)], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            try:
                deadline = time.monotonic() + 5
                while True:
                    try:
                        client = socket.create_connection(("127.0.0.1", port), timeout=0.2)
                        break
                    except ConnectionRefusedError:
                        self.assertLess(time.monotonic(), deadline, "Shaft listener never became ready")
                        time.sleep(0.02)
                with client:
                    client.sendall(b"ping")
                    self.assertEqual(client.recv(4), b"pong")
                stdout, stderr = server.communicate(timeout=5)
            finally:
                if server.poll() is None:
                    server.kill()
                    server.communicate()
            self.assertEqual(server.returncode, 0, stderr)
            self.assertEqual(stdout, b"")

    def test_nonblocking_accept_reports_would_block_without_waiting(self):
        port = unused_port()
        with tempfile.TemporaryDirectory(prefix="shaftc-network-nonblocking-") as directory:
            work = Path(directory)
            source = work / "nonblocking.shaft"
            binary = work / "nonblocking"
            source.write_text(
                "def main(String[] args)\n"
                "{\n"
                f"    reserve Network::Socket listener = Network::tcp_listen_ipv4(Network::ipv4(127, 0, 0, 1), {port}, 1);\n"
                "    if (listener.descriptor < 0 || Network::set_nonblocking(&listener) != 0)\n"
                "    {\n"
                "        exit(1);\n"
                "    }\n"
                "    reserve Network::Socket pending = Network::try_accept(&listener);\n"
                "    if (pending.descriptor != -2)\n"
                "    {\n"
                "        exit(2);\n"
                "    }\n"
                "    Network::close(&listener);\n"
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
            execution = subprocess.run([str(binary)], capture_output=True, timeout=2, check=False)
            self.assertEqual(execution.returncode, 0, execution.stderr)
            self.assertEqual(execution.stdout, b"")


if __name__ == "__main__":
    unittest.main()
