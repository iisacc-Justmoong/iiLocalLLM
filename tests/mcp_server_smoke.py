"""Official SDK client against the installed or built, unmodified C++ server."""
import argparse
import asyncio
from contextlib import asynccontextmanager
from datetime import timedelta
from importlib.metadata import version
import json
import os
from pathlib import Path
import tempfile
import uuid
import secrets

import httpx

from mcp import ClientSession, StdioServerParameters, types
from mcp.client.stdio import stdio_client
from mcp.client.streamable_http import streamable_http_client
from mcp.shared.exceptions import McpError


@asynccontextmanager
async def transport(command, root, http):
    if not http:
        async with stdio_client(command) as streams:
            yield streams
        return
    credential = secrets.token_urlsafe(32)
    credentials = root / "credentials.json"
    credentials.write_text(json.dumps({"com.iisacc.fixture": credential}))
    credentials.chmod(0o600)
    arguments = list(command.args)
    if "--sessions" in arguments:
        index = arguments.index("--sessions")
        del arguments[index:index + 2]
    arguments += ["--http-port", "0", "--credentials", str(credentials), "--state", str(root / "http-state")]
    with (root / "http-server.log").open("ab") as log:
        process = await asyncio.create_subprocess_exec(command.command, *arguments,
            stdout=asyncio.subprocess.PIPE, stderr=log)
        try:
            line = await asyncio.wait_for(process.stdout.readline(), timeout=15)
            assert line, "HTTP MCP server exited before announcing its endpoint"
            endpoint = json.loads(line)["endpoint"]
            async with httpx.AsyncClient(headers={"Authorization": "Bearer " + credential},
                                         timeout=180, trust_env=False) as client:
                async with streamable_http_client(endpoint, http_client=client) as (read, write, _):
                    yield read, write
        finally:
            if process.returncode is None:
                process.terminate()
            try:
                await asyncio.wait_for(process.wait(), timeout=10)
            except asyncio.TimeoutError:
                process.kill()
                await process.wait()
                raise AssertionError("HTTP MCP shutdown did not join its handlers")
            assert process.returncode == 0, f"HTTP MCP server exited {process.returncode}"


class ObservedSend:
    """Observe SDK-assigned wire IDs without rewriting any protocol messages."""
    def __init__(self, stream):
        self.stream = stream
        self.calls = {}

    async def __aenter__(self):
        await self.stream.__aenter__()
        return self

    async def __aexit__(self, *args):
        return await self.stream.__aexit__(*args)

    async def send(self, message):
        request = message.message.root
        if getattr(request, "method", None) == "tools/call":
            self.calls[request.params["name"]] = request.id
        await self.stream.send(message)


def text(result):
    return "\n".join(part.text for part in result.content if part.type == "text")


async def wait_until(predicate, seconds=5):
    deadline = asyncio.get_running_loop().time() + seconds
    while not predicate():
        assert asyncio.get_running_loop().time() < deadline, "Condition did not become true"
        await asyncio.sleep(0.01)


def alive(pid):
    try:
        os.kill(pid, 0)
        return True
    except ProcessLookupError:
        return False


async def basic(binary, root, http=False):
    workspace = root / "workspace"
    workspace.mkdir()
    secret = "MCP_SERVER_" + uuid.uuid4().hex
    (workspace / "secret.txt").write_text(secret)
    (root / "outside.txt").write_text("NOT_EXPOSED")
    command = StdioServerParameters(command=str(binary), args=["--workspace", str(workspace)])
    async with transport(command, root, http) as (read, write):
        async with ClientSession(read, write, read_timeout_seconds=timedelta(seconds=10)) as session:
            info = await session.initialize()
            assert info.protocolVersion == "2025-11-25"
            definitions = (await session.list_tools()).tools
            assert {item.name for item in definitions} == {"Read", "Write", "Edit", "Glob", "Grep", "Bash"}
            assert all(item.meta["iisacc/appId"] == "com.iisacc.iiLocalLLM" for item in definitions)
            result = await session.call_tool("Read", {"path": "secret.txt"})
            assert not result.isError and text(result) == secret
            denied = await session.call_tool("Write", {"path": "blocked.txt", "content": "denied"})
            assert denied.isError and not (workspace / "blocked.txt").exists()
            invalid = await session.call_tool("Read", {})
            assert invalid.isError
            escape = await session.call_tool("Read", {"path": "../outside.txt"})
            assert escape.isError and "NOT_EXPOSED" not in text(escape)
            try:
                await session.call_tool("unknown", {})
                raise AssertionError("Unknown tool was accepted")
            except McpError as error:
                assert error.error.code == -32602
            await session.send_ping()
    command.args += ["--allow", "Write", "--allow", "Bash"]
    async with transport(command, root, http) as (read, write):
        observer = ObservedSend(write)
        async with ClientSession(read, observer, read_timeout_seconds=timedelta(seconds=10)) as session:
            await session.initialize()
            result = await session.call_tool("Write", {"path": "created.txt", "content": secret})
            assert not result.isError and (workspace / "created.txt").read_text() == secret
            command_text = 'printf "%s" "$$" > shell.pid; sleep 30 & printf "%s" "$!" > child.pid; wait; printf late > late.txt'
            pending = asyncio.create_task(session.call_tool("Bash", {"command": command_text, "timeout_ms": 60000}))
            try:
                await wait_until(lambda: (workspace / "child.pid").exists() and (workspace / "child.pid").stat().st_size > 0)
                shell = int((workspace / "shell.pid").read_text())
                child = int((workspace / "child.pid").read_text())
                await session.send_notification(types.ClientNotification(types.CancelledNotification(
                    method="notifications/cancelled", params=types.CancelledNotificationParams(
                        requestId=observer.calls["Bash"], reason="Integration test cancellation"))))
                await wait_until(lambda: not alive(shell) and not alive(child))
                assert not (workspace / "late.txt").exists()
            finally:
                pending.cancel()
                try:
                    await pending
                except asyncio.CancelledError:
                    pass
            await session.send_ping()
            assert text(await session.call_tool("Read", {"path": "created.txt"})) == secret
    return {"official_sdk": version("mcp"), "transport": "http" if http else "stdio", "tools": 6, "real_file_read_write": True,
            "permission_denial": True, "schema_validation": True, "workspace_boundary": True,
            "cancelled_shell_and_child_exited": True, "connection_survived_cancellation": True}


