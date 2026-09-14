"""Qualify file profiles and isolated skills through the daemon API and thin IPC CLI."""
import argparse
import hashlib
from http.client import HTTPConnection
import json
import os
from pathlib import Path
import re
import secrets
import subprocess
import tempfile
import time


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("daemon", type=Path)
    parser.add_argument("cli", type=Path)
    parser.add_argument("catalog", type=Path)
    parser.add_argument("model")
    parser.add_argument("--report", type=Path)
    parser.add_argument("--skill-forks", action="store_true")
    parser.add_argument("--skill-permissions", action="store_true")
    args = parser.parse_args()
    daemon, cli, catalog = (p.resolve() for p in (args.daemon, args.cli, args.catalog))
    report = {"daemon": str(daemon), "cli": str(cli), "model": args.model, "passed": False}
    env = dict(os.environ)
    for key in ("DYLD_LIBRARY_PATH", "DYLD_FRAMEWORK_PATH", "DYLD_FALLBACK_LIBRARY_PATH", "LIBRARY_PATH"):
        env.pop(key, None)
    with tempfile.TemporaryDirectory(prefix="profile-api-", dir=Path.cwd()) as directory:
        root = Path(directory)
        workspace, state, models = root / "work", root / "state", root / "models"
        workspace.mkdir()
        source = catalog / args.model.removeprefix("model://")
        manifest = json.loads((source / "manifest.json").read_text())
        assert manifest["id"] == args.model.removeprefix("model://")
        assert manifest["files"][0]["sha256"] == "d98cdcbd03e17ce47681435b5150e34c1417f50b5c0019dd560e4882c5745785"
        assert manifest["files"][0]["size"] == 5027783488
        package = models / manifest["id"]
        for entry in manifest["files"]:
            target = package / entry["path"]
            target.parent.mkdir(parents=True, exist_ok=True)
            os.link(source / entry["path"], target)
        (package / "manifest.json").write_text(json.dumps(manifest))

        def private(name, value):
            path = root / name
            path.write_text(value)
            path.chmod(0o600)
            return path

        token, foreign = secrets.token_urlsafe(36), secrets.token_urlsafe(36)
        credentials = private("credentials", json.dumps({"society": token, "dreamscapes": foreign}))
        auth = private("auth", token)
        generation = private("generation", json.dumps({"temperature": 0, "max_tokens": 2048}))
        profiles = private("profiles", json.dumps({"project_boundary": str(workspace), "model_aliases": {"reader-model": args.model}}))
        config = private("model-config", json.dumps({"models": [{"model": args.model, "context_tokens": 8192,
            "options": {"enable_thinking": False, "tool_grammar": False}}]}))
        profile = workspace / ".claude/agents/reader.md"
        profile.parent.mkdir(parents=True)
        original = "---\nname: reader\ndescription: Inspect the requested file\nmodel: reader-model\ntools: Read\nskills: [inspect]\n---\nUse Read to inspect the file before returning its exact contents. Never guess.\n"
        profile.write_text(original)
        skill = workspace / ".claude/skills/inspect/SKILL.md"
        skill.parent.mkdir(parents=True)
        skill.write_text("---\ndescription: Verify evidence\n---\nUse the requested file as evidence. This child session is ${CLAUDE_SESSION_ID}.\n")
        endpoint = root / "s"
        command = [str(daemon), "--socket", str(endpoint), "--http-port", "0", "--models-root", str(models), "--config", str(config),
            "--agent-workspace", str(workspace), "--agent-state", str(state), "--agent-credentials", str(credentials),
            "--agent-profiles", str(profiles), "--agent-subagent-options", str(generation), "--agent-allow", "Agent",
            "--agent-no-apps", "--agent-no-tasks", "--agent-no-background", "--agent-no-auto-compact"]
        if args.skill_permissions:
            command += ["--agent-allow", "Skill(writer)"]
        started = time.monotonic()
        with (root / "daemon.log").open("w+") as log:
            process = subprocess.Popen(command, stdout=log, stderr=log, env=env)
            try:
                while True:
                    output = (root / "daemon.log").read_text(errors="replace")
                    match = re.search(r"iiLocalLLM HTTP: http://127\.0\.0\.1:(\d+)", output)
                    if match and endpoint.exists():
                        port = int(match[1])
                        break
                    assert process.poll() is None and time.monotonic() - started < 90, output[-3000:]
                    time.sleep(0.05)

                def rpc(method, params, bearer=token, expected=200):
                    conn = HTTPConnection("127.0.0.1", port, timeout=180)
                    try:
                        conn.request("POST", "/v1/rpc", json.dumps({"id": secrets.token_hex(8), "method": method, "params": params}),
                            {"Content-Type": "application/json", "Authorization": "Bearer " + bearer})
                        response = conn.getresponse()
                        result = json.loads(response.read())
                        assert response.status == expected, result
                        return result.get("result", result)
                    finally:
                        conn.close()

                parent = rpc("agent.sessions.create", {"model": args.model})["session_id"]
                listed = rpc("agent.agents.profiles", {"session_id": parent})
                reader = next(p for p in listed["result"]["profiles"] if p["name"] == "reader")
                assert reader["source"] == "project" and "system_prompt" not in reader
                rpc("agent.agents.profiles", {"session_id": parent}, bearer=foreign, expected=404)
                first, second = "FIRST_" + secrets.token_hex(8), "SECOND_" + secrets.token_hex(8)
                (workspace / "evidence.txt").write_text(first)
                prompt = "Use Read to read evidence.txt now and return the exact current file contents."
                outcome = rpc("agent.agents.run", {"session_id": parent, "prompt": prompt, "subagent_type": "reader"})
                report["first"] = outcome
                child = outcome["result"]
                assert not outcome["is_error"] and child["status"] == "completed" and first in child["result"]["text"], outcome
                profile.write_text(original.replace("Inspect the requested file", "Changed catalog description").replace("Never guess.", "NEW_BODY_MUST_NOT_REPLACE_RESUMED_SYSTEM."))
                refreshed = rpc("agent.agents.profiles", {"session_id": parent})
                assert next(p for p in refreshed["result"]["profiles"] if p["name"] == "reader")["sha256"] != reader["sha256"]
                (workspace / "evidence.txt").write_text(second)
                parameters = private("request", json.dumps({"resume": child["agentId"], "prompt": prompt, "run_in_background": True}))
                invocation = subprocess.run([str(cli), "--socket", str(endpoint), "--auth-file", str(auth), "agent", "agents", "run", parent, str(parameters)],
                    env=env, capture_output=True, text=True, timeout=30)
                assert invocation.returncode == 0, invocation.stderr
                accepted = json.loads(invocation.stdout)
                assert accepted["result"]["status"] == "async_launched", accepted
                resumed = rpc("agent.agents.output", {"session_id": parent, "agent_id": child["agentId"], "block": True, "timeout_ms": 60000})
                report["resumed"] = resumed
                assert not resumed["is_error"] and resumed["result"]["status"] == "completed" and second in resumed["result"]["result"]["text"], resumed
                transcript = state / hashlib.sha256(b"society").hexdigest() / "subagents/sessions" / child["session_id"] / "transcript.jsonl"
                rows = [json.loads(line) for line in transcript.read_text().splitlines()]
                assert "NEW_BODY_MUST_NOT_REPLACE_RESUMED_SYSTEM" not in rows[0]["system_prompt"]
                messages = [r["message"] for r in rows if r["type"] == "message"]
                preloads = [m for m in messages if "iilocal.skill" in m.get("metadata", {})]
                assert len(preloads) == 1 and child["session_id"] in preloads[0]["text"]
                calls = [c for m in messages for c in m["tool_calls"]]
                reads = {c["id"] for c in calls if c["name"] == "Read"}
                assert len(reads) >= 2 and all(any(m["role"] == "tool" and m["tool_call_id"] in reads and value in m["text"] for m in messages) for value in (first, second))
                assert {c["id"] for c in calls} == {m["tool_call_id"] for m in messages if m["role"] == "tool"}
                record = transcript.parent.parent.parent / (child["agentId"] + ".json")
                saved = json.loads(record.read_text())
                assert saved["profile"]["sha256"] == reader["sha256"] and saved["model"] == args.model
                if args.skill_forks:
                    profile.write_text(original)
                    fork_path = workspace / ".claude/skills/fork-inspect/SKILL.md"
                    fork_path.parent.mkdir(parents=True)
                    fork_path.write_text("---\ndescription: Read evidence using a separate reader agent\ncontext: fork\nagent: reader\nmodel: reader-model\n---\n"
                        "PRIVATE_FORK_INSTRUCTIONS parent=${CLAUDE_SESSION_ID}. Use Read to read $0 now. Return its exact current contents. Never guess.\n")
                    fork_results = []
                    report["skill_forks"] = fork_results
                    for route in ("http", "cli", "model-tool"):
                        owner = rpc("agent.sessions.create", {"model": args.model, "system": "Use the specifically requested tool to complete the task."})["session_id"]
                        secret = "FORK_" + secrets.token_hex(8)
                        (workspace / "fork.txt").write_text(secret)
                        params = {"session_id": owner, "skill": "fork-inspect", "skill_arguments": "fork.txt", "max_turns": 6,
                            "options": {"temperature": 0, "max_tokens": 2048}}
                        if route == "cli":
                            command_file = private("fork-request", json.dumps({k: v for k, v in params.items() if k != "session_id"}))
                            proc = subprocess.run([str(cli), "--socket", str(endpoint), "--auth-file", str(auth), "agent", "skills", "run", owner, str(command_file)],
                                env=env, capture_output=True, text=True, timeout=180)
                            assert proc.returncode == 0, proc.stderr
                            run = json.loads(proc.stdout)
                        else:
                            if route == "model-tool":
                                params.pop("skill"); params.pop("skill_arguments")
                                params["prompt"] = "Call the Skill tool with skill=\"fork-inspect\" and args=\"fork.txt\". Return the child result."
                            run = rpc("agent.run", params)
                        report["active_fork"] = {"route": route, "run": run}
                        assert run["status"] == "completed" and secret in run["text"] and run["session_id"] == owner, run
                        parent_state = rpc("agent.sessions.get", {"session_id": owner})
                        parent_messages = parent_state["messages"]
                        assert not any("PRIVATE_FORK_INSTRUCTIONS" in m["text"] for m in parent_messages)
                        parent_calls = [c for m in parent_messages for c in m["tool_calls"]]
                        assert [c["name"] for c in parent_calls] == (["Skill"] if route == "model-tool" else []), parent_calls
                        jobs = rpc("agent.agents.list", {"session_id": owner})["result"]["agents"]
                        assert len(jobs) == 1 and jobs[0]["finished"], jobs
                        child_id = jobs[0]["agentId"]
                        child_state = rpc("agent.agents.output", {"session_id": owner, "agent_id": child_id})["result"]
                        assert child_state["status"] == "completed" and child_state["model"] == args.model, child_state
                        private_record = state / hashlib.sha256(b"society").hexdigest() / "subagents" / (child_id + ".json")
                        record = json.loads(private_record.read_text())
                        assert not record["background"] and not record["fork_context"] and "notification_id" not in record
                        assert record["skill"]["sha256"] == hashlib.sha256(fork_path.read_bytes()).hexdigest()
                        child_path = private_record.parent / "sessions" / jobs[0]["session_id"] / "transcript.jsonl"
                        child_messages = [r["message"] for r in map(json.loads, child_path.read_text().splitlines()) if r["type"] == "message"]
                        loaded = [m for m in child_messages if m.get("metadata", {}).get("iilocal.skill", {}).get("context") == "fork"]
                        assert len(loaded) == 1 and "parent=" + owner in loaded[0]["text"]
                        fork_reads = [c for m in child_messages for c in m["tool_calls"] if c["name"] == "Read"]
                        assert fork_reads and any(m["role"] == "tool" and secret in m["text"] for m in child_messages)
                        rpc("agent.run", {"session_id": owner, "skill": "fork-inspect"}, bearer=foreign, expected=404)
                        fork_results.append({"route": route, "run": run, "child": child_state, "actual_reads": len(fork_reads), "isolated": True})
                    report.pop("active_fork", None)
                if args.skill_permissions:
                    writer_profile = workspace / ".claude/agents/writer.md"
                    writer_profile.write_text("---\nname: writer\ndescription: Write the requested file\ntools: Write, Skill\n---\nUse Write exactly as requested. Return DONE after the tool succeeds.\n")
                    writer = workspace / ".claude/skills/writer/SKILL.md"
                    writer.parent.mkdir(parents=True)
                    results = report["skill_permissions"] = []
                    for mode in ("inline", "fork"):
                        writer.write_text("---\ndescription: Write an exact value to the specified file\nallowed-tools: 'Write(grant-*.txt)'\n"
                            + ("context: fork\nagent: writer\n" if mode == "fork" else "")
                            + "---\nUse Write with path=$0 and content=$1 exactly, without adding a newline. Then return DONE.\n")
                        for route in ("http", "cli", "model-tool"):
                            owner = rpc("agent.sessions.create", {"model": args.model, "system": "Use exactly the requested tool. If it fails, report the failure and stop. Never bypass a tool denial."})["session_id"]
                            value = "GRANTED_" + secrets.token_hex(8)
                            filename = f"grant-{mode}-{route}.txt"
                            params = {"session_id": owner, "skill": "writer", "skill_arguments": filename + " " + value,
                                "max_turns": 4, "options": {"temperature": 0, "max_tokens": 1024}}
                            if route == "cli":
                                request_file = private("permission-request", json.dumps({k: v for k, v in params.items() if k != "session_id"}))
                                proc = subprocess.run([str(cli), "--socket", str(endpoint), "--auth-file", str(auth), "agent", "skills", "run", owner, str(request_file)],
                                    env=env, capture_output=True, text=True, timeout=180)
                                assert proc.returncode == 0, proc.stderr
                                run = json.loads(proc.stdout)
                            else:
                                if route == "model-tool":
                                    params.pop("skill"); literal = params.pop("skill_arguments")
                                    params["prompt"] = 'Call Skill with skill="writer" and args="' + literal + '". Follow the loaded instructions or return the child result.'
                                run = rpc("agent.run", params)
                            report["active_permission"] = {"mode": mode, "route": route, "run": run}
                            assert run["status"] == "completed" and (workspace / filename).read_text() == value, report["active_permission"]
                            parent_state = rpc("agent.sessions.get", {"session_id": owner})
                            records = parent_state["messages"]
                            if mode == "fork":
                                job = rpc("agent.agents.list", {"session_id": owner})["result"]["agents"][0]
                                path = state / hashlib.sha256(b"society").hexdigest() / "subagents/sessions" / job["session_id"] / "transcript.jsonl"
                                records = [r["message"] for r in map(json.loads, path.read_text().splitlines()) if r["type"] == "message"]
                            actual = [c for m in records for c in m["tool_calls"] if c["name"] == "Write"]
                            assert len(actual) == 1 and actual[0]["arguments"] == {"path": filename, "content": value}, actual
                            assert any(m.get("metadata", {}).get("iilocal.skill", {}).get("allowed_tools") == ["Write(grant-*.txt)"] for m in records)
                            before = len(parent_state["messages"])
                            denied_file = f"grant-after-{mode}-{route}.txt"
                            denied = rpc("agent.run", {"session_id": owner, "prompt": f'Call Write directly with path="{denied_file}" and content="DENIED". Do not call Skill or any other tool. If Write fails, stop and report the failure.',
                                "max_turns": 3, "options": {"temperature": 0, "max_tokens": 1024}})
                            negative = rpc("agent.sessions.get", {"session_id": owner})["messages"][before:]
                            calls = [c for m in negative for c in m["tool_calls"]]
                            assert calls and all(c["name"] == "Write" for c in calls), negative
                            assert any(m["is_error"] and "permission denied" in m["text"] for m in negative if m["role"] == "tool"), negative
                            assert not (workspace / denied_file).exists(), "Skill grant leaked into the next run"
                            rpc("agent.run", {"session_id": owner, "prompt": "invalid", "allowed_tools": ["Write"]}, expected=400)
                            rpc("agent.skills.list", {"session_id": owner}, bearer=foreign, expected=404)
                            results.append({"mode": mode, "route": route, "run": run, "exact_write": actual[0], "next_run": denied,
                                "next_run_denied_write_calls": len(calls), "grant_expired": True, "wire_grants_rejected": True})
                    report.pop("active_permission", None)
                report.update(passed=True, profile=reader, skill_preloaded_once=True, model_alias_resolved=True,
                    source_hash_frozen_on_resume=True, cross_app_isolation=True, observed_reads=len(reads), seconds=round(time.monotonic()-started, 3))
            finally:
                process.terminate()
                try:
                    process.wait(timeout=15)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=5)
                report["daemon_exit"] = process.returncode
                if process.returncode != 0:
                    report["passed"] = False
                if args.report:
                    args.report.parent.mkdir(parents=True, exist_ok=True)
                    args.report.write_text(json.dumps(report, indent=2) + "\n")
                    args.report.with_suffix(".daemon.log").write_text((root / "daemon.log").read_text(errors="replace"))
        assert report["passed"], report
        print(json.dumps(report))


if __name__ == "__main__":
    main()
