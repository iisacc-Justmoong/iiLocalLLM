"""Real daemon/CLI/HTTP/native IPC, private app identities and persisted sessions."""
from contextlib import contextmanager
import argparse
import hashlib
from http.client import HTTPConnection
import json
import os
from pathlib import Path
import re
import secrets
import socket
import subprocess
import tempfile
import time


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("daemon", type=Path)
    parser.add_argument("cli", type=Path)
    parser.add_argument("--model", type=Path)
    parser.add_argument("--report", type=Path)
    parser.add_argument("--startup-timeout", type=float, default=60)
    args = parser.parse_args()
    args.daemon = args.daemon.resolve()
    args.cli = args.cli.resolve()
    evidence = {"native_inference": bool(args.model), "daemon": str(args.daemon), "cli": str(args.cli), "checks": []}
    with tempfile.TemporaryDirectory(prefix="api-", dir=Path.cwd()) as directory:
        root = Path(directory)
        workspace = root / "work"
        workspace.mkdir()
        state = root / "private"
        credentials = root / "credentials.json"
        token, other = secrets.token_urlsafe(36), secrets.token_urlsafe(36)
        credentials.write_text(json.dumps({"society": token, "dreamscapes": other}))
        credentials.chmod(0o600)
        client_token = root / "client-token"
        client_token.write_text(token)
        client_token.chmod(0o600)
        endpoint = root / "s"
        catalog = root / "Models"
        catalog.mkdir()
        model = "fixture"
        if args.model:
            assert args.model.stat().st_size == 491400032
            assert hashlib.file_digest(args.model.open("rb"), "sha256").hexdigest() == "74a4da8c9fdbcd15bd1f6d01d621410d31c6fc00986f5eb687824e7b93d7a9db"
            package = catalog / "api-fixture"
            package.mkdir()
            os.link(args.model, package / "model.gguf")
            (package / "manifest.json").write_text(json.dumps({
                "schema_version": 1, "id": "api-fixture", "architecture": "qwen2", "format": "gguf", "quantization": "Q4_K_M",
                "context_length": 32768, "entry_point": "model.gguf", "capabilities": ["text-generation", "chat"],
                "files": [{"path": "model.gguf", "size": 491400032, "sha256": "74a4da8c9fdbcd15bd1f6d01d621410d31c6fc00986f5eb687824e7b93d7a9db"}]}))
            model = "model://api-fixture"
        base = [str(args.daemon), "--socket", str(endpoint), "--http-port", "0", "--models-root", str(catalog),
                "--context-tokens", "4096", "--agent-workspace", str(workspace), "--agent-state", str(state), "--agent-credentials", str(credentials)]
        environment = dict(os.environ)
        environment.pop("DYLD_LIBRARY_PATH", None)
        environment.pop("DYLD_FRAMEWORK_PATH", None)

        @contextmanager
        def daemon():
            log_path = root / f"daemon-{time.monotonic_ns()}.log"
            with log_path.open("w") as log:
                proc = subprocess.Popen(base, stdout=log, stderr=subprocess.STDOUT, env=environment)
                try:
                    started = time.monotonic()
                    deadline = started + args.startup_timeout
                    while time.monotonic() < deadline:
                        text = log_path.read_text()
                        match = re.search(r"iiLocalLLM HTTP: http://127.0.0.1:(\d+)", text)
                        if match and endpoint.exists():
                            evidence.setdefault("daemon_startup_seconds", []).append(round(time.monotonic() - started, 3))
                            yield int(match[1])
                            break
                        assert proc.poll() is None, text
                        time.sleep(0.02)
                    else:
                        raise AssertionError("daemon did not start")
                finally:
                    if proc.poll() is None:
                        proc.terminate()
                    try:
                        proc.wait(timeout=15)
                    except subprocess.TimeoutExpired:
                        proc.kill()
                        proc.wait(timeout=5)
                        raise
                    text = log_path.read_text()
                    assert token not in text and other not in text, "Credentials leaked to logs"
                    assert proc.returncode == 0, text[-2000:]

        def http(port, method, params=None, auth=token, stream=False):
            connection = HTTPConnection("127.0.0.1", port, timeout=120)
            try:
                connection.request("POST", "/v1/rpc", json.dumps({"id": "http", "method": method, "params": params or {}, "stream": stream}),
                                   {"Content-Type": "application/json", "Authorization": "Bearer " + auth})
                response = connection.getresponse()
                payload = response.read().decode()
                if stream:
                    frames = [json.loads(line[6:]) for line in payload.splitlines() if line.startswith("data: ") and line != "data: [DONE]"]
                    assert payload.endswith("data: [DONE]\n\n"), payload[-2000:]
                    return response.status, frames
                return response.status, json.loads(payload)
            finally:
                connection.close()

        def native(method, params=None, auth=token):
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:
                sock.settimeout(120)
                sock.connect(str(endpoint))
                sock.sendall(json.dumps({"id": "native", "method": method, "params": params or {}, "auth": auth}).encode() + b"\n")
                frames = []
                with sock.makefile("rb") as source:
                    for line in source:
                        value = json.loads(line)
                        frames.append(value)
                        if "result" in value or "error" in value:
                            return frames
                raise AssertionError("native RPC disconnected")

        def cli(method, params=None, expect=0):
            command = [str(args.cli), "--socket", str(endpoint), "--auth-file", str(client_token), "rpc", method]
            if params is not None:
                path = root / "params.json"
                path.write_text(json.dumps(params))
                command.append(str(path))
            result = subprocess.run(command, capture_output=True, text=True, timeout=120, env=environment)
            assert result.returncode == expect, (result.stdout[-1000:], result.stderr[-1000:])
            assert token not in result.stdout + result.stderr
            return json.loads(result.stdout) if expect == 0 else result

        credentials.chmod(0o644)
        rejection_started = time.monotonic()
        rejected = subprocess.run(base, capture_output=True, text=True, timeout=10, env=environment)
        assert rejected.returncode != 0 and "private" in rejected.stderr, rejected.stderr
        assert "ggml_metal" not in rejected.stderr, "Invalid credentials reached GPU initialization"
        evidence["invalid_credential_rejection_seconds"] = round(time.monotonic() - rejection_started, 3)
        credentials.chmod(0o600)
        evidence["checks"].append("private_credentials_required")
        with daemon() as port:
            assert http(port, "agent.info", auth="wrong")[0] == 401
            assert cli("agent.info")["client_id"] == "society"
            session = cli("agent.sessions.create", {"model": model})["session_id"]
            assert native("agent.sessions.get", {"session_id": session})[-1]["result"]["session_id"] == session
            assert http(port, "agent.sessions.get", {"session_id": session}, auth=other)[0] == 404
            assert http(port, "agent.sessions.list", auth=other)[1]["result"]["sessions"] == []
            evidence["checks"] += ["http_native_cli_session_identity", "cross_app_isolation"]
            if args.model:
                secret = "LOCAL_" + secrets.token_hex(6)
                (workspace / "secret.txt").write_text(secret)
                prompt = "Use the Read tool to read secret.txt. Then reply with the exact file content."
                status, frames = http(port, "agent.run", {"session_id": session, "prompt": prompt,
                    "max_turns": 4, "options": {"max_tokens": 192, "temperature": 0}}, stream=True)
                assert status == 200 and frames[0]["event"] == "accepted"
                result = frames[-1]["result"]
                assert result["status"] == "completed" and secret in result["text"], result
                history = native("agent.sessions.get", {"session_id": session})[-1]["result"]["messages"]
                assert any(m["role"] == "tool" and secret in m["text"] for m in history), history
                sequence = [e["data"]["sequence"] for e in frames if e.get("event") == "rpc"]
                assert sequence == list(range(1, len(sequence) + 1)), sequence
                evidence.update({"answer": result["text"], "usage": result["usage"], "events": len(sequence)})
                evidence["checks"].append("real_qwen_read_observation_over_http_sse")
            fork = http(port, "agent.sessions.fork", {"session_id": session})[1]["result"]["session_id"]
            assert fork != session
            before = native("agent.sessions.get", {"session_id": session})[-1]["result"]
            client_token.chmod(0o644)
            cli("agent.info", expect=1)
            client_token.chmod(0o600)
        with daemon() as port:
            after = http(port, "agent.sessions.get", {"session_id": session})[1]["result"]
            assert after == before
            assert len(cli("agent.sessions.list")["sessions"]) == 2
            assert native("agent.sessions.get", {"session_id": fork})[-1]["result"]["message_count"] == before["message_count"]
            evidence["checks"] += ["daemon_restart_resume", "transcript_fork"]
            if args.model:
                result = cli("agent.run", {"session_id": fork, "prompt": "What text did you read from secret.txt? Repeat it.",
                    "max_turns": 4, "options": {"max_tokens": 128, "temperature": 0}})
                assert result["status"] == "completed" and secret in result["text"], result
                evidence["checks"].append("cli_fork_continuation")
        if os.name == "posix":
            assert state.stat().st_mode & 0o077 == 0
    if args.report:
        args.report.write_text(json.dumps(evidence, ensure_ascii=False, indent=2) + "\n")
    print(json.dumps(evidence, ensure_ascii=False))


if __name__ == "__main__":
    main()
