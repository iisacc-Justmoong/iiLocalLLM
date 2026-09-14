"""Host settings, live file authorization, authenticated inspection and optional local inference."""
import argparse
import asyncio
from contextlib import contextmanager
import hashlib
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
    p = argparse.ArgumentParser()
    for name in ("daemon", "cli", "mcp"):
        p.add_argument(name, type=Path)
    p.add_argument("--catalog", type=Path)
    p.add_argument("--model", default="model://qwen3-8b-q4")
    p.add_argument("--official-stdio", action="store_true")
    p.add_argument("--additional-directories", action="store_true")
    p.add_argument("--report", type=Path)
    args = p.parse_args()
    daemon, cli, mcp = [str(getattr(args, key).resolve()) for key in ("daemon", "cli", "mcp")]
    report = {"passed": False, "binaries": [daemon, cli, mcp], "inference": bool(args.catalog), "additional_directories": args.additional_directories}
    env = dict(os.environ)
    for key in ("DYLD_LIBRARY_PATH", "DYLD_FRAMEWORK_PATH", "DYLD_FALLBACK_LIBRARY_PATH", "LIBRARY_PATH"):
        env.pop(key, None)
    with tempfile.TemporaryDirectory(prefix="settings-wire-") as temporary:
        root = Path(temporary)
        workspace = root / "work"
        workspace.mkdir()
        shared, cli_shared = root / "shared", root / "cli-shared"
        if args.additional_directories:
            shared.mkdir(); cli_shared.mkdir()
            (shared / "read.txt").write_text("SHARED_VALUE")
            (cli_shared / "read.txt").write_text("CLI_VALUE")
        policy_dir = root / "managed"
        policy_dir.mkdir()
        token, other = secrets.token_urlsafe(36), secrets.token_urlsafe(36)

        def private(name, value):
            path = root / name
            path.write_text(json.dumps(value) if not isinstance(value, str) else value)
            path.chmod(0o600)
            return str(path)

        credentials = private("credentials", {"society": token, "dreamscapes": other})
        auth = private("auth", token)
        host = private("host", {"managed_directory": "managed", "enabled_sources": ["project"],
            "settings": {"env": {"unrelated": "PRIVATE_VALUE_MUST_NOT_APPEAR"}}})
        policy_file = policy_dir / "managed-settings.json"

        def policy(allow=True):
            permissions = {
                "defaultMode": "dontAsk", "allow": ["Edit(/out/**)"] if allow else [],
                "deny": ["Read(.env)"] + ([] if allow else ["Edit(/out/**)"])}
            if args.additional_directories:
                permissions["additionalDirectories"] = ["../shared", ".."] if allow else []
                # Keep this rule after revoking the directory to test the path boundary alone.
                permissions["allow"] += [f"Edit(/{shared}/out/**)", f"Edit(/{cli_shared}/out/**)"]
            policy_file.write_text(json.dumps({"allowManagedPermissionRulesOnly": True, "permissions": permissions}))

        policy()
        project = workspace / ".claude/settings.json"
        project.parent.mkdir()
        project.write_text('{"permissions":{"allow":["Edit"]}}')
        (workspace / ".env").write_text("not disclosed")
        common_daemon = [daemon, "--socket", str(root / "s"), "--http-port", "0", "--models-root", str(root / "models"),
            "--agent-workspace", str(workspace), "--agent-state", str(root / "api-state"), "--agent-credentials", credentials,
            "--agent-no-apps", "--agent-no-tasks", "--agent-no-background", "--agent-no-skills", "--agent-no-subagents", "--no-agent-profiles"]
        common_mcp = [mcp, "--workspace", str(workspace), "--no-apps", "--no-tasks", "--no-background", "--no-agent-profiles"]
        if args.additional_directories:
            common_daemon += ["--agent-add-dir", str(cli_shared)]
            common_mcp += ["--add-dir", str(cli_shared)]
        bad = root / "invalid-host"
        invalid_count = 0
        for value in ([], {"enabled_sources": ["managed"]}, {"mode": "auto"}, {"settings": {"permissions": {"deny": [1]}}},
                      {"settings": {"permissions": {"unknownDirectories": ["outside"]}}}, {"unrecognized": True}):
            bad.write_text(json.dumps(value)); bad.chmod(0o600)
            for command in (common_daemon + ["--agent-permission-settings", str(bad)],
                            common_mcp + ["--permission-settings", str(bad), "--model", args.model, "--models", str(root / "models")]):
                result = subprocess.run(command, env=env, capture_output=True, timeout=15)
                assert result.returncode and not result.stdout, result
                assert not (root / "models").exists(), "Invalid settings reached Service initialization"
                invalid_count += 1
        for mode in (0o644,):
            bad.write_text("{}"); bad.chmod(mode)
            for command in (common_daemon + ["--agent-permission-settings", str(bad)], common_mcp + ["--permission-settings", str(bad)]):
                assert subprocess.run(command, env=env, capture_output=True, timeout=15).returncode
                invalid_count += 1
        # A root workspace contains every local host configuration path.
        bad.write_text("[]"); bad.chmod(0o600)
        root_command = list(common_mcp)
        root_command[root_command.index("--workspace") + 1] = "/"
        rejected = subprocess.run(root_command + ["--permission-settings", str(bad)], env=env, capture_output=True, timeout=15)
        assert rejected.returncode and b"outside the workspace" in rejected.stderr
        invalid_count += 1
        report["invalid_host_cases"] = invalid_count
        extra = []
        if args.catalog:
            source = args.catalog.resolve() / args.model.removeprefix("model://")
            manifest = json.loads((source / "manifest.json").read_text())
            assert manifest["id"] == args.model.removeprefix("model://")
            package = root / "models" / manifest["id"]
            for entry in manifest["files"]:
                target = package / entry["path"]
                target.parent.mkdir(parents=True, exist_ok=True)
                os.link(source / entry["path"], target)
            (package / "manifest.json").write_text(json.dumps(manifest))
            report["model_manifest"] = manifest
            config = private("models-config", {"models": [{"model": args.model, "context_tokens": 8192,
                "options": {"enable_thinking": False, "tool_grammar": False}}]})
            extra = ["--config", config, "--agent-no-auto-compact"]

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

        def post(port, path, body, bearer, session=None):
            headers = {"Content-Type": "application/json", "Accept": "application/json, text/event-stream", "Authorization": "Bearer " + bearer}
            if session:
                headers.update({"Mcp-Session-Id": session, "Mcp-Protocol-Version": "2025-11-25"})
            connection = HTTPConnection("127.0.0.1", port, timeout=180)
            try:
                connection.request("POST", path, json.dumps(body), headers)
                response = connection.getresponse()
                raw = response.read()
                if response.getheader("Content-Type", "").startswith("text/event-stream"):
                    messages = [json.loads(line[5:]) for line in raw.splitlines() if line.startswith(b"data:") and line[5:].strip()]
                    data = next(m for m in messages if m.get("id") == body.get("id") and ("result" in m or "error" in m))
                else:
                    data = json.loads(raw) if raw else {}
                return response.status, data, response.getheader("Mcp-Session-Id")
            finally:
                connection.close()

        with server("api", common_daemon + ["--agent-allow", "Write", "--agent-permission-settings", host] + extra,
                    r"iiLocalLLM HTTP: http://127\.0\.0\.1:(\d+)") as match:
            port = int(match[1])

            def rpc(method, params, bearer=token, expected=200):
                status, data, _ = post(port, "/v1/rpc", {"id": secrets.token_hex(8), "method": method, "params": params}, bearer)
                assert status == expected, data
                return data.get("result", data)

            session = rpc("agent.sessions.create", {"model": args.model})["session_id"]
            inspection = rpc("agent.permissions.get", {"session_id": session})
            assert inspection["managed_rules_only"] and inspection["mode"] == "dontAsk"
            assert not any(r["source"] in ("cliArg", "projectSettings") for r in inspection["rules"])
            assert "PRIVATE_VALUE_MUST_NOT_APPEAR" not in json.dumps(inspection)
            if args.additional_directories:
                assert str(shared) in inspection["working_directories"] and str(root) in inspection["working_directories"]
                assert any(x["source"] == "cliArg" and x["path"] == str(cli_shared) for x in inspection["additional_directories"])
            rpc("agent.permissions.get", {"session_id": session}, other, 404)
            rpc("agent.permissions.get", {"session_id": session}, "invalid", 401)
            rpc("agent.permissions.get", {"session_id": session, "mode": "bypassPermissions"}, expected=400)
            rpc("agent.permissions.get", {"session_id": session, "working_directories": [str(root)]}, expected=400)
            command = [cli, "--socket", str(root / "s"), "--auth-file", auth, "agent", "permissions", "get", session]
            process = subprocess.run(command, env=env, text=True, capture_output=True, timeout=15)
            assert process.returncode == 0, process.stderr
            assert json.loads(process.stdout) == inspection
            report["inspection"] = inspection
            if args.catalog:
                results = []
                for allowed in (True, False):
                    policy(allowed)
                    owner = rpc("agent.sessions.create", {"model": args.model})["session_id"]
                    path = "out/allowed.txt" if allowed else "out/blocked.txt"
                    if args.additional_directories:
                        path = "../shared/" + path
                    content = "SETTINGS_" + secrets.token_hex(8)
                    outcome = rpc("agent.run", {"session_id": owner, "prompt": f'Call Write once with path="{path}" and content="{content}". Do not add a newline. Return DONE on success; return DENIED if the tool is denied.',
                        "max_turns": 4, "options": {"temperature": 0, "max_tokens": 1024}})
                    assert outcome["status"] == "completed", outcome
                    history = rpc("agent.sessions.get", {"session_id": owner})["messages"]
                    calls = [c for m in history for c in m["tool_calls"] if c["name"] == "Write"]
                    assert calls, outcome
                    tool_results = [m for m in history if m["role"] == "tool" and m["tool_call_id"] in {c["id"] for c in calls}]
                    if allowed:
                        assert (workspace / path).read_text() == content, outcome
                    else:
                        assert not (workspace / path).exists() and any(m.get("is_error") and any(reason in m["text"].lower()
                            for reason in ("permission denied", "outside the configured working directories")) for m in tool_results), outcome
                    results.append({"allowed": allowed, "outcome": outcome, "write_calls": len(calls), "tool_results": tool_results})
                report["model_results"] = results
                policy()

        mcp_flags = ["--permission-settings", host, "--allow", "Write"]
        with server("mcp", common_mcp + mcp_flags + ["--http-port", "0", "--credentials", credentials, "--state", str(root / "mcp-state")],
                    r'\{"endpoint":"([^"\n]+)"\}') as match:
            url = urlsplit(match[1])
            initialize = {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {"protocolVersion": "2025-11-25", "capabilities": {},
                "clientInfo": {"name": "Society-settings", "version": "1"}}}
            assert post(url.port, "/mcp", initialize, "invalid")[0] == 401
            status, _, identity = post(url.port, "/mcp", initialize, token)
            assert status == 200
            assert post(url.port, "/mcp", {"jsonrpc": "2.0", "method": "notifications/initialized"}, token, identity)[0] == 202

            def call(name, arguments=None):
                status, data, _ = post(url.port, "/mcp", {"jsonrpc": "2.0", "id": secrets.token_hex(8), "method": "tools/call",
                    "params": {"name": name, "arguments": arguments or {}}}, token, identity)
                assert status == 200, data
                return data["result"]

            inspected = call("iiLocalLLM.agent.permissions.get")["structuredContent"]
            assert inspected["managed_rules_only"]
            assert not call("Write", {"path": "out/mcp.txt", "content": "MCP_SETTINGS"}).get("isError")
            assert (workspace / "out/mcp.txt").read_text() == "MCP_SETTINGS"
            assert call("Write", {"path": "outside.txt", "content": "blocked"})["isError"]
            assert call("Read", {"path": ".env"})["isError"]
            assert call("Write", {"path": ".claude/settings.json", "content": "{}"})["isError"]
            if args.additional_directories:
                assert not call("Write", {"path": "../shared/out/http.txt", "content": "ADDITIONAL_HTTP"}).get("isError")
                assert (shared / "out/http.txt").read_text() == "ADDITIONAL_HTTP"
                assert not call("Write", {"path": "../cli-shared/out/http.txt", "content": "CLI_HTTP"}).get("isError")
                assert (cli_shared / "out/http.txt").read_text() == "CLI_HTTP"
                assert call("Read", {"path": "../shared/read.txt"})["structuredContent"]["path"] == str(shared / "read.txt")
                assert call("Glob", {"path": "../shared", "pattern": "read.txt"})["structuredContent"]["paths"] == ["read.txt"]
                assert len(call("Grep", {"path": "../shared", "pattern": "SHARED_VALUE"})["structuredContent"]["matches"]) == 1
                for path in (credentials, host, str(root / "mcp-state" / "mcp.lock")):
                    rejected = call("Read", {"path": path})
                    assert rejected.get("isError") and token not in json.dumps(rejected), rejected
                case_path = Path(credentials).with_name(Path(credentials).name.upper())
                report["private_case_variant_checked"] = case_path.exists()
                if case_path.exists():
                    assert call("Read", {"path": str(case_path)}).get("isError"), "Private credential case variant was readable"
                assert call("Write", {"path": credentials, "content": "forbidden"})["isError"]
                assert call("Grep", {"path": str(root / "mcp-state"), "pattern": ".*"})["isError"]
                assert call("Write", {"path": "../shared/.claude/settings.json", "content": "{}"})["isError"]
            policy(False)
            changed = call("iiLocalLLM.agent.permissions.get")["structuredContent"]
            assert changed["sources"] != inspected["sources"]
            assert call("Write", {"path": "out/mcp-blocked.txt", "content": "blocked"})["isError"]
            assert not (workspace / "out/mcp-blocked.txt").exists()
            if args.additional_directories:
                assert str(shared) not in changed["working_directories"] and str(root) not in changed["working_directories"]
                assert str(cli_shared) in changed["working_directories"]
                assert not call("Read", {"path": "../cli-shared/read.txt"}).get("isError")
                assert call("Read", {"path": "../shared/read.txt"})["isError"]
                assert call("Write", {"path": "../shared/out/revoked.txt", "content": "forbidden"})["isError"]
                assert not (shared / "out/revoked.txt").exists()
                report["directory_mcp_http"] = {"settings_write": True, "cli_write": True, "read": True,
                    "glob": True, "grep": True, "private_paths": True, "settings_protected": True, "revocation": True}
            assert post(url.port, "/mcp", initialize, other, identity)[0] == 404
            report["mcp_http"] = {"write": True, "denials": 4, "live_reload": True, "foreign_session_rejected": True}

        if args.additional_directories:
            with server("cli_only", common_mcp + ["--http-port", "0", "--credentials", credentials, "--state", str(root / "cli-state")],
                        r'\{"endpoint":"([^"\n]+)"\}') as match:
                url = urlsplit(match[1])
                status, _, identity = post(url.port, "/mcp", initialize, token)
                assert status == 200
                assert post(url.port, "/mcp", {"jsonrpc": "2.0", "method": "notifications/initialized"}, token, identity)[0] == 202
                inspected = call("iiLocalLLM.agent.permissions.get")["structuredContent"]
                assert inspected["sources"] == [] and set(inspected["working_directories"]) == {str(workspace), str(cli_shared)}
                assert not call("Read", {"path": "../cli-shared/read.txt"}).get("isError")
                assert call("Read", {"path": "../shared/read.txt"})["isError"]
                assert call("Write", {"path": "../cli-shared/ambient.txt", "content": "forbidden"})["isError"]
                assert not (cli_shared / "ambient.txt").exists()
                report["cli_directory_without_disk_settings"] = True

        if args.official_stdio:
            from mcp import ClientSession, StdioServerParameters
            from mcp.client.stdio import stdio_client
            policy()

            async def official():
                params = StdioServerParameters(command=mcp, args=common_mcp[1:] + mcp_flags, env=env)
                async with stdio_client(params) as (reader, writer):
                    async with ClientSession(reader, writer) as session:
                        await session.initialize()
                        result = await session.call_tool("iiLocalLLM.agent.permissions.get", {})
                        assert not result.isError and result.structuredContent["managed_rules_only"]
                        result = await session.call_tool("Write", {"path": "out/stdio.txt", "content": "OFFICIAL_MCP"})
                        assert not result.isError and (workspace / "out/stdio.txt").read_text() == "OFFICIAL_MCP"
                        if args.additional_directories:
                            result = await session.call_tool("Write", {"path": "../shared/out/stdio.txt", "content": "ADDITIONAL_STDIO"})
                            assert not result.isError and (shared / "out/stdio.txt").read_text() == "ADDITIONAL_STDIO"
                            result = await session.call_tool("Read", {"path": host})
                            assert result.isError
                        policy(False)
                        result = await session.call_tool("Write", {"path": "out/stdio-blocked.txt", "content": "blocked"})
                        assert result.isError and not (workspace / "out/stdio-blocked.txt").exists()
                        if args.additional_directories:
                            result = await session.call_tool("Read", {"path": "../shared/read.txt"})
                            assert result.isError
                            report["directory_mcp_stdio"] = {"write": True, "host_config_private": True, "revocation": True}
                report["official_mcp_stdio"] = {"write": True, "live_reload_denied": True}

            asyncio.run(official())
        report["passed"] = True
    if args.report:
        args.report.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({"passed": True, "invalid_host_cases": report["invalid_host_cases"], "inference": report["inference"],
        "official_mcp_stdio": args.official_stdio}))


if __name__ == "__main__":
    main()
