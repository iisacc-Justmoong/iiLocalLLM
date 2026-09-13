"""Real daemon/client transport, pinned loopback pulls, cancellation, and shared inference."""
from contextlib import contextmanager
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import http.client
import json
import os
import pty
from pathlib import Path
import re
import select
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time


def main():
    daemon, cli = map(str, map(Path, sys.argv[1:3]))
    weights = Path(sys.argv[3]) if len(sys.argv) > 3 else None
    payload = weights.read_bytes() if weights else b"local registry test\n" * 32768
    counts = {}
    slow_started = threading.Event()

    class Downloads(BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def do_GET(self):
            counts[self.path] = counts.get(self.path, 0) + 1
            if self.path == "/redirect":
                self.send_response(302)
                self.send_header("Location", "http://example.invalid/model")
                self.end_headers()
                return
            self.send_response(200)
            self.send_header("Content-Length", str(len(payload)))
            self.end_headers()
            try:
                for start in range(0, len(payload), 16384):
                    self.wfile.write(payload[start:start + 16384])
                    self.wfile.flush()
                    if self.path == "/slow":
                        slow_started.set()
                        time.sleep(0.03)
            except (BrokenPipeError, ConnectionResetError):
                pass

    downloads = ThreadingHTTPServer(("127.0.0.1", 0), Downloads)
    downloads.daemon_threads = True
    thread = threading.Thread(target=downloads.serve_forever, daemon=True)
    thread.start()
    try:
        with tempfile.TemporaryDirectory(prefix="cli-", dir=Path.cwd()) as directory:
            root = Path(directory)
            endpoint = root / "llm.sock"
            storage = root / "Models"
            base = f"http://127.0.0.1:{downloads.server_port}"
            asset = "model.gguf" if weights else "model.test"

            def package(identifier, alias, path, digest=None, delta=0):
                return {"aliases": [alias], "manifest": {
                    "id": identifier, "architecture": "llama" if weights else "test", "format": "gguf" if weights else "test",
                    "quantization": "Q4_0" if weights else "none", "context_length": 512, "capabilities": ["chat", "text-generation"],
                    "entry_point": asset, "files": [{"path": asset, "size": len(payload) + delta,
                        "sha256": digest or hashlib.sha256(payload).hexdigest()}]}, "sources": {asset: base + path}}

            registry = root / "registry.json"
            registry.write_text(json.dumps({"models": [package("test", "qwen3:8b", "/model"),
                package("bad", "bad:checksum", "/bad", "0" * 64), package("short", "bad:short", "/short", delta=1),
                package("large", "bad:large", "/large", delta=-1), package("cancel", "slow:model", "/slow"),
                package("redirect", "bad:redirect", "/redirect")]}))
            environment = dict(os.environ, IILLM_SOCKET=str(endpoint))

            def command(*args, ok=True, **kwargs):
                result = subprocess.run([cli, *args], capture_output=True, text=True, env=environment, timeout=60, **kwargs)
                assert (result.returncode == 0) == ok, (args, result.returncode, result.stdout, result.stderr)
                return result

            # No daemon means a connection error; the CLI cannot start private inference.
            assert "Cannot connect" in command("run", "qwen3:8b", "hello", ok=False).stderr
            if sys.platform == "darwin":
                linkage = subprocess.check_output(["otool", "-L", cli], text=True)
                assert "libiiLocalLLM" not in linkage and "libllama" not in linkage and "libggml" not in linkage, linkage

            @contextmanager
            def running():
                with (root / "daemon.log").open("w+") as log:
                    child = subprocess.Popen([daemon, "--socket", str(endpoint), "--models-root", str(storage),
                        "--registry", str(registry), "--http-port", "0", "--keep-alive", "5m"], stdout=log, stderr=log)
                    try:
                        deadline = time.monotonic() + 30
                        while True:
                            text = (root / "daemon.log").read_text(errors="replace")
                            match = re.search(r"iiLocalLLM HTTP: http://127\.0\.0\.1:(\d+)", text)
                            if match:
                                break
                            assert child.poll() is None and time.monotonic() < deadline, text[-6000:]
                            time.sleep(0.02)
                        yield int(match[1])
                    finally:
                        child.terminate()
                        try:
                            assert child.wait(timeout=15) == 0
                        except subprocess.TimeoutExpired:
                            child.kill()
                            child.wait(timeout=5)
                            raise
                        assert not endpoint.exists()

            def rpc(method, params=None):
                with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
                    client.settimeout(30)
                    client.connect(str(endpoint))
                    client.sendall((json.dumps({"id": "test", "method": method, "params": params or {}}) + "\n").encode())
                    with client.makefile("rb") as stream:
                        result = json.loads(stream.readline())
                        assert "error" not in result, result
                        return result["result"]

            with running() as http_port:
                groups = json.loads(command("parameters", "--json").stdout)
                assert any(group["id"] == "trl.GRPOConfig" for group in groups), groups
                definition = json.loads(command("parameters", "llama.common_params_sampling").stdout)
                assert any(field["name"] == "samplers" for field in definition["parameters"])
                config = root / "training.json"
                config.write_text(json.dumps({"learning_rate": .0002, "per_device_train_batch_size": 2}))
                validated = json.loads(command("parameters", "transformers.TrainingArguments", str(config)).stdout)
                assert validated == json.loads(config.read_text()), validated
                config.write_text('{"learning_rate":"invalid"}')
                command("parameters", "transformers.TrainingArguments", str(config), ok=False)
                assert json.loads(command("models", "--json").stdout)["models"] == []
                assert json.loads(command("ps", "--json").stdout) == []
                installed = json.loads(command("pull", "qwen3:8b", "--json").stdout)
                assert installed["model"] == "model://test" and not installed["loaded"]
                assert (storage / "test" / asset).read_bytes() == payload
                assert counts["/model"] == 1
                command("pull", "qwen3:8b", "--json")
                assert counts["/model"] == 1, "Verified installed models should not download again"
                assert json.loads(command("models", "--json").stdout)["models"][0]["model"] == "model://test"
                assert json.loads(command("ps", "--json").stdout) == []
                for alias in ("bad:checksum", "bad:short", "bad:large", "bad:redirect"):
                    assert command("pull", alias, ok=False).returncode == 1
                assert not any((storage / name).exists() for name in ("bad", "short", "large", "redirect"))
                assert not list(storage.glob(".pull-*")), "Failed downloads must leave no staging files"
                command("pull", "model://unknown", ok=False)
                command("run", "../model.gguf", "hello", ok=False)
                for temperature in ("-1", "10.1", "nan", "inf", "invalid"):
                    error = command("run", "qwen3:8b", "hello", f"--temperature={temperature}", ok=False)
                    assert "temperature must be" in error.stderr, error.stderr
                assert rpc("stats")["sessions"] == 0
                cancel = subprocess.Popen([cli, "pull", "slow:model", "--json"], env=environment,
                                          stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
                try:
                    assert slow_started.wait(10)
                    cancel.send_signal(signal.SIGINT)
                    out, err = cancel.communicate(timeout=10)
                    assert cancel.returncode == 130, (out, err, cancel.returncode)
                finally:
                    if cancel.poll() is None:
                        cancel.kill()
                        cancel.wait(timeout=5)
                # Round trip after the disconnect waits for the worker to observe cancellation.
                command("models", "--json")
                assert not (storage / "cancel").exists() and not list(storage.glob(".pull-*"))
                # An idle interactive run is interruptible and closes its daemon session.
                master, slave = pty.openpty()
                interactive = subprocess.Popen([cli, "run", "qwen3:8b"], env=environment, stdin=slave,
                                               stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
                os.close(slave)
                try:
                    deadline = time.monotonic() + 10
                    while rpc("stats")["sessions"] == 0:
                        assert interactive.poll() is None and time.monotonic() < deadline
                        time.sleep(0.02)
                    # /clear is a local command, even when no usable runtime is installed.
                    os.write(master, b"/clear\n")
                    cleared = b""
                    while b"Conversation cleared" not in cleared:
                        assert interactive.poll() is None and time.monotonic() < deadline, cleared
                        if select.select([interactive.stderr], [], [], 0.1)[0]:
                            cleared += os.read(interactive.stderr.fileno(), 4096)
                    assert rpc("stats")["sessions"] == 1
                    interactive.send_signal(signal.SIGINT)
                    out, err = interactive.communicate(timeout=5)
                    assert interactive.returncode == 130, (out, err, interactive.returncode)
                    assert rpc("stats")["sessions"] == 0
                finally:
                    if interactive.poll() is None:
                        interactive.kill()
                        interactive.wait(timeout=5)
                    os.close(master)
                if weights:
                    # TinyStories lacks a template. Host-side configuration is explicit and remembered across eviction.
                    rpc("models.load", {"model": "qwen3:8b", "context_tokens": 512, "options": {"chat_template": "chatml"}})
                    rpc("models.unload", {"model": "qwen3:8b"})
                    run_args = ("run", "qwen3:8b", "Tell a story about a bird.", "--max-tokens", "12", "--temperature", "0", "--json")
                    config.write_text(json.dumps({"min_p": .05, "repetition_penalty": 1.05, "frequency_penalty": .1}))
                    parameterized = json.loads(command(*run_args, "--options", str(config)).stdout)
                    assert "error" not in parameterized and parameterized["finish_reason"] in ("stop", "length"), parameterized
                    result = json.loads(command(*run_args).stdout)
                    assert result["text"] and result["finish_reason"] in ("length", "stop")
                    assert json.loads(command(*run_args).stdout)["text"] == result["text"], "Greedy CLI generation must be repeatable"
                    residents = json.loads(command("ps", "--json").stdout)
                    assert len(residents) == 1 and residents[0]["model"] == "model://test"
                    assert residents[0]["memory"]["estimated_bytes"] > len(payload)
                    assert residents[0]["memory"]["context_bytes"] > 0
                    assert residents[0]["keep_alive_ms"] == 300000
                    before = rpc("stats")
                    connection = http.client.HTTPConnection("127.0.0.1", http_port, timeout=20)
                    connection.request("POST", "/v1/chat/completions", json.dumps({"model": "qwen3:8b",
                        "messages": [{"role": "user", "content": "Tell a story."}], "max_tokens": 12}),
                        {"Content-Type": "application/json"})
                    response = connection.getresponse()
                    body = response.read()
                    assert response.status == 200, body
                    connection.close()
                    command("run", "qwen3:8b", "--max-tokens", "12", input="Tell a story about a cat.\n")
                    after = rpc("stats")
                    assert after["model_loads"] == before["model_loads"], (before, after)
                    assert after["sessions"] == 0, "CLI and HTTP temporary sessions should be closed"
                    command("run", "qwen3:8b", "Goodbye.", "--max-tokens", "8", "--keep-alive", "0")
                    assert json.loads(command("ps", "--json").stdout) == []
                    assert rpc("stats")["cached_contexts"] == 0
            with running():
                before = counts["/model"]
                assert len(json.loads(command("models", "--json").stdout)["models"]) == 1
                command("pull", "qwen3:8b", "--json")
                assert counts["/model"] == before
                assert json.loads(command("ps", "--json").stdout) == []
            print("CLI client-only linkage, pinned pull, failure cleanup, cancellation and restart passed"
                  + ("; CLI/HTTP shared instance and keep_alive=0 inference passed" if weights else ""))
    finally:
        downloads.shutdown()
        downloads.server_close()
        thread.join(timeout=5)


if __name__ == "__main__":
    main()
