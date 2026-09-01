import os
import socket
import subprocess
import tempfile
import threading
import unittest
from pathlib import Path


REPOSITORY = Path(__file__).resolve().parents[1]
SHAFTC = REPOSITORY / "build" / "shaftc"


@unittest.skipUnless(SHAFTC.is_file() and os.name == "posix", "requires Linux shaftc")
class StdNetworkTests(unittest.TestCase):
    def test_tcp_connect_send_receive_and_close(self):
        listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind(("127.0.0.1", 0))
        listener.listen(1)
        port = listener.getsockname()[1]
        server_error = []

        def serve():
            try:
                connection, _ = listener.accept()
                with connection:
                    self.assertEqual(connection.recv(4), b"ping")
                    connection.sendall(b"pong")
            except Exception as error:  # Report thread failures in the test process.
                server_error.append(error)
            finally:
                listener.close()

        thread = threading.Thread(target=serve, daemon=True)
        started = False
        try:
            with tempfile.TemporaryDirectory(prefix="shaftc-network-") as directory:
                work = Path(directory)
                source = work / "network.shaft"
                binary = work / "network"
                source.write_text(
                    "def main(String[] args)\n"
                    "{\n"
                    "    if (Network::ipv4(127, 0, 0, 1) != 2130706433)\n"
                    "    {\n"
                    "        exit(5);\n"
                    "    }\n"
                    f"    reserve Network::Socket client = Network::tcp_connect_ipv4(Network::ipv4(127, 0, 0, 1), {port});\n"
                    "    if (client.descriptor < 0)\n"
                    "    {\n"
                    "        exit(1);\n"
                    "    }\n"
                    "    reserve i64 written = Network::send(&client, \"ping\");\n"
                    "    if (written != 4)\n"
                    "    {\n"
                    "        exit(2);\n"
                    "    }\n"
                    "    *u8 response = __shaft_alloc(4);\n"
                    "    reserve i64 received = Network::receive(&client, response, 4);\n"
                    "    if (received != 4 || response[0] != 112 || response[3] != 103)\n"
                    "    {\n"
                    "        exit(3);\n"
                    "    }\n"
                    "    reserve i64 closed = Network::close(&client);\n"
                    "    if (closed != 0)\n"
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
                thread.start()
                started = True
                execution = subprocess.run([str(binary)], capture_output=True, check=False)
                self.assertEqual(execution.returncode, 0, execution.stderr)
                self.assertEqual(execution.stdout, b"")
        finally:
            if started:
                thread.join(timeout=5)
            else:
                listener.close()
        self.assertFalse(started and thread.is_alive(), "network fixture did not finish")
        self.assertEqual(server_error, [])


if __name__ == "__main__":
    unittest.main()
