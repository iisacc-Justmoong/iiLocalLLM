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
import sys

import httpx

from mcp import ClientSession, StdioServerParameters, types
from mcp.client.stdio import stdio_client
from mcp.client.streamable_http import streamable_http_client
from mcp.shared.exceptions import McpError


@asynccontextmanager
async def transport(command, root, http):
    # The official stdio client inherits only a small environment allowlist.
    # Explicitly isolate both child transports from running user applications.
    temporary = root / "tmp"
    temporary.mkdir(exist_ok=True)
    command.env = {**(command.env or {}), "IILOCALLLM_APP_ENDPOINTS": str(root / "app-endpoints"),
                   "TMPDIR": str(temporary), "TMP": str(temporary), "TEMP": str(temporary)}
    if not http:
        async with stdio_client(command) as streams:
            yield streams
        return
    credential = secrets.token_urlsafe(32)
    credentials = root / "credentials.json"
    credentials.write_text(json.dumps({"com.iisacc.fixture": credential}))
    credentials.chmod(0o600)
    arguments = list(command.args)
    for flag in ("--sessions", "--state"):
        if flag in arguments:
            index = arguments.index(flag)
            del arguments[index:index + 2]
    arguments += ["--http-port", "0", "--credentials", str(credentials), "--state", str(root / "http-state")]
    with (root / "http-server.log").open("ab") as log:
        process = await asyncio.create_subprocess_exec(command.command, *arguments,
            stdout=asyncio.subprocess.PIPE, stderr=log, env={**os.environ, **command.env})
        try:
            # Explicit model options preload and verify the model before listening.
            startup_timeout = 120 if "--model-options" in arguments else 15
            line = await asyncio.wait_for(process.stdout.readline(), timeout=startup_timeout)
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


