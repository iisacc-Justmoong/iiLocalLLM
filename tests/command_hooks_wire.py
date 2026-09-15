"""Configured command/HTTP hooks through API, CLI, MCP and optional native inference."""
import argparse
import asyncio
from contextlib import contextmanager, ExitStack
from http.client import HTTPConnection
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import re
import secrets
import selectors
import signal
import shlex
import shutil
import subprocess
import sys
import tempfile
import threading
import time
from urllib.parse import urlsplit


@contextmanager
def http_endpoint(script, token, report):
    """Test-only external peer; the production hook executor stays in C++."""
    requests, errors = [], []

    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, *_):
            pass

        def do_POST(self):
            try:
                assert self.headers.get("Authorization") == "Bearer " + token
                assert not self.headers.get("X-Forbidden")
                assert self.headers.get("Content-Type") == "application/json"
                body = self.rfile.read(int(self.headers["Content-Length"]))
                value = json.loads(body)
                requests.append(value["hook_event_name"])
                executed = subprocess.run([sys.executable, "-B", script], input=body, capture_output=True, timeout=10)
                if executed.returncode == 2:
                    response = {"decision": "block", "reason": executed.stderr.decode().strip()}
                else:
                    assert executed.returncode == 0, executed.stderr.decode(errors="replace")
                    response = json.loads(executed.stdout) if executed.stdout.strip() else {}
                status = 200
            except Exception as error:
                errors.append(str(error))
                response, status = {}, 500
            encoded = json.dumps(response).encode()
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(encoded)))
            self.end_headers()
            self.wfile.write(encoded)

    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    worker = threading.Thread(target=server.serve_forever, daemon=True)
    worker.start()
    try:
        yield f"http://127.0.0.1:{server.server_port}/hook"
        assert requests and not errors, errors
        report["http_endpoint"] = {"requests": len(requests), "events": sorted(set(requests)),
                                   "authenticated": True, "forbidden_environment_omitted": True}
    finally:
        server.shutdown()
        server.server_close()
        worker.join(timeout=5)


def main():
    parser = argparse.ArgumentParser()
    for name in ("daemon", "cli", "mcp"):
        parser.add_argument(name, type=Path)
    parser.add_argument("--catalog", type=Path)
    parser.add_argument("--model", default="model://qwen3-8b-q4")
    parser.add_argument("--official-stdio", action="store_true")
    parser.add_argument("--http-hooks", action="store_true")
    parser.add_argument("--report", type=Path)
    args = parser.parse_args()
    daemon, cli, mcp = (str(getattr(args, key).resolve()) for key in ("daemon", "cli", "mcp"))
    report = {"passed": False, "binaries": [daemon, cli, mcp], "inference": bool(args.catalog), "hook_type": "http" if args.http_hooks else "command"}
    env = dict(os.environ)
    for key in ("DYLD_LIBRARY_PATH", "DYLD_FRAMEWORK_PATH", "DYLD_FALLBACK_LIBRARY_PATH", "LIBRARY_PATH"):
        env.pop(key, None)
    with tempfile.TemporaryDirectory(prefix="hw-") as temporary, ExitStack() as resources:
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
        policy = private("permissions", {"enabled_sources": ["local"], "settings": {"permissions": {
            "defaultMode": "default", "deny": ["Edit(/denied.txt)"], "ask": ["Edit(/ask.txt)", "Write(/request-*)", "TaskCreate"]}}})
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
    result = {"hookEventName": event, "permissionDecision": "passthrough" if value["tool_input"]["path"].startswith("persist-") else "allow"}
    if value["tool_input"]["path"] == "rewrite.txt":
        result["updatedInput"] = {"path": "rewritten.txt", "content": "REWRITTEN"}
    print(json.dumps({"hookSpecificOutput": result}))
