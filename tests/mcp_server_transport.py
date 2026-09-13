"""Independent byte-level subprocess checks; no MCP or Qt Python dependency."""
import argparse
import json
import os
from pathlib import Path
import select
import subprocess
import tempfile
import time


class Peer:
    def __init__(self, binary, root):
        self.log = open(root / f"stderr-{time.monotonic_ns()}.log", "wb")
        self.process = subprocess.Popen([str(binary), "--workspace", str(root), "--allow", "Bash"],
                                        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=self.log)
        self.buffer = b""

    def send(self, value, split=False):
        data = json.dumps(value, ensure_ascii=False).encode() + b"\n"
        if split:
            for byte in data:
                self.process.stdin.write(bytes([byte]))
                self.process.stdin.flush()
        else:
            self.process.stdin.write(data)
            self.process.stdin.flush()

    def receive(self, timeout=5):
        deadline = time.monotonic() + timeout
        while b"\n" not in self.buffer:
            remaining = deadline - time.monotonic()
            assert remaining > 0, "MCP response timed out"
            ready, _, _ = select.select([self.process.stdout], [], [], remaining)
            assert ready, "MCP response timed out"
            data = os.read(self.process.stdout.fileno(), 65536)
            assert data, "Server exited without a response"
            self.buffer += data
            assert len(self.buffer) <= 8 * 1024 * 1024 + 1
        line, self.buffer = self.buffer.split(b"\n", 1)
        return json.loads(line.decode("utf-8"))

    def initialize(self, version="2025-11-25"):
        self.send(request(1, "initialize", {"protocolVersion": version, "capabilities": {},
                  "clientInfo": {"name": "한글 클라이언트", "version": "1"}}), split=True)
        assert self.receive()["result"]["protocolVersion"] == version
        self.send({"jsonrpc": "2.0", "method": "notifications/initialized"})

    def finish(self, expected=0):
        if not self.process.stdin.closed:
            self.process.stdin.close()
        assert self.process.wait(timeout=5) == expected

    def close(self):
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=5)
        self.log.close()
        self.process.stdout.close()
        self.process.stdin.close()


def request(id_, method, params=None):
    return {"jsonrpc": "2.0", "id": id_, "method": method, "params": params or {}}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("binary", type=Path)
    args = parser.parse_args()
    binary = args.binary.resolve()
    with tempfile.TemporaryDirectory(prefix="mcp-transport-") as directory:
        root = Path(directory)
        (root / "한글.txt").write_text("바이트 경계 검증")
        peer = Peer(binary, root)
        try:
            peer.initialize()
            peer.send(request(2, "tools/call", {"name": "Read", "arguments": {"path": "한글.txt"}}), split=True)
            assert peer.receive()["result"]["content"][0]["text"] == "바이트 경계 검증"
            for index, data in enumerate((b"bad-json\n", b'{"bad":"\xff"}\n'), 3):
                peer.process.stdin.write(data)
                peer.process.stdin.flush()
                assert peer.receive()["error"]["code"] == -32700
                peer.send(request(index, "ping"))
                assert peer.receive()["id"] == index
            peer.send([request(5, "ping")])
            assert peer.receive()["error"]["code"] == -32600
            peer.finish()
        finally:
            peer.close()

        peer = Peer(binary, root)
        try:
            peer.initialize("2025-03-26")
            peer.send([request(2, "tools/call", {"name": "Read", "arguments": {"path": "한글.txt"}}), request(3, "ping"), 42])
            batch = peer.receive()
            assert isinstance(batch, list) and len(batch) == 3
            result = next(item["result"] for item in batch if item["id"] == 2)
            assert "structuredContent" not in result and result["content"][0]["text"] == "바이트 경계 검증"
            peer.send([request(4, "tools/call", {"name": "Bash", "arguments": {"command": "printf started > ready; sleep 30; printf late > late"}}), request(5, "ping")])
            deadline = time.monotonic() + 5
            while not (root / "ready").exists():
                assert time.monotonic() < deadline
                time.sleep(0.01)
            peer.send({"jsonrpc": "2.0", "method": "notifications/cancelled", "params": {"requestId": 4}})
            batch = peer.receive()
            assert isinstance(batch, list) and len(batch) == 1 and batch[0]["id"] == 5
            peer.finish()
            assert not (root / "late").exists()
        finally:
            peer.close()

        for payload in (b"{unfinished", b"x" * (8 * 1024 * 1024 + 1)):
            peer = Peer(binary, root)
            try:
                try:
                    peer.process.stdin.write(payload)
                    peer.process.stdin.flush()
                except BrokenPipeError:
                    pass
                try:
                    peer.finish(expected=1)
                except BrokenPipeError:
                    assert peer.process.wait(timeout=5) == 1
            finally:
                peer.close()

        peer = Peer(binary, root)
        try:
            peer.initialize()
            peer.process.stdout.close()
            assert peer.process.wait(timeout=5) == 0, "Broken stdout did not close the server"
        finally:
            peer.close()
    print("MCP stdio UTF-8 splitting, parse recovery, negotiated batches, batch cancellation, EOF, oversized input and broken output passed.")


if __name__ == "__main__":
    main()