def read_text(result):
    # 0.10 publishes the structured result as a separate MCP text block too.
    # Preserve the exact file bytes and independently verify the JSON projection.
    assert not result.isError and len(result.content) == 2, result
    first, structured = result.content
    assert first.type == structured.type == "text", result
    assert json.loads(structured.text) == result.structuredContent, result
    return first.text


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
    command = StdioServerParameters(command=str(binary), args=["--workspace", str(workspace), "--allow", "Bash"])
    async with transport(command, root, http) as (read, write):
        async with ClientSession(read, write, read_timeout_seconds=timedelta(seconds=10)) as session:
            info = await session.initialize()
            assert info.protocolVersion == "2025-11-25"
            definitions = (await session.list_tools()).tools
            task_names = {"TaskCreate", "TaskGet", "TaskList", "TaskUpdate", "TaskClaim", "TodoWrite", "TodoRead"}
            shell_names = {"TaskOutput", "TaskStop", "ShellTaskList"}
            assert {item.name for item in definitions} == {"Read", "Write", "Edit", "Glob", "Grep", "Bash"} | task_names | shell_names, sorted(item.name for item in definitions)
            assert all(item.meta["iisacc/appId"] == "com.iisacc.iiLocalLLM" for item in definitions)
            created = await session.call_tool("TaskCreate", {"subject": "Verify package", "description": "Inspect the installed output"})
            assert not created.isError and created.structuredContent["task"]["id"] == "1", created
            claimed = await session.call_tool("TaskClaim", {"taskId": "1", "owner": "official-client"})
            assert not claimed.isError and claimed.structuredContent["success"], claimed
            wrong_revision = await session.call_tool("TaskUpdate", {"taskId": "1", "status": "completed", "expectedRevision": 0})
            assert wrong_revision.isError, wrong_revision
            listed = await session.call_tool("TaskList", {})
            assert listed.structuredContent["tasks"][0]["status"] == "in_progress", listed
            completed = await session.call_tool("TaskUpdate", {"taskId": "1", "status": "completed"})
            assert not completed.isError and completed.structuredContent["task"]["status"] == "completed", completed
            todos = [{"content": "Read result", "activeForm": "Reading result", "status": "completed"}]
            assert not (await session.call_tool("TodoWrite", {"todos": todos})).isError
            assert (await session.call_tool("TodoRead", {})).structuredContent["todos"] == todos
            assert (await session.call_tool("TaskGet", {"taskId": "1", "listId": "other"})).isError
            started = await session.call_tool("Bash", {"command": "sleep 0.1; cat secret.txt", "run_in_background": True})
            assert not started.isError, started
            shell_id = started.structuredContent["backgroundTaskId"]
            output = await session.call_tool("TaskOutput", {"task_id": shell_id, "timeout": 5000})
            assert not output.isError and output.structuredContent["task"]["status"] == "completed", output
            assert output.structuredContent["task"]["output"] == secret, output
            output_path = output.structuredContent["task"]["output_file"]
            assert read_text(await session.call_tool("Read", {"path": output_path})) == secret
            active = await session.call_tool("Bash", {"command": "sleep 30", "run_in_background": True})
            stopped = await session.call_tool("TaskStop", {"task_id": active.structuredContent["backgroundTaskId"]})
            assert not stopped.isError and stopped.structuredContent["status"] == "killed", stopped
            result = await session.call_tool("Read", {"path": "secret.txt"})
            assert read_text(result) == secret
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
            assert (await session.call_tool("TaskList", {})).structuredContent["tasks"] == []
            assert (await session.call_tool("ShellTaskList", {})).structuredContent["tasks"] == []
            assert (await session.call_tool("TaskOutput", {"task_id": shell_id, "block": False})).isError
            assert (await session.call_tool("Read", {"path": output_path})).isError
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
            assert read_text(await session.call_tool("Read", {"path": "created.txt"})) == secret
    command.args += ["--no-tasks", "--no-background"]
    async with transport(command, root, http) as (read, write):
        async with ClientSession(read, write) as session:
            await session.initialize()
            assert {item.name for item in (await session.list_tools()).tools} == {"Read", "Write", "Edit", "Glob", "Grep", "Bash"}
    return {"official_sdk": version("mcp"), "transport": "http" if http else "stdio", "tools": 16, "real_file_read_write": True,
            "background_shell_start_output_stop": True, "background_shell_connection_isolation": True, "background_shell_host_opt_out": True,
            "task_create_claim_complete_todos": True, "task_revision_conflict": True, "task_connection_isolation": True, "task_host_opt_out": True,
            "permission_denial": True, "schema_validation": True, "workspace_boundary": True,
            "cancelled_shell_and_child_exited": True, "connection_survived_cancellation": True}