elif event == "PermissionRequest":
    decision = {"behavior": "allow"}
    args = value["tool_input"]
    path = args.get("path", "")
    if value["tool_name"] == "TaskCreate":
        if args["subject"] == "REQUEST_TASK":
            decision["updatedInput"] = dict(args, subject="APPROVED_TASK")
        elif args["subject"] == "REQUEST_DENIED_TASK":
            decision = {"behavior": "deny", "message": "REQUEST_DENIED"}
    elif path == "request-rewrite.txt":
        decision["updatedInput"] = {"path": "request-rewritten.txt", "content": "REQUEST_REWRITTEN"}
    elif path == "request-native.txt":
        decision["updatedInput"] = {"path": "permission-native-applied.txt", "content": (root / "permission-content.txt").read_text()}
    elif path == "request-forbidden.txt":
        decision["updatedInput"] = {"path": "denied.txt", "content": "FORBIDDEN"}
    elif path == "request-updates.txt":
        decision["updatedPermissions"] = [{"type": "addRules", "destination": "userSettings", "behavior": "allow", "rules": [{"toolName": "Write"}]}]
    elif path == "persist-session-other.txt":
        decision = {"behavior": "deny", "message": "NO_SESSION_GRANT"}
    elif path.startswith("persist-session-"):
        decision["updatedPermissions"] = [{"type": "addRules", "destination": "session", "behavior": "allow", "rules": [{"toolName": "Write", "ruleContent": "/persist-session-*.txt"}]}]
    elif path.startswith("persist-local-") or path.startswith("persist-native-"):
        prefix = "persist-local" if path.startswith("persist-local-") else "persist-native"
        decision["updatedPermissions"] = [{"type": "addRules", "destination": "localSettings", "behavior": "allow", "rules": [{"toolName": "Write", "ruleContent": "/" + prefix + "-*.txt"}]}]
    elif path == "request-invalid.txt":
        decision["updatedInput"] = {"path": "invalid-target.txt", "content": 42}
    elif path == "request-interrupt.txt":
        decision = {"behavior": "deny", "message": "REQUEST_INTERRUPT", "interrupt": True}
    elif path == "request-denied.txt":
        decision = {"behavior": "deny", "message": "REQUEST_DENIED"}
    else:
        sys.exit(0)
    print(json.dumps({"hookSpecificOutput": {"hookEventName": event, "decision": decision}}))
elif event == "SessionStart":
    initial = root / "initial.txt"
    fields = {"hookEventName": event, "additionalContext": "SESSION_START_CONTEXT"}
    if value["source"] == "clear" and (root / "clear-context.txt").exists():
        fields["additionalContext"] = (root / "clear-context.txt").read_text()
    if initial.exists():
        fields["initialUserMessage"] = initial.read_text()
    print(json.dumps({"continue": False, "decision": "block", "reason": "IGNORED_START_ERROR",
        "hookSpecificOutput": fields}))
elif event == "SessionEnd":
    assert pathlib.Path(value["transcript_path"]).is_file()
    print(json.dumps({"continue": False, "decision": "block", "reason": "IGNORED_END_ERROR"}))
elif event == "UserPromptSubmit":
    if "BLOCK_USER_PROMPT" in value["prompt"]:
        print("USER_PROMPT_DENIED", file=sys.stderr)
        sys.exit(2)
    if "STOP_USER_PROMPT" in value["prompt"]:
        print(json.dumps({"continue": False, "stopReason": "USER_PROMPT_STOPPED"}))
    else:
        context = "USER_PROMPT_CONTEXT"
        if "WRITE_FROM_HOOK_CONTEXT" in value["prompt"]:
            context = (root / "dynamic-context.txt").read_text()
        print(json.dumps({"hookSpecificOutput": {"hookEventName": event, "additionalContext": context}}))
elif event == "TaskCreated" and (root / "task-block").exists():
    print("TASK_BLOCK", file=sys.stderr)
    sys.exit(2)
elif event == "Stop" and (root / "stop").exists():
    print(json.dumps({"continue": False, "stopReason": "HOST_STOP"}))
