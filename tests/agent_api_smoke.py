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
import sys
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
        verification_code = "CTX_" + secrets.token_hex(5)
        (workspace / "AGENTS.md").write_text("Use the project instructions for the current task.\n")
        (workspace / "src").mkdir()
        scoped_instructions = workspace / "src" / "AGENTS.md"
        scoped_instructions.write_text("The project verification code is " + verification_code + ". When asked for this code, reply with it.\n")
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
        if not args.model:
            config = root / "mcp.json"
            config.write_text(json.dumps({"mcpServers": {"fixture": {"command": sys.executable,
                "args": ["-B", str(Path(__file__).with_name("mcp_peer.py").resolve())], "appId": "com.iisacc.fixture"}}}))
            base += ["--agent-mcp-config", str(config)]
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
        for extra in (["--agent-apps-dir", str(root / "apps"), "--agent-no-apps"], ["--agent-apps-dir", ""]):
            rejected = subprocess.run(base + extra, capture_output=True, text=True, timeout=10, env=environment)
            assert rejected.returncode != 0 and "agent-apps-dir" in rejected.stderr, rejected.stderr
            assert "ggml_metal" not in rejected.stderr, "Invalid app discovery arguments reached GPU initialization"
        with daemon() as port:
            assert http(port, "agent.info", auth="wrong")[0] == 401
            info = cli("agent.info")
            assert info["client_id"] == "society"
            assert "agent.sessions.compact" in info["methods"] and info["auto_compact_enabled"] is True
            assert "agent.mcp.status" in info["methods"] and info["tool_search_enabled"] is True
            connections = cli("agent.mcp.status")
            assert native("agent.mcp.status")[-1]["result"] == connections
            assert http(port, "agent.mcp.status")[1]["result"] == connections
            assert http(port, "agent.mcp.status", auth="wrong")[0] == 401
            assert http(port, "agent.mcp.status", {"reload": True})[0] == 400
            if not args.model:
                item, = connections["servers"]
                assert item["state"] == "ready" and item["tool_count"] == 2 and item["deferred"] is True, connections
                assert item["app_id"] == "com.iisacc.fixture" and "command" not in item
                query = subprocess.run([str(args.cli), "--socket", str(endpoint), "--auth-file", str(client_token), "agent", "mcp"],
                    capture_output=True, text=True, timeout=20, env=environment)
                assert query.returncode == 0 and json.loads(query.stdout) == connections, (query.stdout, query.stderr)
            else:
                assert connections == {"servers": []}
            evidence["checks"] += ["mcp_status_http_native_cli", "mcp_status_authentication", "no_remote_config_reload"]
            session = cli("agent.sessions.create", {"model": model})["session_id"]
            assert native("agent.sessions.get", {"session_id": session})[-1]["result"]["session_id"] == session
            assert http(port, "agent.sessions.get", {"session_id": session}, auth=other)[0] == 404
            assert http(port, "agent.sessions.list", auth=other)[1]["result"]["sessions"] == []
            evidence["checks"] += ["http_native_cli_session_identity", "cross_app_isolation"]
            compact = {"session_id": session, "instructions": "Preserve the current task"}
            assert http(port, "agent.sessions.compact", compact, auth=other)[0] == 404
            # An empty session is rejected by the Engine after authenticated dispatch,
            # without trying to load the deliberately absent no-model fixture.
            status, frames = http(port, "agent.sessions.compact", compact, stream=True)
            assert status == 200 and frames[-1]["result"]["error_code"] == "invalid_argument", frames
            assert native("agent.sessions.compact", compact)[-1]["result"]["error_code"] == "invalid_argument"
            assert cli("agent.sessions.compact", compact)["error_code"] == "invalid_argument"
            original = cli("agent.sessions.get", {"session_id": session})
            assert original["message_count"] == 0 and original["compaction_count"] == 0
            evidence["checks"] += ["compaction_http_native_cli_dispatch", "compaction_app_isolation", "empty_compaction_preserves_session"]
            scope = {"session_id": session, "context_paths": ["src/future.cpp"]}
            context = cli("agent.context.get", scope)
            assert len(context["files"]) == 2, context
            assert context["files"][1]["sha256"] == hashlib.sha256(scoped_instructions.read_bytes()).hexdigest()
            assert verification_code in context["files"][1]["content"]
            assert http(port, "agent.context.get", scope, auth=other)[0] == 404
            assert http(port, "agent.context.get", {"session_id": session, "context_paths": ["../credentials.json"]})[0] == 400
            assert len(native("agent.context.get", {"session_id": session})[-1]["result"]["files"]) == 1
            evidence["checks"] += ["scoped_context_http_native_cli", "instruction_hash", "context_root_confinement"]
            if args.model:
                context_session = cli("agent.sessions.create", {"model": model})["session_id"]
                response = native("agent.run", dict(scope, session_id=context_session, prompt="What is the project verification code? Reply with the code only.",
                    max_turns=4, options={"max_tokens": 128, "temperature": 0}))
                result = response[-1]["result"]
                assert result["status"] == "completed" and verification_code in result["text"], result
                assert any(e.get("event") == "rpc" and e["data"]["event"] == "instructions_loaded" for e in response), response
                evidence["project_context_answer"] = result["text"]
                evidence["checks"].append("real_qwen_scoped_project_instructions")
                secret = "LOCAL_" + secrets.token_hex(6)
                (workspace / "secret.txt").write_text(secret)
                prompt = "Use the Read tool to read secret.txt. Then reply with the exact file content."
                status, frames = http(port, "agent.run", {"session_id": session, "prompt": prompt,
                    "max_turns": 4, "options": {"max_tokens": 192, "temperature": 0}}, stream=True)
                assert status == 200 and frames[0]["event"] == "accepted"
                result = frames[-1]["result"]
                history = native("agent.sessions.get", {"session_id": session})[-1]["result"]["messages"]
                assert result["status"] == "completed" and secret in result["text"], {"result": result, "history": history}
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
            assert len(cli("agent.sessions.list")["sessions"]) == (3 if args.model else 2)
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