async def native(binary, root, weights, http=False, skill_permissions=False, catalog=None, model_uri=None):
    workspace = root / "workspace"
    workspace.mkdir()
    secret = "LOCAL_" + uuid.uuid4().hex[:12]
    (workspace / "secret.txt").write_text(secret)
    skill_dir = workspace / ".claude" / "skills" / "inspect"
    skill_dir.mkdir(parents=True)
    (skill_dir / "SKILL.md").write_text("---\ndescription: Inspect a file\ndisable-model-invocation: true\n---\nUse the Read tool to read $0. Then return the exact file contents as your final answer. Do not guess.\n")
    manifest = {"schema_version": 1, "id": "agent-fixture", "architecture": "qwen2", "format": "gguf",
                "quantization": "Q4_K_M", "context_length": 32768, "entry_point": "model.gguf",
                "capabilities": ["text-generation", "chat"], "files": [{"path": "model.gguf", "size": 491400032,
                "sha256": "74a4da8c9fdbcd15bd1f6d01d621410d31c6fc00986f5eb687824e7b93d7a9db"}]}
    if catalog:
        assert model_uri and model_uri.startswith("model://")
        source = catalog / model_uri.removeprefix("model://")
        manifest = json.loads((source / "manifest.json").read_text())
        assert manifest["id"] == model_uri.removeprefix("model://")
    package = root / "Models" / manifest["id"]
    package.mkdir(parents=True)
    for entry in manifest["files"]:
        target = package / entry["path"]
        target.parent.mkdir(parents=True, exist_ok=True)
        os.link(source / entry["path"] if catalog else weights, target)
    (package / "manifest.json").write_text(json.dumps(manifest))
    command = StdioServerParameters(command=str(binary), args=["--workspace", str(workspace),
        "--models", str(root / "Models"), "--model", "model://" + manifest["id"], "--state", str(root / "stdio-state"),
        "--allow", "Agent", "--allow", "AgentStop", "--temperature", "0", "--request-timeout", "120000"])
    model_options = {}
    if catalog:
        model_options = {"enable_thinking": False, "tool_grammar": False}
        options_file = root / "model-options.json"
        options_file.write_text(json.dumps(model_options))
        options_file.chmod(0o600)
        command.args += ["--context", "8192", "--max-tokens", "2048", "--model-options", str(options_file)]
    updates = []

    async def progress(value, total, message):
        updates.append({"progress": value, "total": total, "message": message})

    async with transport(command, root, http) as (read, write):
        async with ClientSession(read, write, read_timeout_seconds=timedelta(seconds=150)) as session:
            await session.initialize()
            names = {tool.name for tool in (await session.list_tools()).tools}
            assert {"iiLocalLLM.agent.run", "iiLocalLLM.agent.session"} <= names
            assert {"iiLocalLLM.agent.agents." + action for action in ("run", "output", "stop", "list", "profiles")} <= names
            profile_dir = workspace / ".claude" / "agents"
            profile_dir.mkdir(parents=True)
            (profile_dir / "reader.md").write_text("---\nname: reader\ndescription: Read one file\ntools: Read\n---\nUse Read to inspect the requested file. Report its exact contents; do not invent observations.\n")
            profiles = await session.call_tool("iiLocalLLM.agent.agents.profiles", {})
            assert not profiles.isError and "reader" in {p["name"] for p in profiles.structuredContent["profiles"]}
            assert all("system_prompt" not in p for p in profiles.structuredContent["profiles"])
            catalog = await session.call_tool("iiLocalLLM.agent.skills.list", {})
            assert not catalog.isError and len(catalog.structuredContent["skills"]) == 1
            assert "content" not in catalog.structuredContent["skills"][0]
            result = await session.call_tool("iiLocalLLM.agent.run", {
                "skill": "inspect", "skill_arguments": "secret.txt",
                "max_turns": 4}, progress_callback=progress)
            run = result.structuredContent
            assert not result.isError and run["status"] == "completed", result
            assert secret in run["text"] and run["turns"] >= 2 and run["usage"]["generated_tokens"] > 0, run
            saved = (await session.call_tool("iiLocalLLM.agent.session", {"include_messages": True})).structuredContent
            messages = saved["messages"]
            assert any("iilocal.skill" in message.get("metadata", {}) for message in messages), messages
            assert saved["session_id"] == run["session_id"]
            calls = [call for message in messages for call in message["tool_calls"]]
            assert any(call["name"] == "Read" for call in calls), messages
            assert any(message["role"] == "tool" and secret in message["text"] for message in messages)
            assert {call["id"] for call in calls} == {message["tool_call_id"] for message in messages if message["role"] == "tool"}
            assert len(updates) >= 3 and all(a["progress"] < b["progress"] for a, b in zip(updates, updates[1:]))
            child_secret = "CHILD_" + secrets.token_hex(8)
            (workspace / "child.txt").write_text(child_secret)
            child_prompt = "Use Read to read child.txt now, then return its exact current contents. Do not guess."
            child_result = await session.call_tool("iiLocalLLM.agent.agents.run", {"prompt": child_prompt, "subagent_type": "reader", "max_turns": 4})
            child = child_result.structuredContent
            assert not child_result.isError and child["status"] == "completed" and child_secret in child["result"]["text"], child_result
            resumed_secret = "CHILD_" + secrets.token_hex(8)
            (workspace / "child.txt").write_text(resumed_secret)
            launched = await session.call_tool("iiLocalLLM.agent.agents.run", {"prompt": child_prompt, "resume": child["agentId"], "run_in_background": True, "max_turns": 4})
            assert not launched.isError and launched.structuredContent["status"] == "async_launched", launched
            output = await session.call_tool("iiLocalLLM.agent.agents.output", {"agent_id": child["agentId"], "block": True, "timeout_ms": 60000})
            resumed = output.structuredContent
            assert not output.isError and resumed["finished"] and resumed["status"] == "completed" and resumed_secret in resumed["result"]["text"], output
            assert resumed["session_id"] == child["session_id"]
            listed = await session.call_tool("iiLocalLLM.agent.agents.list", {})
            assert len(listed.structuredContent["agents"]) == 1 and listed.structuredContent["agents"][0]["agentId"] == child["agentId"]
            stopped = await session.call_tool("iiLocalLLM.agent.agents.stop", {"agent_id": child["agentId"]})
            assert not stopped.isError and not stopped.structuredContent["stop_requested"]
            notifications = await session.call_tool("iiLocalLLM.agent.inputs.list", {})
            assert notifications.structuredContent["count"] == 1
            private_state = root / ("http-state" if http else "stdio-state")
            child_transcript = private_state / "subagents" / "sessions" / child["session_id"] / "transcript.jsonl"
            child_messages = [row["message"] for row in map(json.loads, child_transcript.read_text().splitlines()) if row["type"] == "message"]
            child_calls = [call for message in child_messages for call in message["tool_calls"]]
            assert sum(call["name"] == "Read" for call in child_calls) >= 2
            assert all(any(m["role"] == "tool" and code in m["text"] for m in child_messages) for code in (child_secret, resumed_secret))
            assert {call["id"] for call in child_calls} == {m["tool_call_id"] for m in child_messages if m["role"] == "tool"}
            fork_secret = "FORK_" + secrets.token_hex(8)
            (workspace / "fork.txt").write_text(fork_secret)
            (skill_dir / "SKILL.md").write_text("---\ndescription: Inspect in a child\ncontext: fork\nagent: reader\ndisable-model-invocation: true\n---\n"
                "FORK_PRIVATE_BODY parent=${CLAUDE_SESSION_ID}. Use Read to read $0 and return its exact contents. Do not guess.\n")
            forked = await session.call_tool("iiLocalLLM.agent.run", {"skill": "inspect", "skill_arguments": "fork.txt", "max_turns": 4}, progress_callback=progress)
            fork_run = forked.structuredContent
            assert not forked.isError and fork_run["status"] == "completed" and fork_secret in fork_run["text"], forked
            assert fork_run["session_id"] == saved["session_id"]
            fork_parent = (await session.call_tool("iiLocalLLM.agent.session", {"include_messages": True})).structuredContent
            assert len(fork_parent["messages"]) == len(messages) + 2
            assert not any("FORK_PRIVATE_BODY" in m["text"] for m in fork_parent["messages"])
            execution = fork_parent["messages"][-1]["metadata"]["iilocal.skill_fork"]
            fork_path = private_state / "subagents/sessions" / execution["session_id"] / "transcript.jsonl"
            fork_messages = [r["message"] for r in map(json.loads, fork_path.read_text().splitlines()) if r["type"] == "message"]
            assert any("parent=" + saved["session_id"] in m["text"] for m in fork_messages)
            assert any(c["name"] == "Read" for m in fork_messages for c in m["tool_calls"])
            assert any(m["role"] == "tool" and fork_secret in m["text"] for m in fork_messages)
            assert all(code not in m["text"] for m in fork_messages for code in (secret, child_secret, resumed_secret))
            permission_results = []
            if skill_permissions:
                (profile_dir / "writer.md").write_text("---\nname: writer\ndescription: Write one file\ntools: Write\n---\nUse Write exactly as requested. Return DONE after it succeeds.\n")
                for mode in ("inline", "fork"):
                    (skill_dir / "SKILL.md").write_text("---\ndescription: Write an exact file\ndisable-model-invocation: true\nallowed-tools: 'Write(grant-*.txt)'\n"
                        + ("context: fork\nagent: writer\n" if mode == "fork" else "")
                        + "---\nCall Write with path=$0 and content=$1 exactly without an added newline. Then return DONE.\n")
                    name, value = "grant-" + mode + ".txt", "GRANTED_" + secrets.token_hex(6)
                    observed = await session.call_tool("iiLocalLLM.agent.run", {"skill": "inspect", "skill_arguments": name + " " + value, "max_turns": 4})
                    assert not observed.isError and observed.structuredContent["status"] == "completed", observed
                    assert (workspace / name).is_file(), observed
                    assert (workspace / name).read_text() == value, observed
                    after = (await session.call_tool("iiLocalLLM.agent.session", {"include_messages": True})).structuredContent
                    records = after["messages"]
                    if mode == "fork":
                        grant_execution = records[-1]["metadata"]["iilocal.skill_fork"]
                        path = private_state / "subagents/sessions" / grant_execution["session_id"] / "transcript.jsonl"
                        records = [r["message"] for r in map(json.loads, path.read_text().splitlines()) if r["type"] == "message"]
                    writes = [c for m in records for c in m["tool_calls"] if c["name"] == "Write"]
                    def target_path(call):
                        path = Path(call["arguments"]["path"])
                        return (path if path.is_absolute() else workspace / path).resolve()
                    assert writes and all(target_path(c) == (workspace / name).resolve() and c["arguments"]["content"] == value for c in writes), {"mode": mode, "writes": writes, "run": observed.structuredContent}
                    write_results = [m for m in records if m["role"] == "tool" and m["tool_call_id"] in {c["id"] for c in writes}]
                    assert len(write_results) == len(writes) and all(not m["is_error"] for m in write_results), write_results
                    # Direct MCP tools share host policy, never a previous agent run's grants.
                    denied = await session.call_tool("Write", {"path": "grant-outside-run.txt", "content": "DENIED"})
                    assert denied.isError and not (workspace / "grant-outside-run.txt").exists()
                    invalid = await session.call_tool("iiLocalLLM.agent.run", {"prompt": "invalid", "allowed_tools": ["Write"]})
                    assert invalid.isError
                    permission_results.append({"mode": mode, "run": observed.structuredContent, "actual_write": writes[0], "actual_write_calls": len(writes),
                        "write_calls": writes, "write_results": write_results,
                        "outside_invocation_denied": True, "wire_grants_rejected": True})
    assert list((private_state / "sessions").glob("**/*.jsonl")), "Agent transcript was not persisted"
    return {"official_sdk": version("mcp"), "transport": "http" if http else "stdio", "native_model": manifest["id"], "model_manifest": manifest, "run": run,
            "model_options": model_options,
            "progress": updates, "transcript": saved, "unpredictable_file_value_verified": True, "skill_fork": {"run": fork_run, "execution": execution, "actual_read_verified": True},
            "skill_permissions": permission_results,
            "subagent": {"foreground": child, "resumed": resumed, "actual_read_calls": sum(c["name"] == "Read" for c in child_calls), "notification_count": 1}}


