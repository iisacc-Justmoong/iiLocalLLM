"""Qualify installed local plugins over real daemon HTTP/IPC and MCP HTTP/stdio.

No inference is claimed here. The C++ runtime fixture qualifies native inference.
Only test code depends on the official Python MCP client.
"""
import argparse
import asyncio
from contextlib import contextmanager
from http.client import HTTPConnection
import importlib.metadata
import json
import os
from pathlib import Path
import re
import secrets
import shutil
import subprocess
import tempfile
import time


def main():
    parser = argparse.ArgumentParser()
    for name in ("daemon", "cli", "mcp", "plugins", "peer"):
        parser.add_argument(name, type=Path)
    parser.add_argument("--report", required=True, type=Path)
    args = parser.parse_args()
    binaries = {name: str(getattr(args, name).resolve()) for name in ("daemon", "cli", "mcp", "plugins", "peer")}
    report = {"passed": False, "native_inference": False, "binaries": binaries}
    env = dict(os.environ)
    for key in ("DYLD_LIBRARY_PATH", "DYLD_FRAMEWORK_PATH", "DYLD_FALLBACK_LIBRARY_PATH", "LIBRARY_PATH"):
        env.pop(key, None)
    args.report.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="plw-", dir=args.report.parent) as directory:
        root = Path(directory).resolve()
        work, source, store = [root / p for p in ("work", "source", "store")]
        work.mkdir()
        (source / ".claude-plugin").mkdir(parents=True)
        (source / "skills" / "inspect").mkdir(parents=True)
        shutil.copy2(binaries["peer"], source / "peer")
        (source / "skills/inspect/SKILL.md").write_text("---\ndescription: Wire plugin\n---\nInspect the plugin peer.\n")
        (source / ".claude-plugin/plugin.json").write_text(json.dumps({
            "name": "wire", "version": "1",
            "mcpServers": {"peer": {"command": "${CLAUDE_PLUGIN_ROOT}/peer", "env": {"PLUGIN_SECRET": "WIRE_PLUGIN_VALUE"}}}
        }))
        token, other = secrets.token_urlsafe(36), secrets.token_urlsafe(36)
        credentials, auth = root / "credentials", root / "token"
        credentials.write_text(json.dumps({"society": token, "dreamscapes": other}))
        auth.write_text(token)
        credentials.chmod(0o600)
        auth.chmod(0o600)

        def command(exe, *values):
            return subprocess.run([binaries[exe], *map(str, values)], cwd=root, env=env, check=True, capture_output=True, text=True, timeout=45).stdout

        command("plugins", "--store", store, "install", source)

        @contextmanager
        def server(name, values, pattern):
            path = args.report.with_suffix("." + name + ".log")
            with path.open("w") as log:
                process = subprocess.Popen(values, cwd=root, env=env, stdout=log, stderr=log)
                try:
                    deadline = time.monotonic() + 45
                    while True:
                        output = path.read_text()
                        assert process.poll() is None, output[-5000:]
                        match = re.search(pattern, output)
                        if match:
                            break
                        assert time.monotonic() < deadline, output[-5000:]
                        time.sleep(.025)
                    yield int(match[1])
                finally:
                    process.terminate()
                    try:
                        process.wait(timeout=15)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait()

        def post(port, endpoint, method, params=None, credential=token, session=None):
            payload = {"id": secrets.token_hex(6), "method": method, "params": params or {}}
            headers = {"Content-Type": "application/json", "Accept": "application/json, text/event-stream", "Authorization": "Bearer " + credential}
            if endpoint == "/mcp":
                payload["jsonrpc"] = "2.0"
                if method.startswith("notifications/"):
                    payload.pop("id")
                if session:
                    headers.update({"Mcp-Session-Id": session, "MCP-Protocol-Version": "2025-11-25"})
            connection = HTTPConnection("127.0.0.1", port, timeout=40)
            try:
                connection.request("POST", endpoint, json.dumps(payload), headers)
                response = connection.getresponse()
                body = response.read()
                if response.getheader("Content-Type", "").startswith("text/event-stream"):
                    frames = [json.loads(line[5:]) for line in body.splitlines() if line.startswith(b"data:") and line[5:].strip()]
                    value = next(v for v in frames if v.get("id") == payload["id"] and ("result" in v or "error" in v))
                else:
                    value = json.loads(body) if body else {}
                return response.status, value, response.getheader("Mcp-Session-Id")
            finally:
                connection.close()

        try:
            daemon = [binaries["daemon"], "--socket", str(root / "s"), "--http-port", "0", "--models-root", str(root / "models"),
                      "--agent-workspace", str(work), "--agent-state", str(root / "api"), "--agent-credentials", str(credentials),
                      "--agent-plugins", str(store), "--agent-no-worktrees", "--agent-no-apps", "--agent-allow", "*"]
            with server("api", daemon, r"iiLocalLLM HTTP: http://127\.0\.0\.1:(\d+)") as port:
                def rpc(method, params=None):
                    status, value, _ = post(port, "/v1/rpc", method, params)
                    assert status == 200 and "error" not in value, (status, value)
                    return value["result"]
                snapshot = rpc("agent.plugins.list")
                assert snapshot["plugins"][0]["name"] == "wire" and snapshot["plugins"][0]["contributions"]["mcp"] == 1, snapshot
                assert "WIRE_PLUGIN_VALUE" not in json.dumps(snapshot) and str(root) not in json.dumps(snapshot)
                owner = rpc("agent.sessions.create", {"model": "model://absent-plugin-fixture"})["session_id"]
                assert rpc("agent.skills.list", {"session_id": owner})["skills"][0]["name"] == "wire:inspect"
                assert post(port, "/v1/rpc", "agent.skills.list", {"session_id": owner}, other)[0] != 200
                assert post(port, "/v1/rpc", "agent.plugins.list", {}, "invalid")[0] != 200
                value = json.loads(command("cli", "--socket", root / "s", "--auth-file", auth, "agent", "plugins"))
                assert value == snapshot, value
                command("plugins", "--store", store, "disable", "wire")
                assert rpc("agent.plugins.list") == snapshot
                assert json.loads(command("plugins", "--store", store, "inspect"))["plugins"][0]["status"] == "disabled"
                report.update(http=True, ipc_cli=True, auth_isolation=True, frozen_running_snapshot=True)
            command("plugins", "--store", store, "enable", "wire")
            common = ["--workspace", str(work), "--models", str(root / "models"), "--model", "model://absent-plugin-fixture",
                      "--plugins", str(store), "--allow", "*", "--no-worktrees", "--no-apps", "--mcp-eager"]
            init_args = {"protocolVersion": "2025-11-25", "capabilities": {}, "clientInfo": {"name": "plugin-wire", "version": "1"}}
            with server("mcp-http", [binaries["mcp"], *common, "--state", str(root / "mh"), "--http-port", "0", "--credentials", str(credentials)], r"http://127\.0\.0\.1:(\d+)/mcp") as port:
                status, value, session = post(port, "/mcp", "initialize", init_args)
                assert status == 200 and session, value
                post(port, "/mcp", "notifications/initialized", {}, session=session)
                _, value, _ = post(port, "/mcp", "tools/call", {"name": "iiLocalLLM.agent.plugins.list", "arguments": {}}, session=session)
                assert value["result"]["structuredContent"]["revision"] == snapshot["revision"], value
                _, value, _ = post(port, "/mcp", "tools/call", {"name": "mcp__plugin_wire_peer__echo", "arguments": {}}, session=session)
                assert not value["result"].get("isError") and "WIRE_PLUGIN_VALUE" in json.dumps(value), value
                report["mcp_http"] = True
            from mcp import ClientSession, StdioServerParameters
            from mcp.client.stdio import stdio_client

            async def official():
                parameters = StdioServerParameters(command=binaries["mcp"], args=[*common, "--state", str(root / "ms")], env=env, cwd=str(root))
                with args.report.with_suffix(".stdio.log").open("w") as log:
                    async with stdio_client(parameters, errlog=log) as (reader, writer):
                        async with ClientSession(reader, writer) as client:
                            await client.initialize()
                            tools = await client.list_tools()
                            assert any(t.name == "iiLocalLLM.agent.plugins.list" for t in tools.tools)
                            result = await client.call_tool("iiLocalLLM.agent.plugins.list", {})
                            assert not result.isError and result.structuredContent["revision"] == snapshot["revision"]
                            result = await client.call_tool("mcp__plugin_wire_peer__echo", {})
                            assert not result.isError and "WIRE_PLUGIN_VALUE" in result.model_dump_json()
                report["official_stdio"] = True
            asyncio.run(official())
            report.update(passed=True, python_mcp_version=importlib.metadata.version("mcp"))
        finally:
            args.report.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n")
    print(json.dumps(report, ensure_ascii=False))


if __name__ == "__main__":
    main()
