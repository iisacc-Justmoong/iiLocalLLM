"""Qualify file profiles through the actual daemon HTTP API and thin IPC CLI."""
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