async def inputs(binary, root, http=False):
    workspace = root / "workspace"
    workspace.mkdir()
    catalog = root / "Models"
    catalog.mkdir()
    command = StdioServerParameters(command=str(binary), args=["--workspace", str(workspace),
        "--models", str(catalog), "--model", "model://not-loaded", "--sessions", str(root / "sessions"),
        "--allow", "iiLocalLLM.agent.inputs.*"])
    async with transport(command, root, http) as (read, write):
        async with ClientSession(read, write, read_timeout_seconds=timedelta(seconds=20)) as session:
            await session.initialize()
            names = {tool.name for tool in (await session.list_tools()).tools}
            assert {"iiLocalLLM.agent.inputs." + action for action in ("enqueue", "list", "remove", "run")} <= names
            async def call(action, arguments=None):
                result = await session.call_tool("iiLocalLLM.agent.inputs." + action, arguments or {})
                assert not result.isError, result
                return result.structuredContent
            later = (await call("enqueue", {"text": "notification", "kind": "notification"}))["input"]
            now = (await call("enqueue", {"text": "urgent", "priority": "now"}))["input"]
            state = await call("list", {"limit": 1})
            assert state["count"] == 2 and state["inputs"] == [now] and state["next_offset"] == 1, state
            assert (await call("list", {"offset": 1}))["inputs"] == [later]
            invalid = await session.call_tool("iiLocalLLM.agent.inputs.enqueue", {"text": "bad", "priority": "invalid"})
            assert invalid.isError
            assert (await call("remove", {"input_id": now["id"]}))["removed"]
            assert (await call("remove", {"input_id": later["id"]}))["removed"]
            empty = await session.call_tool("iiLocalLLM.agent.inputs.run", {})
            assert empty.isError
            assert (await call("list"))["count"] == 0
            saved = (await session.call_tool("iiLocalLLM.agent.session", {"include_messages": True})).structuredContent
            assert saved["messages"] == [], saved
    return {"official_sdk": version("mcp"), "transport": "http" if http else "stdio", "native_inference": False,
            "input_controls_verified": ["discovery", "enqueue", "priority", "pagination", "schema", "remove", "empty_run", "no_placeholder_message"]}