async def native(binary, root, weights, http=False):
    workspace = root / "workspace"
    workspace.mkdir()
    secret = "LOCAL_" + uuid.uuid4().hex[:12]
    (workspace / "secret.txt").write_text(secret)
    package = root / "Models" / "agent-fixture"
    package.mkdir(parents=True)
    os.link(weights, package / "model.gguf")
    manifest = {"schema_version": 1, "id": "agent-fixture", "architecture": "qwen2", "format": "gguf",
                "quantization": "Q4_K_M", "context_length": 32768, "entry_point": "model.gguf",
                "capabilities": ["text-generation", "chat"], "files": [{"path": "model.gguf", "size": 491400032,
                "sha256": "74a4da8c9fdbcd15bd1f6d01d621410d31c6fc00986f5eb687824e7b93d7a9db"}]}
    (package / "manifest.json").write_text(json.dumps(manifest))
    command = StdioServerParameters(command=str(binary), args=["--workspace", str(workspace),
        "--models", str(root / "Models"), "--model", "model://agent-fixture", "--sessions", str(root / "sessions"),
        "--temperature", "0", "--request-timeout", "120000"])
    updates = []

    async def progress(value, total, message):
        updates.append({"progress": value, "total": total, "message": message})

    async with transport(command, root, http) as (read, write):
        async with ClientSession(read, write, read_timeout_seconds=timedelta(seconds=150)) as session:
            await session.initialize()
            names = {tool.name for tool in (await session.list_tools()).tools}
            assert {"iiLocalLLM.agent.run", "iiLocalLLM.agent.session"} <= names
            result = await session.call_tool("iiLocalLLM.agent.run", {
                "prompt": "Use the Read tool to read secret.txt. Then return the exact file contents as your final answer. Do not guess.",
                "max_turns": 4}, progress_callback=progress)
            run = result.structuredContent
            assert not result.isError and run["status"] == "completed", result
            assert secret in run["text"] and run["turns"] >= 2 and run["usage"]["generated_tokens"] > 0, run
            saved = (await session.call_tool("iiLocalLLM.agent.session", {"include_messages": True})).structuredContent
            messages = saved["messages"]
            assert saved["session_id"] == run["session_id"]
            calls = [call for message in messages for call in message["tool_calls"]]
            assert any(call["name"] == "Read" for call in calls), messages
            assert any(message["role"] == "tool" and secret in message["text"] for message in messages)
            assert {call["id"] for call in calls} == {message["tool_call_id"] for message in messages if message["role"] == "tool"}
            assert len(updates) >= 3 and all(a["progress"] < b["progress"] for a, b in zip(updates, updates[1:]))
    assert list((root / "http-state" / "sessions" if http else root / "sessions").glob("**/*.jsonl")), "Agent transcript was not persisted"
    return {"official_sdk": version("mcp"), "transport": "http" if http else "stdio", "native_model": "Qwen2.5 0.5B Q4_K_M", "run": run,
            "progress": updates, "transcript": saved, "unpredictable_file_value_verified": True}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("binary", type=Path)
    parser.add_argument("--native", type=Path)
    parser.add_argument("--report", type=Path)
    parser.add_argument("--http", action="store_true")
    args = parser.parse_args()
    assert version("mcp") == "1.26.0"
    with tempfile.TemporaryDirectory(prefix="mcp-server-") as directory:
        root = Path(directory)
        report = asyncio.run(native(args.binary.resolve(), root, args.native.resolve(), args.http) if args.native
                             else basic(args.binary.resolve(), root, args.http))
    if args.report:
        args.report.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n")
    print(json.dumps(report, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