''')
        command = shlex.join([sys.executable, "-B", script])
        settings = {"hooks": {event: [{"matcher": "Write" if "ToolUse" in event else "*",
            "hooks": [{"type": "command", "command": command, "timeout": 10}]}]
            for event in ("PreToolUse", "PostToolUse", "PostToolUseFailure", "PermissionRequest", "TaskCreated", "TaskCompleted", "Stop", "SessionStart", "UserPromptSubmit", "SessionEnd")}}
        if args.http_hooks:
            hook_token = secrets.token_urlsafe(36)
            url = resources.enter_context(http_endpoint(script, hook_token, report))
            env.update(IILOCALLLM_TEST_HOOK_TOKEN=hook_token, IILOCALLLM_TEST_FORBIDDEN="MUST_NOT_BE_SENT", no_proxy="127.0.0.1")
            for groups in settings["hooks"].values():
                groups[0]["hooks"] = [{"type": "http", "url": url, "timeout": 10,
                    "headers": {"Authorization": "Bearer ${IILOCALLLM_TEST_HOOK_TOKEN}", "X-Forbidden": "$IILOCALLLM_TEST_FORBIDDEN"},
                    "allowedEnvVars": ["IILOCALLLM_TEST_HOOK_TOKEN"]}]
            settings.update(allowedHttpHookUrls=[url], httpHookAllowedEnvVars=["IILOCALLLM_TEST_HOOK_TOKEN"])
        hooks = private("hooks", settings)
        common_daemon = [daemon, "--socket", str(root / "s"), "--http-port", "0", "--models-root", str(root / "models"),
            "--agent-workspace", str(workspace), "--agent-state", str(root / "api-state"), "--agent-credentials", credentials,
            "--agent-no-apps", "--agent-no-background", "--agent-no-skills", "--agent-no-subagents", "--no-agent-profiles"]
        common_mcp = [mcp, "--workspace", str(workspace), "--no-apps", "--no-background", "--no-agent-profiles"]
        invalid = 0
        for value in ([], {}, {"hooks": {"Notification": []}}, {"hooks": {"PreToolUse": [{"hooks": [{"type": "http"}]}]}},
                      {"hooks": {"Stop": [{"hooks": [{"type": "command", "command": "true", "async": True}]}]}},
                      {"hooks": {"Stop": [{"hooks": [{"type": "command", "command": 1}]}]}},
                      {"hooks": {"Stop": [{"hooks": [{"type": "http", "url": "file:///etc/hosts"}]}]}},
                      {"hooks": {}, "allowedHttpHookUrls": False}):
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
            approved = rpc("agent.tasks.create", {"session_id": owner, "subject": "REQUEST_TASK", "description": "REQUEST_REWRITE"})
            assert not approved["is_error"] and approved["result"]["task"]["subject"] == "APPROVED_TASK", approved
            rejected = rpc("agent.tasks.create", {"session_id": owner, "subject": "REQUEST_DENIED_TASK", "description": "NO_COMMIT"})
            assert rejected["is_error"] and "REQUEST_DENIED" in rejected["text"], rejected
            assert "REQUEST_DENIED_TASK" not in json.dumps(rpc("agent.tasks.list", {"session_id": owner}))
            parameters = private("permission-task", {"session_id": owner, "subject": "REQUEST_TASK", "description": "IPC_REWRITE"})
            command_result = subprocess.run([cli, "--socket", str(root / "s"), "--auth-file", auth, "rpc", "agent.tasks.create", parameters],
                env=env, text=True, capture_output=True, timeout=15)
            assert command_result.returncode == 0 and json.loads(command_result.stdout)["result"]["task"]["subject"] == "APPROVED_TASK", command_result
            report["permission_request_api"] = {"rewrite": True, "deny_no_commit": True, "native_ipc_cli": True}
            for prompt, expected in (("BLOCK_USER_PROMPT", "failed"), ("STOP_USER_PROMPT", "cancelled")):
                outcome = rpc("agent.run", {"session_id": owner, "prompt": prompt})
                assert outcome["status"] == expected and outcome["usage"]["generated_tokens"] == 0, outcome
                assert "USER_PROMPT_" in outcome["error_message"], outcome
            queued = rpc("agent.inputs.enqueue", {"session_id": owner, "text": "BLOCK_USER_PROMPT queued"})["input"]
            outcome = rpc("agent.inputs.run", {"session_id": owner})
            assert outcome["status"] == "failed" and "USER_PROMPT_DENIED" in outcome["error_message"], outcome
            assert rpc("agent.inputs.list", {"session_id": owner})["count"] == 0
            history = rpc("agent.sessions.get", {"session_id": owner})["messages"]
            records = [m for m in history if m.get("metadata", {}).get("iilocal.user_prompt_hook")]
            assert [m["metadata"]["iilocal.user_prompt_hook"]["disposition"] for m in records] == ["blocked", "stopped", "blocked"], records
            assert records[-1]["id"] == queued["id"] and not any(m["tool_calls"] for m in history)
            rpc("agent.run", {"session_id": owner, "prompt": "BLOCK_USER_PROMPT", "userPrompt": False}, expected=400)
            parameters = private("cli-run", {"session_id": owner, "prompt": "BLOCK_USER_PROMPT from CLI"})
            cli_run = subprocess.run([cli, "--socket", str(root / "s"), "--auth-file", auth, "rpc", "agent.run", parameters],
                env=env, text=True, capture_output=True, timeout=15)
            cli_outcome = json.loads(cli_run.stdout)
            assert cli_outcome["status"] == "failed" and "USER_PROMPT_DENIED" in cli_outcome["error_message"], cli_run
            report["api_input_lifecycle"] = {"direct_block": True, "direct_stop": True, "queued_acknowledgement": True,
                "saved_dispositions": True, "remote_bypass_rejected": True, "cli_block": True}
            (root / "initial.txt").write_text("BLOCK_USER_PROMPT initial message")
            initial_owner = rpc("agent.sessions.create", {"model": args.model})["session_id"]
            outcome = rpc("agent.run", {"session_id": initial_owner, "prompt": "Wait for initial host context"})
            assert outcome["status"] == "failed" and "USER_PROMPT_DENIED" in outcome["error_message"], outcome
            assert outcome["usage"]["generated_tokens"] == 0
            initial_history = rpc("agent.sessions.get", {"session_id": initial_owner})["messages"]
            initial_inputs = [m for m in initial_history if m.get("metadata", {}).get("iilocal.input")]
            assert len(initial_inputs) == 1 and initial_inputs[0]["text"] == "BLOCK_USER_PROMPT initial message", initial_history
            assert rpc("agent.inputs.list", {"session_id": initial_owner})["count"] == 0
            (root / "initial.txt").unlink()
            report["api_input_lifecycle"]["initial_user_message_uses_queue_and_prompt_hook"] = True
            rpc("agent.sessions.end", {"session_id": initial_owner}, other, 404)
            rpc("agent.sessions.end", {"session_id": initial_owner, "reason": "invented"}, expected=400)
            parameters = private("end-parameters", {"session_id": initial_owner, "reason": "logout"})
            cli_end = subprocess.run([cli, "--socket", str(root / "s"), "--auth-file", auth, "rpc", "agent.sessions.end", parameters],
                env=env, text=True, capture_output=True, timeout=15)
            ended = json.loads(cli_end.stdout)
            assert cli_end.returncode == 0 and ended["ended"] and ended["reason"] == "logout", cli_end
            assert not rpc("agent.sessions.end", {"session_id": initial_owner})["ended"]
            assert rpc("agent.sessions.get", {"session_id": initial_owner})["messages"] == initial_history
            report["session_end"] = {"api_authentication": True, "invalid_reason": True, "cli": ended, "history_preserved": True, "repeat_idempotent": True}
            clear_old = rpc("agent.sessions.create", {"model": args.model})["session_id"]
            rpc("agent.run", {"session_id": clear_old, "prompt": "BLOCK_USER_PROMPT BEFORE_CLEAR"})
            old_history = rpc("agent.sessions.get", {"session_id": clear_old})["messages"]
            rpc("agent.sessions.clear", {"session_id": clear_old}, other, 404)
            cleared = rpc("agent.sessions.clear", {"session_id": clear_old})
            assert cleared["complete"] and cleared["session_id"] != clear_old, cleared
            fresh = rpc("agent.sessions.get", {"session_id": cleared["session_id"]})
            assert fresh["parent_session_id"] == clear_old and len(fresh["messages"]) == 1, fresh
            assert fresh["messages"][0]["metadata"]["iilocal.session_start"] == "clear", fresh
            assert rpc("agent.sessions.get", {"session_id": clear_old})["messages"] == old_history
            report["session_clear"] = {"api": cleared, "new_history": fresh, "old_history_preserved": True}
            parameters = private("clear-parameters", {"session_id": cleared["session_id"]})
            cli_clear = subprocess.run([cli, "--socket", str(root / "s"), "--auth-file", auth, "rpc", "agent.sessions.clear", parameters],
                env=env, text=True, capture_output=True, timeout=15)
            cli_result = json.loads(cli_clear.stdout)
            assert cli_clear.returncode == 0 and cli_result["complete"] and cli_result["previous_session_id"] == cleared["session_id"], cli_clear
            report["session_clear"]["cli"] = cli_result
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
                    prompt_records = [m for m in history if m.get("metadata", {}).get("iilocal.user_prompt_hook")]
                    assert len(prompt_records) == 1 and prompt_records[0]["metadata"]["iilocal.user_prompt_hook"]["context"] == "USER_PROMPT_CONTEXT", history
                    starts = [m for m in history if m.get("metadata", {}).get("iilocal.session_start")]
                    assert len(starts) == 1 and starts[0]["text"] == "SESSION_START_CONTEXT", starts
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
                session = rpc("agent.sessions.create", {"model": args.model})["session_id"]
                content = "CONTEXT_" + secrets.token_hex(16)
                (root / "dynamic-context.txt").write_text('Write path="hook-context.txt", content="' + content + '". Do not add a newline.')
                outcome = rpc("agent.run", {"session_id": session,
                    "prompt": "WRITE_FROM_HOOK_CONTEXT: Use the host-provided UserPromptSubmit context to call Write once with its exact path and content, then return DONE.",
                    "max_turns": 4, "options": {"temperature": 0, "max_tokens": 1024}})
                assert outcome["status"] == "completed" and (workspace / "hook-context.txt").read_text() == content, outcome
                report["model_input_context"] = {"passed": True, "outcome": outcome, "content": content}
                content = "CLEAR_" + secrets.token_hex(16)
                (root / "clear-context.txt").write_text('Write path="clear-context.txt", content="' + content + '". Do not add a newline.')
                cleared = rpc("agent.sessions.clear", {"session_id": session})
                assert cleared["complete"], cleared
                fresh = rpc("agent.sessions.get", {"session_id": cleared["session_id"]})
                assert len(fresh["messages"]) == 1 and content in fresh["messages"][0]["text"], fresh
                outcome = rpc("agent.run", {"session_id": cleared["session_id"],
                    "prompt": "Follow the new SessionStart instruction. Call Write once with its exact path and content, then return DONE.",
                    "max_turns": 4, "options": {"temperature": 0, "max_tokens": 1024}})
                assert outcome["status"] == "completed" and (workspace / "clear-context.txt").read_text() == content, outcome
                report["model_clear_context"] = {"passed": True, "clear": cleared, "outcome": outcome, "content": content}
                (root / "clear-context.txt").unlink()
                content = "PERMISSION_" + secrets.token_hex(16)
                (root / "permission-content.txt").write_text(content)
                session = rpc("agent.sessions.create", {"model": args.model})["session_id"]
                outcome = rpc("agent.run", {"session_id": session, "prompt": 'Call Write exactly once with path="request-native.txt" and content="MODEL_CONTENT". Return DONE on success. Do not use another tool.',
                    "max_turns": 4, "options": {"temperature": 0, "max_tokens": 1024}})
                assert outcome["status"] == "completed" and not (workspace / "request-native.txt").exists(), outcome
                assert (workspace / "permission-native-applied.txt").read_text() == content, outcome
                history = rpc("agent.sessions.get", {"session_id": session})["messages"]
                calls = [c for m in history for c in m["tool_calls"] if c["name"] == "Write"]
                assert len(calls) == 1 and calls[0]["arguments"]["path"] == "request-native.txt", history
                report["model_permission_request"] = {"rewritten": True, "outcome": outcome, "content": content, "model_calls": calls}
                session = rpc("agent.sessions.create", {"model": args.model})["session_id"]
                interrupted = rpc("agent.run", {"session_id": session, "prompt": 'Call Write once with path="request-interrupt.txt" and content="NO_WRITE".',
                    "max_turns": 3, "options": {"temperature": 0, "max_tokens": 1024}})
                assert interrupted["status"] == "cancelled" and "REQUEST_INTERRUPT" in json.dumps(interrupted), interrupted
                assert not (workspace / "request-interrupt.txt").exists()
                report["model_permission_request"]["interrupt"] = interrupted
                session = rpc("agent.sessions.create", {"model": args.model})["session_id"]
                content = "PERSIST_" + secrets.token_hex(12)
                outcome = rpc("agent.run", {"session_id": session, "prompt": f'Call Write exactly once with path="persist-native-first.txt" and content="{content}". Do not add a newline. Return DONE.',
                    "max_turns": 4, "options": {"temperature": 0, "max_tokens": 1024}})
                assert outcome["status"] == "completed" and (workspace / "persist-native-first.txt").read_text() == content, outcome
                rules = rpc("agent.permissions.get", {"session_id": session})["rules"]
                assert any(r["source"] == "localSettings" and r["rule"] == "Write(/persist-native-*.txt)" for r in rules), rules
                report["model_permission_updates"] = {"first": outcome, "persisted_rule": True}

        events = [json.loads(line) for line in (root / "events.jsonl").read_text().splitlines()]
        assert [e["reason"] for e in events if e["hook_event_name"] == "SessionEnd" and e["session_id"] == owner] == ["other"]
        assert [e["reason"] for e in events if e["hook_event_name"] == "SessionEnd" and e["session_id"] == initial_owner] == ["logout"]
        report["session_end"]["daemon_shutdown_once"] = True
        with server("api-resume", common_daemon + ["--agent-hooks", hooks, "--agent-permission-settings", policy] + extra,
                    r"iiLocalLLM HTTP: http://127\.0\.0\.1:(\d+)") as match:
            port = int(match[1])
            outcome = rpc("agent.run", {"session_id": owner, "prompt": "BLOCK_USER_PROMPT after resume"})
            assert outcome["status"] == "failed" and "USER_PROMPT_DENIED" in outcome["error_message"], outcome
            events = [json.loads(line) for line in (root / "events.jsonl").read_text().splitlines()]
            sources = [e["source"] for e in events if e["hook_event_name"] == "SessionStart" and e["session_id"] == owner]
            assert sources == ["startup", "resume"], sources
            report["api_input_lifecycle"]["activation_resume_once"] = True
            if args.catalog:
                session = rpc("agent.sessions.create", {"model": args.model}, other)["session_id"]
                content = "RESTART_" + secrets.token_hex(12)
                outcome = rpc("agent.run", {"session_id": session, "prompt": f'Call Write exactly once with path="persist-native-restart.txt" and content="{content}". Do not add a newline. Return DONE.',
                    "max_turns": 4, "options": {"temperature": 0, "max_tokens": 1024}}, other)
                assert outcome["status"] == "completed" and (workspace / "persist-native-restart.txt").read_text() == content, outcome
                events = [json.loads(line) for line in (root / "events.jsonl").read_text().splitlines()]
                assert not any(e["hook_event_name"] == "PermissionRequest" and e["session_id"] == session for e in events), events
                report["model_permission_updates"]["restart_new_client_without_request"] = outcome

        mcp_flags = ["--hooks", hooks, "--permission-settings", policy, "--model", args.model, "--models", str(root / "models"),
                     "--no-subagents", "--no-skills", "--allow", "iiLocalLLM.agent.inputs.*"]
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

            def call_tool(name, arguments, hook_progress=True):
                status, data, _, frames = post(url.port, "/mcp", {"jsonrpc": "2.0", "id": secrets.token_hex(8), "method": "tools/call",
                    "params": {"name": name, "arguments": arguments, "_meta": {"progressToken": "hooks-test"}}}, session=identity)
                assert status == 200, data
                progress = [f for f in frames if f.get("method") == "notifications/progress"]
                if hook_progress:
                    assert any(f["params"].get("_meta", {}).get("iisacc/agentEvent", {}).get("event") == "hook" for f in progress), frames
                return data["result"]

            def call(path, content="VALUE"):
                return call_tool("Write", {"path": path, "content": content})

            for prompt, expected in (("BLOCK_USER_PROMPT MCP", "failed"), ("STOP_USER_PROMPT MCP", "cancelled")):
                outcome = call_tool("iiLocalLLM.agent.run", {"prompt": prompt})
                assert outcome["isError"] and outcome["structuredContent"]["status"] == expected, outcome
                assert "USER_PROMPT_" in outcome["structuredContent"]["error_message"], outcome
            previous_id = outcome["structuredContent"]["session_id"]
            outcome = call_tool("iiLocalLLM.agent.run", {"prompt": "BLOCK_USER_PROMPT fresh MCP", "new_session": True})
            mcp_id = outcome["structuredContent"]["session_id"]
            assert previous_id != mcp_id
            events = [json.loads(line) for line in (root / "events.jsonl").read_text().splitlines()]
            assert [e["reason"] for e in events if e["hook_event_name"] == "SessionEnd" and e["session_id"] == previous_id] == ["clear"]
            report["session_end"]["mcp_replace_clear"] = True
            assert outcome["structuredContent"]["clear"]["complete"], outcome
            assert [e["source"] for e in events if e["hook_event_name"] == "SessionStart" and e["session_id"] == mcp_id] == ["clear"]
            # The explicit control runs without invoking the model or borrowing
            # the foreground tool lock. Hook diagnostics are part of its report.
            status, data, _, _ = post(url.port, "/mcp", {"jsonrpc": "2.0", "id": "clear-control", "method": "tools/call",
                "params": {"name": "iiLocalLLM.agent.clear", "arguments": {}}}, session=identity)
            assert status == 200 and data["result"]["structuredContent"]["complete"], data
            report["session_clear"]["mcp_http"] = data["result"]["structuredContent"]
            mcp_id = data["result"]["structuredContent"]["session_id"]

            assert not call("rewrite.txt").get("isError")
            assert not (workspace / "rewrite.txt").exists() and (workspace / "rewritten.txt").read_text() == "REWRITTEN"
            assert not call("request-rewrite.txt").get("isError") and (workspace / "request-rewritten.txt").read_text() == "REQUEST_REWRITTEN"
            assert not (workspace / "request-rewrite.txt").exists()
            for path in ("request-denied.txt", "request-forbidden.txt", "request-updates.txt", "request-invalid.txt"):
                result = call(path)
                assert result["isError"] and not (workspace / path).exists(), result
                if path == "request-updates.txt":
                    assert "destination is disabled" in json.dumps(result), result
            assert not (workspace / "denied.txt").exists() and not (workspace / "invalid-target.txt").exists()
            report["permission_request_mcp"] = {"rewrite": True, "deny": True, "host_deny_rechecked": True, "schema_rechecked": True, "disabled_destination_fails": True}
            for path in ("persist-local-first.txt", "persist-local-second.txt", "persist-session-first.txt", "persist-session-second.txt"):
                assert not call(path).get("isError") and (workspace / path).read_text() == "VALUE", path
            cleared_permissions = call_tool("iiLocalLLM.agent.clear", {}, hook_progress=False)
            assert not cleared_permissions.get("isError"), cleared_permissions
            assert any(e["event"] == "hook" for e in cleared_permissions["structuredContent"]["start_diagnostics"]), cleared_permissions
            mcp_id = cleared_permissions["structuredContent"]["session_id"]
            assert not call("persist-session-after-clear.txt").get("isError")
            events = [json.loads(line) for line in (root / "events.jsonl").read_text().splitlines()]
            request_paths = [e["tool_input"].get("path") for e in events if e["hook_event_name"] == "PermissionRequest"]
            assert request_paths.count("persist-local-first.txt") == 1 and request_paths.count("persist-session-first.txt") == 1, request_paths
            assert not any(p in request_paths for p in ("persist-local-second.txt", "persist-session-second.txt", "persist-session-after-clear.txt")), request_paths
            status, _, other_identity, _ = post(url.port, "/mcp", initialize, other)
            assert status == 200
            assert post(url.port, "/mcp", {"jsonrpc": "2.0", "method": "notifications/initialized"}, other, other_identity)[0] == 202
            result = post(url.port, "/mcp", {"jsonrpc": "2.0", "id": 55, "method": "tools/call", "params": {
                "name": "Write", "arguments": {"path": "persist-session-other.txt", "content": "NO"}}}, other, other_identity)[1]["result"]
            assert result["isError"] and not (workspace / "persist-session-other.txt").exists(), result
            report["permission_updates_mcp"] = {"persisted_rule": True, "session_grant": True, "clear_inheritance": True, "other_client_isolated": True}
            for path in ("denied.txt", "ask.txt"):
                assert call(path)["isError"] and not (workspace / path).exists()
            (root / "block").touch()
            assert call("mcp-blocked.txt")["isError"] and not (workspace / "mcp-blocked.txt").exists()
            (root / "block").unlink()
            Path(hooks).write_text("INVALID_AFTER_LOAD")
            assert not call("frozen.txt").get("isError") and (workspace / "frozen.txt").read_text() == "VALUE"
            Path(hooks).write_text(json.dumps(settings))
            assert post(url.port, "/mcp", initialize, other, identity)[0] == 404
            report["mcp_http"] = {"rewrite": True, "deny_ask_precedence": True, "block": True, "hook_progress": True,
                "frozen_config": True, "user_prompt_block_stop": True}
            connection = HTTPConnection("127.0.0.1", url.port, timeout=10)
            try:
                connection.request("DELETE", "/mcp", headers={"Authorization": "Bearer " + token, "Mcp-Session-Id": identity})
                response = connection.getresponse(); response.read(); assert response.status == 200
            finally:
                connection.close()
            deadline = time.monotonic() + 5
            while True:
                events = [json.loads(line) for line in (root / "events.jsonl").read_text().splitlines()]
                reasons = [e["reason"] for e in events if e["hook_event_name"] == "SessionEnd" and e["session_id"] == mcp_id]
                if reasons: break
                assert time.monotonic() < deadline, "HTTP DELETE did not finish SessionEnd"
                time.sleep(.02)
            assert reasons == ["other"], reasons
            report["session_end"]["mcp_http_delete"] = True

        for sig in (signal.SIGTERM, signal.SIGINT):
            with (root / (sig.name + ".log")).open("w") as log:
                process = subprocess.Popen(common_mcp + mcp_flags, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=log, env=env)
                incoming = b""
                with selectors.DefaultSelector() as selector:
                    selector.register(process.stdout, selectors.EVENT_READ)
                    def stdio_rpc(message):
                        nonlocal incoming
                        process.stdin.write(json.dumps(message).encode() + b"\n"); process.stdin.flush()
                        if "id" not in message: return
                        deadline = time.monotonic() + 15
                        while True:
                            while b"\n" in incoming:
                                line, incoming = incoming.split(b"\n", 1)
                                value = json.loads(line)
                                if value.get("id") == message["id"]: return value
                            assert time.monotonic() < deadline, "stdio response timed out"
                            if selector.select(.1):
                                chunk = os.read(process.stdout.fileno(), 65536)
                                assert chunk, "stdio closed before its response"
                                incoming += chunk
                    try:
                        stdio_rpc(initialize)
                        stdio_rpc({"jsonrpc": "2.0", "method": "notifications/initialized"})
                        outcome = stdio_rpc({"jsonrpc": "2.0", "id": 3, "method": "tools/call", "params": {
                            "name": "iiLocalLLM.agent.run", "arguments": {"prompt": "BLOCK_USER_PROMPT signal cleanup"}}})
                        signal_id = outcome["result"]["structuredContent"]["session_id"]
                        process.send_signal(sig); assert process.wait(timeout=10) == 0
                    finally:
                        if process.poll() is None: process.kill(); process.wait()
                        process.stdin.close(); process.stdout.close()
            events = [json.loads(line) for line in (root / "events.jsonl").read_text().splitlines()]
            assert [e["reason"] for e in events if e["hook_event_name"] == "SessionEnd" and e["session_id"] == signal_id] == ["other"]
            report["session_end"]["stdio_" + sig.name.lower()] = True

        if args.official_stdio:
            from mcp import ClientSession, StdioServerParameters
            from mcp.client.stdio import stdio_client

            async def official():
                parameters = StdioServerParameters(command=mcp, args=common_mcp[1:] + mcp_flags, env=env)
                async with stdio_client(parameters) as (reader, writer):
                    async with ClientSession(reader, writer) as session:
                        await session.initialize()
                        for prompt in ("BLOCK_USER_PROMPT STDIO", "STOP_USER_PROMPT STDIO"):
                            denied = await session.call_tool("iiLocalLLM.agent.run", {"prompt": prompt})
                            assert denied.isError and "USER_PROMPT_" in denied.structuredContent["error_message"], denied
                        stdio_id = denied.structuredContent["session_id"]
                        cleared = await session.call_tool("iiLocalLLM.agent.clear", {})
                        assert not cleared.isError and cleared.structuredContent["complete"], cleared
                        assert cleared.structuredContent["previous_session_id"] == stdio_id
                        stdio_id = cleared.structuredContent["session_id"]
                        report["session_clear"]["official_stdio"] = cleared.structuredContent
                        result = await session.call_tool("Write", {"path": "stdio.txt", "content": "OFFICIAL"})
                        assert not result.isError and (workspace / "stdio.txt").read_text() == "OFFICIAL"
                        observed = await session.call_tool("Read", {"path": "request-rewritten.txt"})
                        assert not observed.isError, observed
                        result = await session.call_tool("Write", {"path": "request-rewrite.txt", "content": "OFFICIAL_ORIGINAL"})
                        assert not result.isError and (workspace / "request-rewritten.txt").read_text() == "REQUEST_REWRITTEN", result
                        result = await session.call_tool("Write", {"path": "request-denied.txt", "content": "NO"})
                        assert result.isError and not (workspace / "request-denied.txt").exists(), result
                        report["permission_request_mcp"]["official_stdio"] = True
                        result = await session.call_tool("Write", {"path": "persist-local-stdio.txt", "content": "PERSISTED"})
                        assert not result.isError and (workspace / "persist-local-stdio.txt").read_text() == "PERSISTED", result
                        result = await session.call_tool("Write", {"path": "persist-session-other.txt", "content": "NO"})
                        assert result.isError and not (workspace / "persist-session-other.txt").exists(), result
                        report["permission_updates_mcp"]["official_stdio_restart"] = True
                        (root / "block").touch()
                        result = await session.call_tool("Write", {"path": "stdio-blocked.txt", "content": "NO"})
                        assert result.isError and not (workspace / "stdio-blocked.txt").exists()
                        (root / "block").unlink()
                report["official_mcp_stdio"] = {"write": True, "block": True, "user_prompt_block_stop": True}
                events = [json.loads(line) for line in (root / "events.jsonl").read_text().splitlines()]
                assert [e["reason"] for e in events if e["hook_event_name"] == "SessionEnd" and e["session_id"] == stdio_id] == ["other"]
                report["session_end"]["official_stdio_close"] = True

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
    print(json.dumps({"passed": True, "invalid_host_cases": invalid, "inference": bool(args.catalog), "official_stdio": args.official_stdio, "hook_type": report["hook_type"]}))


if __name__ == "__main__":
    main()