async def managed(binary, root, http=False):
    workspace = root / "workspace"
    workspace.mkdir()
    (workspace / ".mcp.json").write_text(json.dumps({"mcpServers": {"fixture": {"command": sys.executable,
        "args": ["-B", str(Path(__file__).with_name("mcp_peer.py").resolve())], "appId": "com.iisacc.fixture"}}}))
    command = StdioServerParameters(command=str(binary), args=["--workspace", str(workspace),
        "--mcp-project", "--allow", "mcp__fixture__echo"])
    async with transport(command, root, http) as (read, write):
        async with ClientSession(read, write, read_timeout_seconds=timedelta(seconds=10)) as session:
            await session.initialize()
            names = {tool.name for tool in (await session.list_tools()).tools}
            assert {"mcp__fixture__echo", "mcp__fixture__other"} <= names
            value = "DISCOVERY_" + secrets.token_hex(8)
            result = await session.call_tool("mcp__fixture__echo", {"value": value})
            assert not result.isError and result.structuredContent == {"value": value}, result
            denied = await session.call_tool("mcp__fixture__other", {"value": "denied"})
            assert denied.isError
    return {"official_sdk": version("mcp"), "transport": "http" if http else "stdio",
            "configured_mcp_forwarding": True, "remote_annotations_do_not_grant_permission": True}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("binary", type=Path)
    parser.add_argument("--native", type=Path)
    parser.add_argument("--native-catalog", type=Path)
    parser.add_argument("--native-model")
    parser.add_argument("--report", type=Path)
    parser.add_argument("--http", action="store_true")
    parser.add_argument("--managed", action="store_true")
    parser.add_argument("--inputs", action="store_true")
    parser.add_argument("--skill-permissions", action="store_true")
    args = parser.parse_args()
    assert version("mcp") == "1.26.0"
    with tempfile.TemporaryDirectory(prefix="mcp-server-") as directory:
        root = Path(directory)
        report = asyncio.run(native(args.binary.resolve(), root, args.native.resolve() if args.native else None, args.http, args.skill_permissions,
                                   args.native_catalog.resolve() if args.native_catalog else None, args.native_model) if args.native or args.native_catalog
                             else inputs(args.binary.resolve(), root, args.http) if args.inputs
                             else managed(args.binary.resolve(), root, args.http) if args.managed else basic(args.binary.resolve(), root, args.http))
    if args.report:
        args.report.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n")
    print(json.dumps(report, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
