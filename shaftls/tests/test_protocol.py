#!/usr/bin/env python3
import json
import os
import subprocess
import unittest
from pathlib import Path

PROJECT = Path(__file__).resolve().parents[1]
REPOSITORY = PROJECT.parent
SHAFTC = Path(os.environ.get("SHAFTC", REPOSITORY / "build-linux-x86_64-release" / "shaftc"))
BINARY = PROJECT / "build" / "shaftls"


def frame(message):
    body = json.dumps(message, separators=(",", ":")).encode("utf-8")
    return f"Content-Length: {len(body)}\r\n\r\n".encode("ascii") + body


def read_message(stream):
    headers = b""
    while b"\r\n\r\n" not in headers:
        byte = stream.read(1)
        if not byte:
            raise AssertionError("shaftls closed stdout before replying")
        headers += byte
    header_text, body = headers.split(b"\r\n\r\n", 1)
    length = int(next(line.split(b":", 1)[1].strip() for line in header_text.split(b"\r\n") if line.lower().startswith(b"content-length:")))
    while len(body) < length:
        chunk = stream.read(length - len(body))
        if not chunk:
            raise AssertionError("shaftls sent a truncated LSP message")
        body += chunk
    return json.loads(body.decode("utf-8"))


class ShaftLsProtocolTests(unittest.TestCase):
    def test_initialize_shutdown_and_exit_follow_lsp_framing(self):
        build = subprocess.run([str(SHAFTC), "--build", "Shaft.build"], cwd=PROJECT, text=True, capture_output=True, check=False)
        self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
        server = subprocess.Popen([str(BINARY)], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            server.stdin.write(frame({"jsonrpc": "2.0", "id": 42, "method": "initialize", "params": {}}))
            server.stdin.flush()
            initialized = read_message(server.stdout)
            self.assertEqual(initialized["id"], 42)
            self.assertEqual(initialized["result"]["serverInfo"]["name"], "shaftls")
            self.assertTrue(initialized["result"]["capabilities"]["textDocumentSync"]["openClose"])
            self.assertEqual(initialized["result"]["capabilities"]["textDocumentSync"]["change"], 2)

            server.stdin.write(frame({"jsonrpc": "2.0", "id": 2, "method": "shutdown", "params": {}}))
            server.stdin.flush()
            self.assertEqual(read_message(server.stdout), {"jsonrpc": "2.0", "id": 2, "result": None})
            server.stdin.write(frame({"jsonrpc": "2.0", "method": "exit", "params": {}}))
            server.stdin.flush()
            self.assertEqual(server.wait(timeout=3), 0)
        finally:
            if server.poll() is None:
                server.kill()
                server.wait()
            server.stdin.close()
            server.stdout.close()
            server.stderr.close()

    def test_exit_without_shutdown_uses_the_lsp_failure_status(self):
        build = subprocess.run([str(SHAFTC), "--build", "Shaft.build"], cwd=PROJECT, text=True, capture_output=True, check=False)
        self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
        server = subprocess.Popen([str(BINARY)], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            server.stdin.write(frame({"jsonrpc": "2.0", "method": "exit", "params": {}}))
            server.stdin.flush()
            self.assertEqual(server.wait(timeout=3), 1)
        finally:
            if server.poll() is None:
                server.kill()
                server.wait()
            server.stdin.close()
            server.stdout.close()
            server.stderr.close()

    def test_semantic_tokens_cover_imports_and_metaprogramming_macros(self):
        build = subprocess.run([str(SHAFTC), "--build", "Shaft.build"], cwd=PROJECT, text=True, capture_output=True, check=False)
        self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
        server = subprocess.Popen([str(BINARY)], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            uri = "file:///workspace/meta.shaft"
            messages = [
                {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {}},
                {"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {"textDocument": {"uri": uri, "languageId": "shaft", "version": 1, "text": 'import "shared.shaft";\n@config.build.target = "x86_64"\n@asm\nret\n@end\n'}}},
                {"jsonrpc": "2.0", "id": 2, "method": "textDocument/semanticTokens/full", "params": {"textDocument": {"uri": uri}}},
                {"jsonrpc": "2.0", "method": "exit", "params": {}},
            ]
            for message in messages:
                server.stdin.write(frame(message))
            server.stdin.flush()
            output, error = server.communicate(timeout=3)
            self.assertEqual(server.returncode, 1, error.decode())
            responses = []
            while output:
                header, output = output.split(b"\r\n\r\n", 1)
                length = int(next(line.split(b":", 1)[1].strip() for line in header.split(b"\r\n") if line.lower().startswith(b"content-length:")))
                body, output = output[:length], output[length:]
                responses.append(json.loads(body.decode("utf-8")))
            tokens = next(response["result"]["data"] for response in responses if response.get("id") == 2)
            self.assertEqual(tokens, [
                0, 0, 6, 0, 0,   # import
                0, 7, 14, 4, 0,  # "shared.shaft"
                1, 0, 7, 0, 0,   # @config
                1, 0, 4, 0, 0,   # @asm
                1, 0, 3, 7, 0,   # ret
                1, 0, 4, 0, 0,   # @end
            ])
        finally:
            if server.poll() is None:
                server.kill()
                server.wait()
            server.stdin.close()
            server.stdout.close()
            server.stderr.close()

    def test_semantic_tokens_highlight_assembly_instructions_registers_operands_and_comments(self):
        build = subprocess.run([str(SHAFTC), "--build", "Shaft.build"], cwd=PROJECT, text=True, capture_output=True, check=False)
        self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
        server = subprocess.Popen([str(BINARY)], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            uri = "file:///workspace/assembly.shaft"
            messages = [
                {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {}},
                {"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {"textDocument": {"uri": uri, "languageId": "shaft", "version": 1, "text": "cdef naked answer() -> i32\n{\n    @asm(value)\n        addl $2, $value // increment\n        movl %eax, %ebx\n    @end\n}\n"}}},
                {"jsonrpc": "2.0", "id": 2, "method": "textDocument/semanticTokens/full", "params": {"textDocument": {"uri": uri}}},
                {"jsonrpc": "2.0", "method": "exit", "params": {}},
            ]
            for message in messages:
                server.stdin.write(frame(message))
            server.stdin.flush()
            output, error = server.communicate(timeout=3)
            self.assertEqual(server.returncode, 1, error.decode())
            responses = []
            while output:
                header, output = output.split(b"\r\n\r\n", 1)
                length = int(next(line.split(b":", 1)[1].strip() for line in header.split(b"\r\n") if line.lower().startswith(b"content-length:")))
                body, output = output[:length], output[length:]
                responses.append(json.loads(body.decode("utf-8")))
            initialized = next(response["result"] for response in responses if response.get("id") == 1)
            self.assertEqual(initialized["capabilities"]["semanticTokensProvider"]["legend"]["tokenTypes"], ["keyword", "type", "function", "number", "string", "comment", "operator", "macro", "variable"])
            tokens = next(response["result"]["data"] for response in responses if response.get("id") == 2)
            self.assertEqual(tokens, [
                2, 4, 4, 0, 0,    # @asm
                1, 8, 4, 7, 0,    # addl
                0, 6, 1, 3, 0,    # 2
                0, 3, 6, 8, 0,    # $value
                0, 7, 12, 5, 0,   # // increment
                1, 8, 4, 7, 0,    # movl
                0, 5, 4, 8, 0,    # %eax
                0, 6, 4, 8, 0,    # %ebx
                1, 4, 4, 0, 0,    # @end
            ])
        finally:
            if server.poll() is None:
                server.kill()
                server.wait()
            server.stdin.close()
            server.stdout.close()
            server.stderr.close()

    def test_did_change_replaces_document_text_before_semantic_token_requests(self):
        build = subprocess.run([str(SHAFTC), "--build", "Shaft.build"], cwd=PROJECT, text=True, capture_output=True, check=False)
        self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
        server = subprocess.Popen([str(BINARY)], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            uri = "file:///workspace/change.shaft"
            messages = [
                {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {}},
                {"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {"textDocument": {"uri": uri, "languageId": "shaft", "version": 1, "text": 'import "old.shaft";\n'}}},
                {"jsonrpc": "2.0", "method": "textDocument/didChange", "params": {"textDocument": {"uri": uri, "version": 2}, "contentChanges": [{"text": "  @asm\n@end\n"}]}},
                {"jsonrpc": "2.0", "id": 2, "method": "textDocument/semanticTokens/full", "params": {"textDocument": {"uri": uri}}},
                {"jsonrpc": "2.0", "method": "exit", "params": {}},
            ]
            for message in messages:
                server.stdin.write(frame(message))
            server.stdin.flush()
            output, error = server.communicate(timeout=3)
            self.assertEqual(server.returncode, 1, error.decode())
            responses = []
            while output:
                header, output = output.split(b"\r\n\r\n", 1)
                length = int(next(line.split(b":", 1)[1].strip() for line in header.split(b"\r\n") if line.lower().startswith(b"content-length:")))
                body, output = output[:length], output[length:]
                responses.append(json.loads(body.decode("utf-8")))
            tokens = next(response["result"]["data"] for response in responses if response.get("id") == 2)
            self.assertEqual(tokens, [0, 2, 4, 0, 0, 1, 0, 4, 0, 0])
        finally:
            if server.poll() is None:
                server.kill()
                server.wait()
            server.stdin.close()
            server.stdout.close()
            server.stderr.close()

    def test_publish_diagnostics_highlights_unmatched_and_unclosed_braces(self):
        build = subprocess.run([str(SHAFTC), "--build", "Shaft.build"], cwd=PROJECT, text=True, capture_output=True, check=False)
        self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
        server = subprocess.Popen([str(BINARY)], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            uri = "file:///workspace/structure.shaft"
            messages = [
                {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {}},
                {"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {"textDocument": {"uri": uri, "languageId": "shaft", "version": 1, "text": "{\n}\n}\n"}}},
                {"jsonrpc": "2.0", "method": "textDocument/didChange", "params": {"textDocument": {"uri": uri, "version": 2}, "contentChanges": [{"text": "{\n"}]}},
                {"jsonrpc": "2.0", "method": "exit", "params": {}},
            ]
            for message in messages:
                server.stdin.write(frame(message))
            server.stdin.flush()
            output, error = server.communicate(timeout=3)
            self.assertEqual(server.returncode, 1, error.decode())
            notifications = []
            while output:
                header, output = output.split(b"\r\n\r\n", 1)
                length = int(next(line.split(b":", 1)[1].strip() for line in header.split(b"\r\n") if line.lower().startswith(b"content-length:")))
                body, output = output[:length], output[length:]
                message = json.loads(body.decode("utf-8"))
                if message.get("method") == "textDocument/publishDiagnostics":
                    notifications.append(message["params"])
            self.assertEqual(notifications[0], {
                "uri": uri,
                "diagnostics": [{
                    "range": {"start": {"line": 2, "character": 0}, "end": {"line": 2, "character": 1}},
                    "severity": 1,
                    "source": "shaftls",
                    "message": "Unmatched closing brace",
                }],
            })
            self.assertEqual(notifications[1], {
                "uri": uri,
                "diagnostics": [{
                    "range": {"start": {"line": 0, "character": 0}, "end": {"line": 0, "character": 1}},
                    "severity": 1,
                    "source": "shaftls",
                    "message": "Unclosed opening brace",
                }],
            })
        finally:
            if server.poll() is None:
                server.kill()
                server.wait()
            server.stdin.close()
            server.stdout.close()
            server.stderr.close()

    def test_did_change_applies_ranged_edits_to_stored_document_text(self):
        build = subprocess.run([str(SHAFTC), "--build", "Shaft.build"], cwd=PROJECT, text=True, capture_output=True, check=False)
        self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
        server = subprocess.Popen([str(BINARY)], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            uri = "file:///workspace/range-change.shaft"
            messages = [
                {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {}},
                {"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {"textDocument": {"uri": uri, "languageId": "shaft", "version": 1, "text": 'import "old.shaft";\n@asm\n@end\n'}}},
                {"jsonrpc": "2.0", "method": "textDocument/didChange", "params": {"textDocument": {"uri": uri, "version": 2}, "contentChanges": [{"range": {"start": {"line": 0, "character": 0}, "end": {"line": 0, "character": 6}}, "text": "@config"}]}},
                {"jsonrpc": "2.0", "id": 2, "method": "textDocument/semanticTokens/full", "params": {"textDocument": {"uri": uri}}},
                {"jsonrpc": "2.0", "method": "exit", "params": {}},
            ]
            for message in messages:
                server.stdin.write(frame(message))
            server.stdin.flush()
            output, error = server.communicate(timeout=3)
            self.assertEqual(server.returncode, 1, error.decode())
            responses = []
            while output:
                header, output = output.split(b"\r\n\r\n", 1)
                length = int(next(line.split(b":", 1)[1].strip() for line in header.split(b"\r\n") if line.lower().startswith(b"content-length:")))
                body, output = output[:length], output[length:]
                responses.append(json.loads(body.decode("utf-8")))
            tokens = next(response["result"]["data"] for response in responses if response.get("id") == 2)
            self.assertEqual(tokens, [0, 0, 7, 0, 0, 1, 0, 4, 0, 0, 1, 0, 4, 0, 0])
        finally:
            if server.poll() is None:
                server.kill()
                server.wait()
            server.stdin.close()
            server.stdout.close()
            server.stderr.close()


if __name__ == "__main__":
    unittest.main()
