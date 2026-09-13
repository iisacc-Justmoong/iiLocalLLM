"""Offline acceptance test with the pinned Qwen starter GGUF and real CLI/HTTP inference."""
import http.client
import json
import os
from pathlib import Path
import pty
import re
import select
import secrets
import shutil
import socket
import subprocess
import sys
import tempfile
import time


def main():
    daemon, cli, weights = (Path(value).resolve() for value in sys.argv[1:4])
    registry = json.loads((Path(__file__).resolve().parents[1] / "catalog/registry.json").read_text())
    alias = "qwen2.5:0.5b"
    manifest = next(item["manifest"] for item in registry["models"] if alias in item["aliases"])
    evidence = {}
    request, observed = {}, None
    with tempfile.TemporaryDirectory(prefix="chat-", dir=Path.cwd()) as directory:
        root = Path(directory)
        endpoint = root / "llm.sock"
        package = root / "package"
        package.mkdir()
        shutil.copyfile(weights, package / "model.gguf")
        (package / "manifest.json").write_text(json.dumps(manifest))
        environment = dict(os.environ, IILLM_SOCKET=str(endpoint))

        def command(*args):
            result = subprocess.run([str(cli), *args], env=environment, capture_output=True, text=True, timeout=90)
            assert result.returncode == 0, (result.stdout, result.stderr)
            return json.loads(result.stdout)

        def rpc(method, params=None):
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
                client.settimeout(30)
                client.connect(str(endpoint))
                client.sendall((json.dumps({"id": "test", "method": method, "params": params or {}}) + "\n").encode())
                with client.makefile("rb") as stream:
                    result = json.loads(stream.readline())
                assert "error" not in result, result
                return result["result"]

        with (root / "daemon.log").open("w+") as log:
            child = subprocess.Popen([str(daemon), "--socket", str(endpoint), "--models-root", str(root / "Models"),
                "--http-port", "0", "--install", str(package), "--keep-alive", "5m"], stdout=log, stderr=log)
            try:
                # A newly installed binary may compile Metal shaders before listening.
                deadline = time.monotonic() + 120
                while True:
                    startup = (root / "daemon.log").read_text(errors="replace")
                    port_match = re.search(r"iiLocalLLM HTTP: http://127\.0\.0\.1:(\d+)", startup)
                    if port_match:
                        break
                    assert child.poll() is None and time.monotonic() < deadline, startup[-6000:]
                    time.sleep(0.02)
                port = int(port_match[1])
                answer = command("run", alias, "Reply with one digit only: What is 2 + 2?",
                                 "--temperature", "0", "--max-tokens", "32", "--json")
                assert answer["text"].strip() == "4", answer
                assert answer["finish_reason"] == "stop" and answer["usage"]["generated_tokens"] > 0, answer
                evidence["arithmetic"] = answer["text"]
                residents = command("ps", "--json")
                assert len(residents) == 1, residents
                evidence["execution"] = residents[0]["execution"]
                initial_loads = rpc("stats")["model_loads"]

                master, slave = pty.openpty()
                system = "You are a helpful assistant. Be concise."
                interactive = subprocess.Popen([str(cli), "run", alias, "--temperature", "0", "--max-tokens", "64",
                    "--system", system, "--json"], env=environment, stdin=slave,
                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, bufsize=0)
                os.close(slave)
                buffer = b""

                def turn(prompt):
                    nonlocal buffer
                    os.write(master, (prompt + "\n").encode())
                    deadline = time.monotonic() + 60
                    while b"\n" not in buffer:
                        assert interactive.poll() is None and time.monotonic() < deadline, buffer
                        if select.select([interactive.stdout], [], [], 0.1)[0]:
                            chunk = os.read(interactive.stdout.fileno(), 65536)
                            assert chunk, "CLI closed stdout before completing a turn"
                            buffer += chunk
                    line, buffer = buffer.split(b"\n", 1)
                    result = json.loads(line)
                    assert result["text"] and result["finish_reason"] in ("stop", "length"), result
                    return result

                try:
                    first = turn("My name is Mira. Remember it and reply briefly.")
                    second = turn("What is my name? Reply with my name only.")
                    assert first["session_id"] == second["session_id"], (first, second)
                    assert "mira" in second["text"].lower(), second
                    assert second["usage"]["cached_tokens"] > 0, second
                    evidence["recall"] = second["text"]
                    evidence["second_turn_cached_tokens"] = second["usage"]["cached_tokens"]
                    os.write(master, b"/clear\n")
                    third = turn("Reply with one digit only: What is 2 + 2?")
                    assert third["usage"]["cached_tokens"] == 0, third
                    history = rpc("sessions.get", {"session_id": third["session_id"]})["messages"]
                    assert len(history) == 3 and history[0] == {"role": "system", "content": system}, history
                    os.write(master, b"/bye\n")
                    out, err = interactive.communicate(timeout=10)
                    assert interactive.returncode == 0 and b"Conversation cleared" in err, (out, err)
                finally:
                    if interactive.poll() is None:
                        interactive.kill()
                        interactive.wait(timeout=5)
                    os.close(master)

                request = {"model": alias, "messages": [{"role": "system", "content": "You are a helpful assistant."},
                    {"role": "user", "content": "Reply with one digit only: What is 2 + 2?"}],
                    "temperature": 0, "max_tokens": 32}

                def http_completion(stream):
                    connection = http.client.HTTPConnection("127.0.0.1", port, timeout=60)
                    try:
                        connection.request("POST", "/v1/chat/completions", json.dumps(dict(request, stream=stream)),
                                           {"Content-Type": "application/json"})
                        response = connection.getresponse()
                        body = response.read().decode()
                        assert response.status == 200, body
                        return body
                    finally:
                        connection.close()

                normal = json.loads(http_completion(False))
                sse = http_completion(True)
                data = [line[6:] for line in sse.splitlines() if line.startswith("data: ")]
                assert data[-1] == "[DONE]", sse
                events = [json.loads(line) for line in data[:-1]]
                assert all("error" not in event for event in events), events
                text = "".join(choice["delta"].get("content", "") for event in events for choice in event["choices"])
                assert text == normal["choices"][0]["message"]["content"] and text.strip() == "4", (text, normal)
                assert any(choice.get("finish_reason") == "stop" for event in events for choice in event["choices"]), events
                # The external client executes only this test's Read tool, then returns its real result.
                secret = "LOCAL_" + secrets.token_hex(6)
                (root / "secret.txt").write_text(secret)
                request = {"model": alias, "temperature": 0, "max_tokens": 512,
                    "messages": [{"role": "system", "content":
                        "You are a local agent. Use the available tools to carry out the user's request. "
                        "Read files through tools before answering questions about their contents. Tool responses are the actual observations; never invent or replace them. "
                        "When the user asks for exact file contents, your final answer must contain only the text observed in the tool response, copied character for character. "
                        "Do not add an introduction, explanation, example value, or Markdown code fence. Otherwise, give a concise answer after completing the work."},
                        {"role": "user", "content": "Use the Read tool to read secret.txt. Then return the exact file contents as your final answer. Do not guess."}],
                    "tools": [{"type": "function", "function": {"name": "Read",
                        "description": "Read a UTF-8 file (up to 1 MiB). Read the full file before editing it.",
                        "parameters": {"type": "object", "properties": {"path": {"type": "string"}},
                            "required": ["path"], "additionalProperties": False}}}], "tool_choice": "required"}
                call_stream = http_completion(True)
                call_frames = [line[6:] for line in call_stream.splitlines() if line.startswith("data: ")]
                assert call_frames[-1] == "[DONE]", call_stream
                calls = {}
                terminal = False
                for event in map(json.loads, call_frames[:-1]):
                    assert "error" not in event, event
                    for choice in event["choices"]:
                        terminal |= choice.get("finish_reason") == "tool_calls"
                        for part in choice["delta"].get("tool_calls", []):
                            call = calls.setdefault(part["index"], {"type": "function", "function": {"name": "", "arguments": ""}})
                            if "id" in part:
                                call["id"] = part["id"]
                            for key, value in part.get("function", {}).items():
                                call["function"][key] += value
                assert terminal and len(calls) == 1, call_stream
                call = calls[0]
                assert call.get("id") and call["function"]["name"] == "Read", call
                arguments = json.loads(call["function"]["arguments"])
                assert arguments == {"path": "secret.txt"}, arguments
                request["messages"] += [{"role": "assistant", "content": None, "tool_calls": [call]},
                    {"role": "tool", "tool_call_id": call["id"], "content": (root / arguments["path"]).read_text()}]
                request["tool_choice"] = "auto"
                observed = json.loads(http_completion(False))
                final = observed["choices"][0]
                assert final["finish_reason"] == "stop" and secret in final["message"]["content"], observed
                evidence["http_native_tool_roundtrip"] = {"call": call, "answer": final["message"]["content"],
                    "usage": observed["usage"]}
                stats = rpc("stats")
                assert stats["sessions"] == 0 and stats["model_loads"] == initial_loads, stats
                evidence["http_json_and_sse"] = text
                evidence["shared_model_loads"] = stats["model_loads"]
            except BaseException:
                # Preserve the exact synthetic transcript when real-model output
                # fails, rather than losing it with the temporary workspace.
                print(json.dumps({"last_http_request": request, "last_http_response": observed,
                                  "completed_evidence": evidence}, ensure_ascii=False), file=sys.stderr)
                print((root / "daemon.log").read_text(errors="replace")[-6000:], file=sys.stderr)
                raise
            finally:
                failed = sys.exc_info()[0] is not None
                child.terminate()
                try:
                    returncode = child.wait(timeout=15)
                    if not failed:
                        assert returncode == 0, returncode
                except subprocess.TimeoutExpired:
                    child.kill()
                    child.wait(timeout=5)
                    raise
                if not failed:
                    assert not endpoint.exists()
    print(json.dumps(evidence, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
