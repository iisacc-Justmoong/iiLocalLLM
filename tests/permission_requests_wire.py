"""App permission prompts across API, IPC, MCP and optional real local inference."""
import argparse
import asyncio
from concurrent.futures import ThreadPoolExecutor
from contextlib import contextmanager
from http.client import HTTPConnection
import json
import os
from pathlib import Path
import re
import secrets
import shutil
import subprocess
import tempfile
import time
from urllib.parse import urlsplit


def main():
    parser = argparse.ArgumentParser()
    for key in ("daemon", "cli", "mcp"):
        parser.add_argument(key, type=Path)
    parser.add_argument("--catalog", type=Path)
    parser.add_argument("--model", default="model://qwen3-8b-q4")
    parser.add_argument("--official-stdio", action="store_true")
    parser.add_argument("--report", type=Path)
    args = parser.parse_args()
    daemon, cli, mcp = [str(getattr(args, key).resolve()) for key in ("daemon", "cli", "mcp")]
    env = dict(os.environ)
    for key in ("DYLD_LIBRARY_PATH", "DYLD_FRAMEWORK_PATH", "DYLD_FALLBACK_LIBRARY_PATH", "LIBRARY_PATH"):
        env.pop(key, None)
    report = {"passed": False, "version": "0.27.0", "inference": bool(args.catalog), "binaries": [daemon, cli, mcp]}
    # The sanitizer build root is long; keep native AF_UNIX socket paths short.
    with tempfile.TemporaryDirectory(prefix="pr-") as temp:
        root = Path(temp)
        work = root / "work"
        work.mkdir()
        token, other = secrets.token_urlsafe(36), secrets.token_urlsafe(36)

        def private(name, value):
            path = root / name
            path.write_text(value if isinstance(value, str) else json.dumps(value))
            path.chmod(0o600)
            return str(path)

        credentials = private("credentials", {"society": token, "dreamscapes": other})
        auth = private("auth", token)
        requests = private("requests", {"timeout_ms": 30000})
        settings = private("settings", {"enabled_sources": [], "settings": {"permissions": {"ask": ["TaskCreate"], "deny": ["Write(/denied.txt)"]}}})
        common = [daemon, "--socket", str(root / "s"), "--http-port", "0", "--models-root", str(root / "models"),
            "--agent-workspace", str(work), "--agent-state", str(root / "api-state"), "--agent-credentials", credentials,
            "--agent-no-apps", "--agent-no-background", "--agent-no-skills", "--agent-no-subagents", "--no-agent-profiles"]
        mcp_common = [mcp, "--workspace", str(work), "--no-apps", "--no-background", "--no-agent-profiles"]
        invalid = 0
        for value in ([], {"timeout_ms": 0}, {"max_pending": 1025}, {"max_request_bytes": 1}, {"max_history": 1.5}, {"mode": "bypassPermissions"}):
            bad = private("invalid", value)
            for command in (common + ["--agent-permission-requests", bad], mcp_common + ["--permission-requests", bad, "--model", args.model, "--models", str(root / "models")]):
                result = subprocess.run(command, env=env, capture_output=True, timeout=15)
                assert result.returncode and not result.stdout, result
                assert not (root / "models").exists(), "Invalid permission configuration reached model initialization"
                invalid += 1
        for bad in (Path(private("public", {})), work / "requests.json"):
            bad.write_text("{}"); bad.chmod(0o644 if bad.parent == root else 0o600)
            for command in (common + ["--agent-permission-requests", str(bad)], mcp_common + ["--permission-requests", str(bad)]):
                assert subprocess.run(command, env=env, capture_output=True, timeout=15).returncode
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
            extra = ["--config", private("model-config", {"models": [{"model": args.model, "context_tokens": 8192,
                "options": {"enable_thinking": False, "tool_grammar": False}}]}), "--agent-no-auto-compact"]

        @contextmanager
        def server(name, command, pattern):
            log_path = root / (name + ".log")
            with log_path.open("w") as log:
                process = subprocess.Popen(command, env=env, stdout=log, stderr=log)
                if args.report:
                    args.report.with_suffix(".active.json").write_text(json.dumps({"pid": process.pid, "log": str(log_path), "phase": name}))
                try:
                    start = time.monotonic()
                    while True:
                        text = log_path.read_text(errors="replace")
                        match = re.search(pattern, text)
                        if match:
                            report[name + "_startup_seconds"] = round(time.monotonic() - start, 3)
                            yield match
                            break
                        assert process.poll() is None and time.monotonic() - start < (300 if args.catalog else 30), text[-2000:]
                        time.sleep(0.02)
                finally:
                    process.terminate()
                    try:
                        process.wait(timeout=15)
                    except subprocess.TimeoutExpired:
                        process.kill(); process.wait()
                    if args.report:
                        shutil.copyfile(log_path, args.report.with_suffix("." + name + ".log"))
                        args.report.with_suffix(".active.json").unlink(missing_ok=True)

        def post(port, path, body, bearer=token, identity=None):
            headers = {"Content-Type": "application/json", "Accept": "application/json, text/event-stream", "Authorization": "Bearer " + bearer}
            if identity:
                headers.update({"Mcp-Session-Id": identity, "Mcp-Protocol-Version": "2025-11-25"})
            connection = HTTPConnection("127.0.0.1", port, timeout=180)
            try:
                connection.request("POST", path, json.dumps(body), headers)
                response = connection.getresponse(); raw = response.read()
                if response.getheader("Content-Type", "").startswith("text/event-stream"):
                    messages = [json.loads(line[5:]) for line in raw.splitlines() if line.startswith(b"data:") and line[5:].strip()]
                    data = next(m for m in messages if m.get("id") == body.get("id") and ("result" in m or "error" in m))
                else:
                    data = json.loads(raw) if raw else {}
                return response.status, data, response.getheader("Mcp-Session-Id")
            finally:
                connection.close()

        def wait_prompt(pending, future, timeout=15):
            start = time.monotonic()
            while time.monotonic() - start < timeout:
                values = pending()["requests"]
                if values:
                    assert len(values) == 1, values
                    return values[0]
                if future.done():
                    raise AssertionError(("Operation completed without a prompt", future.result()))
                time.sleep(0.01)
            raise AssertionError("Permission prompt timeout")

        def command(method, params):
            file = private("cli-params", params)
            result = subprocess.run([cli, "--socket", str(root / "s"), "--auth-file", auth, "agent", "permissions", method, file],
                env=env, capture_output=True, text=True, timeout=15)
            assert result.returncode == 0, result.stderr
            return json.loads(result.stdout)

        with server("api", common + ["--agent-permission-requests", requests, "--agent-permission-settings", settings] + extra,
                    r"iiLocalLLM HTTP: http://127\.0\.0\.1:(\d+)") as match:
            port = int(match[1])

            def rpc(method, params=None, bearer=token, expected=200):
                status, data, _ = post(port, "/v1/rpc", {"id": secrets.token_hex(8), "method": method, "params": params or {}}, bearer)
                assert status == expected, (status, data)
                return data.get("result", data)

            assert rpc("agent.info")["permission_requests_enabled"]
            owner = rpc("agent.sessions.create", {"model": args.model})["session_id"]
            assert rpc("agent.permissions.get", {"session_id": owner})["mode"] == "default"
            with ThreadPoolExecutor(1) as pool:
                task = pool.submit(rpc, "agent.tasks.create", {"session_id": owner, "subject": "ORIGINAL", "description": "original"})
                prompt = wait_prompt(lambda: command("pending", {}), task)
                assert prompt["request"]["tool_name"] == "TaskCreate" and prompt["session_id"] == owner
                response = {"request_id": prompt["request_id"], "decision": {"behavior": "allow", "updatedInput": {"subject": "APPROVED", "description": "by app"}}}
                assert not rpc("agent.permissions.pending", bearer=other)["requests"]
                rpc("agent.permissions.respond", response, other, 404)
                rpc("agent.permissions.respond", response, "invalid", 401)
                rpc("agent.permissions.respond", {"request_id": prompt["request_id"], "decision": {"behavior": "ask"}}, expected=400)
                assert command("respond", response)["accepted"]
                result = task.result(timeout=15)
                assert not result["is_error"] and "APPROVED" in json.dumps(result), result
                assert rpc("agent.permissions.respond", response)["replayed"]
                task = pool.submit(rpc, "agent.tasks.create", {"session_id": owner, "subject": "DENIED", "description": "no commit"})
                prompt = wait_prompt(lambda: rpc("agent.permissions.pending"), task)
                assert rpc("agent.permissions.respond", {"request_id": prompt["request_id"], "decision": {"behavior": "deny", "message": "APP_DENIED"}})["accepted"]
                assert task.result(timeout=15)["is_error"]
                listed = rpc("agent.tasks.list", {"session_id": owner})
                assert "DENIED" not in json.dumps(listed) and "APPROVED" in json.dumps(listed), listed
                report["api_ipc"] = {"changed_input": True, "deny_no_commit": True, "foreign_client": True, "invalid_response": True, "idempotent_retry": True}
                if args.catalog:
                    outcomes = []
                    for interrupt in (False, True):
                        session = rpc("agent.sessions.create", {"model": args.model})["session_id"]
                        content = "REMOTE_" + secrets.token_hex(16)
                        task = pool.submit(rpc, "agent.run", {"session_id": session,
                            "prompt": f'Call Write exactly once with path="native-original.txt" and content="{content}". Return DONE on success. Do not retry a denied tool.',
                            "max_turns": 4, "options": {"temperature": 0, "max_tokens": 1024}})
                        prompt = wait_prompt(lambda: rpc("agent.permissions.pending"), task, 180)
                        assert prompt["request"]["tool_name"] == "Write", prompt
                        decision = {"behavior": "deny", "message": "APP_INTERRUPT", "interrupt": True} if interrupt else {
                            "behavior": "allow", "updatedInput": {"path": "native-approved.txt", "content": content}}
                        assert command("respond", {"request_id": prompt["request_id"], "decision": decision})["accepted"]
                        result = task.result(timeout=180)
                        assert result["status"] == ("cancelled" if interrupt else "completed"), result
                        assert not (work / "native-original.txt").exists()
                        if not interrupt:
                            assert (work / "native-approved.txt").read_text() == content
                        outcomes.append({"interrupt": interrupt, "request": prompt, "outcome": result})
                    report["native"] = outcomes

        with server("mcp", mcp_common + ["--permission-requests", requests, "--permission-settings", settings,
                    "--http-port", "0", "--credentials", credentials, "--state", str(root / "mcp-state")], r'\{"endpoint":"([^"\n]+)"\}') as match:
            port = urlsplit(match[1]).port
            init = {"jsonrpc": "2.0", "id": "init", "method": "initialize", "params": {"protocolVersion": "2025-11-25", "capabilities": {}, "clientInfo": {"name": "Society", "version": "1"}}}
            _, info, identity = post(port, "/mcp", init)
            assert info["result"]["capabilities"]["experimental"]["iisacc/permissionRequests"]["respondMethod"] == "iisacc/permissions/respond"
            assert post(port, "/mcp", {"jsonrpc": "2.0", "method": "notifications/initialized"}, identity=identity)[0] == 202
            _, _, other_identity = post(port, "/mcp", init, other)
            post(port, "/mcp", {"jsonrpc": "2.0", "method": "notifications/initialized"}, other, other_identity)

            def mcp_rpc(method, params=None, foreign=False):
                status, data, _ = post(port, "/mcp", {"jsonrpc": "2.0", "id": secrets.token_hex(8), "method": method, "params": params or {}}, other if foreign else token, other_identity if foreign else identity)
                assert status == 200, (status, data)
                return data

            tools = mcp_rpc("tools/list")["result"]["tools"]
            assert not any("permissions/respond" in t["name"] or "permissions.respond" in t["name"] for t in tools)
            with ThreadPoolExecutor(1) as pool:
                for denied_target in (False, True):
                    task = pool.submit(mcp_rpc, "tools/call", {"name": "Write", "arguments": {"path": "mcp-original.txt", "content": "ORIGINAL"}})
                    prompt = wait_prompt(lambda: mcp_rpc("iisacc/permissions/pending")["result"], task)
                    assert not mcp_rpc("iisacc/permissions/pending", foreign=True)["result"]["requests"]
                    response = {"request_id": prompt["request_id"], "decision": {"behavior": "allow", "updatedInput": {"path": "denied.txt" if denied_target else "mcp-approved.txt", "content": "MCP_APP"}}}
                    assert "error" in mcp_rpc("iisacc/permissions/respond", response, True)
                    assert mcp_rpc("iisacc/permissions/respond", response)["result"]["accepted"]
                    result = task.result(timeout=15)["result"]
                    assert bool(result.get("isError")) == denied_target, result
                assert (work / "mcp-approved.txt").read_text() == "MCP_APP" and not (work / "denied.txt").exists() and not (work / "mcp-original.txt").exists()
            report["mcp_http"] = {"changed_input": True, "host_deny_rechecked": True, "connection_isolation": True, "control_not_a_tool": True}

        if args.official_stdio:
            from mcp import ClientSession, StdioServerParameters, types
            from mcp.client.stdio import stdio_client

            class ControlParams(types.RequestParams):
                # The official base RequestParams ignores unknown extension fields.
                model_config = {"extra": "allow"}

            async def official():
                params = StdioServerParameters(command=mcp, args=mcp_common[1:] + ["--permission-requests", requests], env=env)
                async with stdio_client(params) as (reader, writer):
                    async with ClientSession(reader, writer) as session:
                        await session.initialize()

                        async def control(method, body):
                            result = await session.send_request(types.Request(method=method, params=ControlParams(**body)), types.Result)
                            return result.model_dump(by_alias=True, exclude_none=True)

                        writing = asyncio.create_task(session.call_tool("Write", {"path": "stdio-original.txt", "content": "ORIGINAL"}))
                        try:
                            async with asyncio.timeout(15):
                                while True:
                                    pending = await control("iisacc/permissions/pending", {})
                                    if pending["requests"]:
                                        break
                                    assert not writing.done(), writing.result()
                                    await asyncio.sleep(0.01)
                                response = {"request_id": pending["requests"][0]["request_id"], "decision": {"behavior": "allow", "updatedInput": {"path": "stdio-approved.txt", "content": "OFFICIAL_APP"}}}
                                assert (await control("iisacc/permissions/respond", response))["accepted"]
                                assert not (await writing).isError
                        finally:
                            if not writing.done():
                                writing.cancel()
                                await asyncio.gather(writing, return_exceptions=True)
                assert (work / "stdio-approved.txt").read_text() == "OFFICIAL_APP" and not (work / "stdio-original.txt").exists()
            asyncio.run(official())
            report["official_mcp_stdio"] = True
        report["passed"] = True
    if args.report:
        args.report.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({"passed": True, "inference": bool(args.catalog), "official_stdio": args.official_stdio}))


if __name__ == "__main__":
    main()
