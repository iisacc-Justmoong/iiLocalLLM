"""Real daemon/CLI and authenticated MCP HTTP async hook ownership controls."""
from contextlib import contextmanager
from http.client import HTTPConnection
from pathlib import Path
import argparse
import json
import os
import re
import secrets
import shlex
import subprocess
import tempfile
import time
from urllib.parse import urlsplit


def wait_for(condition, seconds=10):
    deadline = time.monotonic() + seconds
    while True:
        value = condition()
        if value:
            return value
        assert time.monotonic() < deadline, "Async wire deadline expired"
        time.sleep(0.02)


def main():
    parser = argparse.ArgumentParser()
    for name in ("daemon", "cli", "mcp"):
        parser.add_argument(name, type=Path)
    parser.add_argument("--report", type=Path)
    args = parser.parse_args()
    binaries = [str(getattr(args, name).resolve()) for name in ("daemon", "cli", "mcp")]
    report = {"passed": False, "binaries": binaries}
    env = dict(os.environ)
    for name in ("DYLD_LIBRARY_PATH", "DYLD_FRAMEWORK_PATH", "DYLD_FALLBACK_LIBRARY_PATH", "LIBRARY_PATH"):
        env.pop(name, None)
    with tempfile.TemporaryDirectory(prefix="async-wire-") as temporary:
        root = Path(temporary)
        work = root / "work"
        work.mkdir()
        first, second = secrets.token_urlsafe(36), secrets.token_urlsafe(36)

        def private(name, value):
            path = root / name
            path.write_text(value if isinstance(value, str) else json.dumps(value))
            path.chmod(0o600)
            return str(path)

        credentials = private("credentials", {"society": first, "dreamscapes": second})
        auth = private("auth", first)
        script = private("hook.py", '''import json, pathlib, sys, time
root = pathlib.Path(__file__).parent
value = json.load(sys.stdin)
owner = value["session_id"]
if value["hook_event_name"] == "TaskCreated":
    print(json.dumps({"async": True}), flush=True)
(root / ("input-" + owner)).write_text(json.dumps(value))
while not (root / ("release-" + owner)).exists():
    time.sleep(0.01)
print(json.dumps({"continue": False, "systemMessage": "ASYNC_WIRE_CONTEXT"}))
''')
        import sys
        command = shlex.quote(sys.executable) + " -B " + shlex.quote(script)
        hook = {"type": "command", "command": command, "once": True, "timeout": 30}
        hooks = private("hooks.json", {"hooks": {
            "TaskCreated": [{"hooks": [hook]}],
            "PostToolUse": [{"matcher": "Read", "hooks": [dict(hook, **{"async": True})]}],
        }})

        @contextmanager
        def server(name, invocation, pattern):
            log_path = root / (name + ".log")
            with log_path.open("w") as log:
                process = subprocess.Popen(invocation, stdout=log, stderr=subprocess.STDOUT, env=env)
                try:
                    def ready():
                        contents = log_path.read_text()
                        assert process.poll() is None, contents[-4000:]
                        return re.search(pattern, contents)
                    yield wait_for(ready, 30)
                finally:
                    process.terminate()
                    try:
                        process.wait(timeout=10)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait()
                    if args.report:
                        args.report.with_suffix("." + name + ".log").write_bytes(log_path.read_bytes())

        def post(port, path, body, token=first, identity=None):
            headers = {"Authorization": "Bearer " + token, "Content-Type": "application/json", "Accept": "application/json, text/event-stream"}
            if identity:
                headers.update({"Mcp-Session-Id": identity, "Mcp-Protocol-Version": "2025-11-25"})
            connection = HTTPConnection("127.0.0.1", port, timeout=10)
            try:
                connection.request("POST", path, json.dumps(body), headers)
                response = connection.getresponse()
                raw = response.read()
                if response.getheader("Content-Type", "").startswith("text/event-stream"):
                    frames = [json.loads(line[5:]) for line in raw.splitlines() if line.startswith(b"data:") and line[5:].strip()]
                    data = next(v for v in frames if v.get("id") == body.get("id") and ("result" in v or "error" in v))
                else:
                    data = json.loads(raw) if raw else {}
                return response.status, data, response.getheader("Mcp-Session-Id")
            finally:
                connection.close()

        invocation = [binaries[0], "--socket", str(root / "s"), "--http-port", "0", "--models-root", str(root / "models"),
            "--agent-workspace", str(work), "--agent-state", str(root / "api-state"), "--agent-credentials", credentials,
            "--agent-hooks", hooks, "--agent-allow", "TaskCreate", "--agent-no-apps", "--agent-no-background", "--agent-no-skills", "--agent-no-subagents", "--no-agent-profiles"]
        with server("api", invocation, r"iiLocalLLM HTTP: http://127\.0\.0\.1:(\d+)") as match:
            port = int(match[1])
            def rpc(method, params, token=first, expected=200):
                status, result, _ = post(port, "/v1/rpc", {"id": secrets.token_hex(8), "method": method, "params": params}, token)
                assert status == expected, result
                return result.get("result", result)
            owner = rpc("agent.sessions.create", {"model": "model://fixture"})["session_id"]
            created = rpc("agent.tasks.create", {"session_id": owner, "subject": "Async task", "description": "Verify background hook publication"})
            assert not created["is_error"], created
            wait_for(lambda: (root / ("input-" + owner)).exists())
            records = rpc("agent.hooks.status", {"session_id": owner})["hooks"]
            assert len(records) == 1 and records[0]["state"] == "running", records
            rpc("agent.hooks.cancel", {"session_id": owner}, second, 404)
            rpc("agent.hooks.status", {"session_id": owner}, "invalid", 401)
            (root / ("release-" + owner)).touch()
            wait_for(lambda: rpc("agent.hooks.status", {"session_id": owner})["hooks"][0]["state"] != "running")
            finished = rpc("agent.hooks.status", {"session_id": owner})["hooks"][0]
            assert finished["state"] == "completed" and finished.get("delivery") == "accepted", finished
            wait_for(lambda: rpc("agent.inputs.list", {"session_id": owner})["count"] == 1)
            queued = rpc("agent.inputs.list", {"session_id": owner})["inputs"][0]
            assert queued["kind"] == "notification" and queued["text"] == "ASYNC_WIRE_CONTEXT", queued
            params_file = private("params.json", {"session_id": owner})
            outcome = subprocess.run([binaries[1], "--socket", str(root / "s"), "--auth-file", auth, "rpc", "agent.hooks.status", params_file], env=env, text=True, capture_output=True, timeout=10)
            assert outcome.returncode == 0 and json.loads(outcome.stdout)["hooks"][0]["state"] == "completed", outcome
            report["api_cli"] = {"dynamic_handshake": True, "isolated": True, "context_delivered": True, "late_stop_ignored": True}

        (work / "source.txt").write_text("ORIGINAL_OBSERVATION")
        invocation = [binaries[2], "--workspace", str(work), "--hooks", hooks, "--model", "model://fixture", "--models", str(root / "models"),
            "--no-apps", "--no-background", "--no-agent-profiles", "--no-subagents", "--no-skills", "--http-port", "0",
            "--credentials", credentials, "--state", str(root / "mcp-state")]
        with server("mcp", invocation, r'\{"endpoint":"([^"\n]+)"\}') as match:
            port = urlsplit(match[1]).port
            initialize = {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {"protocolVersion": "2025-11-25", "capabilities": {}, "clientInfo": {"name": "async-test", "version": "1"}}}
            _, response, identity = post(port, "/mcp", initialize)
            assert "iisacc/asyncHooks" in response["result"]["capabilities"]["experimental"], response
            _, _, other_identity = post(port, "/mcp", initialize, second)
            for token, sid in ((first, identity), (second, other_identity)):
                assert post(port, "/mcp", {"jsonrpc": "2.0", "method": "notifications/initialized"}, token, sid)[0] == 202
            def rpc(method, params, token=first, sid=identity):
                status, response, _ = post(port, "/mcp", {"jsonrpc": "2.0", "id": secrets.token_hex(8), "method": method, "params": params}, token, sid)
                assert status == 200, response
                return response
            result = rpc("tools/call", {"name": "Read", "arguments": {"path": "source.txt"}})["result"]
            assert not result["isError"] and "ORIGINAL_OBSERVATION" in json.dumps(result), result
            state = rpc("iisacc/hooks/status", {})["result"]
            owner = state["session_id"]
            wait_for(lambda: (root / ("input-" + owner)).exists())
            assert state["hooks"][0]["state"] == "running", state
            assert "error" in rpc("iisacc/hooks/cancel", {"hook_id": state["hooks"][0]["hook_id"]}, second, other_identity)
            assert "error" in rpc("iisacc/hooks/status", {"session_id": owner}, second, other_identity)
            assert rpc("iisacc/hooks/cancel", {})["result"]["cancelled_hooks"] == 1
            wait_for(lambda: rpc("iisacc/hooks/status", {})["result"]["hooks"][0]["state"] == "cancelled")
            report["mcp_http"] = {"configured_async": True, "capability": True, "isolated": True, "cancelled": True}
    report["passed"] = True
    if args.report:
        args.report.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report))


if __name__ == "__main__":
    main()
