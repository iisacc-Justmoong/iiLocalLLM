"""Real command hooks through authenticated API, CLI, MCP and optional native inference."""
import argparse
import asyncio
from contextlib import contextmanager
from http.client import HTTPConnection
import json
import os
from pathlib import Path
import re
import secrets
import shlex
import shutil
import subprocess
import sys
import tempfile
import time
from urllib.parse import urlsplit


def main():
    parser = argparse.ArgumentParser()
    for name in ("daemon", "cli", "mcp"):
        parser.add_argument(name, type=Path)
    parser.add_argument("--catalog", type=Path)
    parser.add_argument("--model", default="model://qwen3-8b-q4")
    parser.add_argument("--official-stdio", action="store_true")
    parser.add_argument("--report", type=Path)
    args = parser.parse_args()
    daemon, cli, mcp = (str(getattr(args, key).resolve()) for key in ("daemon", "cli", "mcp"))
    report = {"passed": False, "binaries": [daemon, cli, mcp], "inference": bool(args.catalog)}
    env = dict(os.environ)
    for key in ("DYLD_LIBRARY_PATH", "DYLD_FRAMEWORK_PATH", "DYLD_FALLBACK_LIBRARY_PATH", "LIBRARY_PATH"):
        env.pop(key, None)
    with tempfile.TemporaryDirectory(prefix="hooks-wire-") as temporary:
        root = Path(temporary)
        workspace = root / "work"
        workspace.mkdir()
        token, other = secrets.token_urlsafe(36), secrets.token_urlsafe(36)

        def private(name, value):
            path = root / name
            path.write_text(value if isinstance(value, str) else json.dumps(value))
            path.chmod(0o600)
            return str(path)

        credentials = private("credentials", {"society": token, "dreamscapes": other})
        auth = private("auth", token)
        policy = private("permissions", {"enabled_sources": [], "settings": {"permissions": {
            "defaultMode": "dontAsk", "deny": ["Edit(/denied.txt)"], "ask": ["Edit(/ask.txt)"]}}})
        script = private("hook.py", '''import json, pathlib, sys
root = pathlib.Path(__file__).parent
value = json.load(sys.stdin)
event = value["hook_event_name"]
with (root / "events.jsonl").open("a") as f:
    f.write(json.dumps(value) + "\\n")
if event == "PreToolUse":
    if (root / "block").exists():
        print("HOOK_BLOCK", file=sys.stderr)
        sys.exit(2)
    result = {"hookEventName": event, "permissionDecision": "allow"}
    if value["tool_input"]["path"] == "rewrite.txt":
        result["updatedInput"] = {"path": "rewritten.txt", "content": "REWRITTEN"}
    print(json.dumps({"hookSpecificOutput": result}))
elif event == "TaskCreated" and (root / "task-block").exists():
    print("TASK_BLOCK", file=sys.stderr)
    sys.exit(2)
elif event == "Stop" and (root / "stop").exists():
    print(json.dumps({"continue": False, "stopReason": "HOST_STOP"}))
''')
        command = shlex.join([sys.executable, "-B", script])
        settings = {"hooks": {event: [{"matcher": "Write" if "ToolUse" in event else "*",
            "hooks": [{"type": "command", "command": command, "timeout": 10}]}]
            for event in ("PreToolUse", "PostToolUse", "PostToolUseFailure", "TaskCreated", "TaskCompleted", "Stop")}}
        hooks = private("hooks", settings)
        common_daemon = [daemon, "--socket", str(root / "s"), "--http-port", "0", "--models-root", str(root / "models"),
            "--agent-workspace", str(workspace), "--agent-state", str(root / "api-state"), "--agent-credentials", credentials,
            "--agent-no-apps", "--agent-no-background", "--agent-no-skills", "--agent-no-subagents", "--no-agent-profiles"]
        common_mcp = [mcp, "--workspace", str(workspace), "--no-apps", "--no-background", "--no-agent-profiles"]
        invalid = 0
        for value in ([], {}, {"hooks": {"SessionStart": []}}, {"hooks": {"PreToolUse": [{"hooks": [{"type": "http"}]}]}},
                      {"hooks": {"Stop": [{"hooks": [{"type": "command", "command": "true", "async": True}]}]}},
                      {"hooks": {"Stop": [{"hooks": [{"type": "command", "command": 1}]}]}}):
            bad = private("invalid", value)
            for invocation in (common_daemon + ["--agent-hooks", bad],
                               common_mcp + ["--hooks", bad, "--model", args.model, "--models", str(root / "models")]):
                process = subprocess.run(invocation, env=env, capture_output=True, timeout=15)
                assert process.returncode and not process.stdout, process
                assert not (root / "models").exists(), "Invalid hooks reached model initialization"
                invalid += 1
        for bad in (Path(private("public-hooks", settings)), workspace / "hooks.json"):
            bad.write_text(json.dumps(settings)); bad.chmod(0o644 if bad.parent == root else 0o600)
            for invocation in (common_daemon + ["--agent-hooks", str(bad)], common_mcp + ["--hooks", str(bad)]):
                assert subprocess.run(invocation, env=env, capture_output=True, timeout=15).returncode
                invalid += 1
        report["invalid_host_cases"] = invalid
        extra = []
        if args.catalog:
            source = args.catalog.resolve() / args.model.removeprefix("model://")
            manifest = json.loads((source / "manifest.json").read_text())
            package = root / "models" / manifest["id"]
            for entry in manifest["files"]:
                target = package / entry["path"]
                target.parent.mkdir(parents=True, exist_ok=True)
                os.link(source / entry["path"], target)
            (package / "manifest.json").write_text(json.dumps(manifest))
            report["model_manifest"] = manifest
            model_config = private("model-config", {"models": [{"model": args.model, "context_tokens": 8192,
                "options": {"enable_thinking": False, "tool_grammar": False}}]})
            extra = ["--config", model_config, "--agent-no-auto-compact"]

        @contextmanager
        def server(name, invocation, pattern):
            log_path = root / (name + ".log")
            with log_path.open("w") as log:
                process = subprocess.Popen(invocation, env=env, stdout=log, stderr=log)
                try:
                    start = time.monotonic()
                    while True:
                        contents = log_path.read_text(errors="replace")
                        match = re.search(pattern, contents)
                        if match:
                            report[name + "_startup_seconds"] = round(time.monotonic() - start, 3)
                            yield match
                            break
                        assert process.poll() is None and time.monotonic() - start < (300 if args.catalog else 30), contents[-3000:]
                        time.sleep(0.02)
                finally:
                    process.terminate()
                    try:
                        process.wait(timeout=15)
                    except subprocess.TimeoutExpired:
                        process.kill(); process.wait()
                    if args.report:
                        shutil.copyfile(log_path, args.report.with_suffix("." + name + ".log"))

        def post(port, path, body, bearer=token, session=None):
            headers = {"Content-Type": "application/json", "Accept": "application/json, text/event-stream", "Authorization": "Bearer " + bearer}
            if session:
                headers.update({"Mcp-Session-Id": session, "Mcp-Protocol-Version": "2025-11-25"})
            connection = HTTPConnection("127.0.0.1", port, timeout=180)
            try:
                connection.request("POST", path, json.dumps(body), headers)
                response = connection.getresponse(); raw = response.read(); frames = []
                if response.getheader("Content-Type", "").startswith("text/event-stream"):
                    frames = [json.loads(line[5:]) for line in raw.splitlines() if line.startswith(b"data:") and line[5:].strip()]
                    data = next(m for m in frames if m.get("id") == body.get("id") and ("result" in m or "error" in m))
                else:
                    data = json.loads(raw) if raw else {}
                return response.status, data, response.getheader("Mcp-Session-Id"), frames
            finally:
                connection.close()

        with server("api", common_daemon + ["--agent-hooks", hooks, "--agent-permission-settings", policy] + extra,
                    r"iiLocalLLM HTTP: http://127\.0\.0\.1:(\d+)") as match:
            port = int(match[1])

            def rpc(method, params, bearer=token, expected=200):
                status, data, _, _ = post(port, "/v1/rpc", {"id": secrets.token_hex(8), "method": method, "params": params}, bearer)
                assert status == expected, data
                return data.get("result", data)

            info = rpc("agent.info", {})
            assert info["hooks_enabled"] and command not in json.dumps(info)
            cli_result = subprocess.run([cli, "--socket", str(root / "s"), "--auth-file", auth, "rpc", "agent.info"],
                env=env, text=True, capture_output=True, timeout=15)
            assert cli_result.returncode == 0 and json.loads(cli_result.stdout) == info, cli_result
            owner = rpc("agent.sessions.create", {"model": args.model})["session_id"]
            rpc("agent.sessions.get", {"session_id": owner}, other, 404)
            rpc("agent.info", {}, "invalid", 401)
            rpc("agent.sessions.create", {"model": args.model, "hooks": settings}, expected=400)
            task = rpc("agent.tasks.create", {"session_id": owner, "subject": "HOOK_TASK", "description": "COMMAND_VERIFIED"})
            assert not task["is_error"], task
            (root / "task-block").touch()
            blocked = rpc("agent.tasks.create", {"session_id": owner, "subject": "BLOCKED_TASK", "description": "MUST_NOT_COMMIT"})
            assert blocked["is_error"] and "TASK_BLOCK" in blocked["text"], blocked
            listed = rpc("agent.tasks.list", {"session_id": owner})
            assert "BLOCKED_TASK" not in json.dumps(listed) and "HOOK_TASK" in json.dumps(listed), listed
            (root / "task-block").unlink()
            completed = rpc("agent.tasks.update", {"session_id": owner, "taskId": task["result"]["task"]["id"], "status": "completed"})
            assert not completed["is_error"], completed
            report["api"] = {"hooks_enabled": True, "cli_matches": True, "authentication": True, "task_commit_veto": True, "task_completed": True}
            if args.catalog:
                results = []
                for allowed in (True, False):
                    if not allowed:
                        (root / "block").touch()
                    session = rpc("agent.sessions.create", {"model": args.model})["session_id"]
                    path = "native-allowed.txt" if allowed else "native-blocked.txt"
                    content = "HOOK_" + secrets.token_hex(8)
                    outcome = rpc("agent.run", {"session_id": session, "prompt": f'Call Write once with path="{path}" and content="{content}". Do not add a newline. Return DONE on success, or DENIED when the tool is blocked.',
                        "max_turns": 4, "options": {"temperature": 0, "max_tokens": 1024}})
                    assert outcome["status"] == "completed", outcome
                    history = rpc("agent.sessions.get", {"session_id": session})["messages"]
                    calls = [c for m in history for c in m["tool_calls"] if c["name"] == "Write"]
                    tool_results = [m for m in history if m["role"] == "tool" and m["tool_call_id"] in {c["id"] for c in calls}]
                    assert calls and len(calls) == len(tool_results), history
                    if allowed:
                        assert (workspace / path).read_text() == content, outcome
                    else:
                        assert not (workspace / path).exists() and any(m.get("is_error") and "HOOK_BLOCK" in m["text"] for m in tool_results), outcome
                    results.append({"allowed": allowed, "outcome": outcome, "write_calls": len(calls), "tool_results": tool_results})
                (root / "block").unlink()
                (root / "stop").touch()
                session = rpc("agent.sessions.create", {"model": args.model})["session_id"]
                stopped = rpc("agent.run", {"session_id": session, "prompt": "Reply HELLO without any tool call.", "max_turns": 2,
                    "options": {"temperature": 0, "max_tokens": 64}})
                assert stopped["status"] == "cancelled" and "HOST_STOP" in json.dumps(stopped), stopped
                (root / "stop").unlink()
                report["model_results"] = results; report["model_stop"] = stopped

        mcp_flags = ["--hooks", hooks, "--permission-settings", policy]
        with server("mcp", common_mcp + mcp_flags + ["--http-port", "0", "--credentials", credentials, "--state", str(root / "mcp-state")],
                    r'\{"endpoint":"([^"\n]+)"\}') as match:
            url = urlsplit(match[1])
            initialize = {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {"protocolVersion": "2025-11-25", "capabilities": {},
                "clientInfo": {"name": "Society-hooks", "version": "1"}}}
            status, _, identity, _ = post(url.port, "/mcp", initialize)
            assert status == 200
            assert post(url.port, "/mcp", {"jsonrpc": "2.0", "method": "notifications/initialized"}, session=identity)[0] == 202
            listed = post(url.port, "/mcp", {"jsonrpc": "2.0", "id": 2, "method": "tools/list"}, session=identity)[1]
            assert all(t["_meta"]["iisacc/hooksEnabled"] for t in listed["result"]["tools"])

            def call(path, content="VALUE"):
                status, data, _, frames = post(url.port, "/mcp", {"jsonrpc": "2.0", "id": secrets.token_hex(8), "method": "tools/call",
                    "params": {"name": "Write", "arguments": {"path": path, "content": content}, "_meta": {"progressToken": "hooks-test"}}}, session=identity)
                assert status == 200, data
                progress = [f for f in frames if f.get("method") == "notifications/progress"]
                assert any(f["params"].get("_meta", {}).get("iisacc/agentEvent", {}).get("event") == "hook" for f in progress), frames
                return data["result"]

            assert not call("rewrite.txt").get("isError")
            assert not (workspace / "rewrite.txt").exists() and (workspace / "rewritten.txt").read_text() == "REWRITTEN"
            for path in ("denied.txt", "ask.txt"):
                assert call(path)["isError"] and not (workspace / path).exists()
            (root / "block").touch()
            assert call("mcp-blocked.txt")["isError"] and not (workspace / "mcp-blocked.txt").exists()
            (root / "block").unlink()
            Path(hooks).write_text("INVALID_AFTER_LOAD")
            assert not call("frozen.txt").get("isError") and (workspace / "frozen.txt").read_text() == "VALUE"
            Path(hooks).write_text(json.dumps(settings))
            assert post(url.port, "/mcp", initialize, other, identity)[0] == 404
            report["mcp_http"] = {"rewrite": True, "deny_ask_precedence": True, "block": True, "hook_progress": True, "frozen_config": True}

        if args.official_stdio:
            from mcp import ClientSession, StdioServerParameters
            from mcp.client.stdio import stdio_client

            async def official():
                parameters = StdioServerParameters(command=mcp, args=common_mcp[1:] + mcp_flags, env=env)
                async with stdio_client(parameters) as (reader, writer):
                    async with ClientSession(reader, writer) as session:
                        await session.initialize()
                        result = await session.call_tool("Write", {"path": "stdio.txt", "content": "OFFICIAL"})
                        assert not result.isError and (workspace / "stdio.txt").read_text() == "OFFICIAL"
                        (root / "block").touch()
                        result = await session.call_tool("Write", {"path": "stdio-blocked.txt", "content": "NO"})
                        assert result.isError and not (workspace / "stdio-blocked.txt").exists()
                        (root / "block").unlink()
                report["official_mcp_stdio"] = {"write": True, "block": True}

            asyncio.run(official())
        events = [json.loads(line) for line in (root / "events.jsonl").read_text().splitlines()]
        assert {"PreToolUse", "PostToolUse", "PostToolUseFailure", "TaskCreated", "TaskCompleted"} <= {e["hook_event_name"] for e in events}
        created = next(e for e in events if e["hook_event_name"] == "TaskCreated")
        assert created["task_id"] and created["task_subject"] == "HOOK_TASK" and created["task_description"] == "COMMAND_VERIFIED", created
        assert created["transcript_path"].endswith("/transcript.jsonl")
        assert all(e["cwd"] == str(workspace) and e["session_id"] for e in events)
        if args.catalog:
            assert all(e["transcript_path"].endswith("/transcript.jsonl") for e in events if e["hook_event_name"] == "Stop")
        report["events"] = events; report["passed"] = True
    if args.report:
        args.report.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({"passed": True, "invalid_host_cases": invalid, "inference": bool(args.catalog), "official_stdio": args.official_stdio}))


if __name__ == "__main__":
    main()
