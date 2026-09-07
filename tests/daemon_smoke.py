"""Standalone installation, persistent URI catalog, Unix IPC and real GGUF inference."""
import json
import http.client
from pathlib import Path
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time


def main():
    executable, weights = map(str, map(Path, sys.argv[1:3]))
    with tempfile.TemporaryDirectory(prefix="ipc-", dir=Path.cwd()) as directory:
        root = Path(directory)
        endpoint, storage, package = root / "llm.sock", root / "Models", root / "source"
        package.mkdir()
        shutil.copyfile(weights, package / "model.gguf")
        (package / "manifest.json").write_text(json.dumps({"id": "test", "architecture": "llama", "format": "gguf",
            "quantization": "Q4_0", "context_length": 512, "capabilities": ["text-generation", "chat"]}))
        install = subprocess.run([executable, "--models-root", str(storage), "--install", str(package)],
                                 capture_output=True, text=True, timeout=40)
        assert install.returncode == 0, install.stderr[-4000:]
        assert json.loads(install.stdout)["model"] == "model://test"
        package.rename(root / "retired-source")  # Inference cannot depend on the import location.
        config = root / "models.json"
        config.write_text(json.dumps({"models": [{"model": "model://test", "context_tokens": 512,
                                                 "options": {"chat_template": "chatml"}}]}))
        process = None
        http_port = 0
        with (root / "daemon.log").open("w+") as log:
            def launch(http_only=False):
                nonlocal http_port
                start_offset = (root / "daemon.log").stat().st_size
                arguments = [executable, "--models-root", str(storage), "--http-port", "0"]
                if not http_only:
                    arguments += ["--config", str(config), "--socket", str(endpoint)]
                child = subprocess.Popen(arguments, stdout=log, stderr=log)
                deadline = time.monotonic() + 40
                while True:
                    output = (root / "daemon.log").read_bytes()[start_offset:].decode(errors="replace")
                    match = re.search(r"iiLocalLLM HTTP: http://127\.0\.0\.1:(\d+)", output)
                    if match and (http_only or endpoint.exists()):
                        http_port = int(match[1])
                        break
                    if child.poll() is not None or time.monotonic() >= deadline:
                        if child.poll() is None:
                            child.kill()
                            child.wait(timeout=5)
                        raise AssertionError("Daemon startup failed: " + output[-4000:])
                    time.sleep(0.02)
                return child

            def stop(child):
                child.terminate()
                assert child.wait(timeout=10) == 0
                assert not endpoint.exists(), "Socket not cleaned up"
                assert not Path(str(endpoint) + ".lock").exists(), "Endpoint lock not cleaned up"
                assert not (storage / ".iilocal-llm.lock").exists(), "Catalog ownership not released"
                try:
                    with socket.create_connection(("127.0.0.1", http_port), timeout=1):
                        raise AssertionError("HTTP listener not closed")
                except OSError:
                    pass

            def http_request(method, path, body=None):
                connection = http.client.HTTPConnection("127.0.0.1", http_port, timeout=20)
                try:
                    connection.request(method, path, body=json.dumps(body) if body else None,
                                       headers={"Content-Type": "application/json"})
                    response = connection.getresponse()
                    data = response.read()
                    assert response.status == 200, (response.status, data)
                    return response.getheader("Content-Type"), data
                finally:
                    connection.close()

            try:
                for run in range(2):
                    process = launch()
                    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
                        client.settimeout(20)
                        client.connect(str(endpoint))
                        with client.makefile("rwb") as stream:
                            def send(identifier, method, params):
                                stream.write((json.dumps({"id": identifier, "method": method, "params": params}) + "\n").encode())
                                stream.flush()

                            def receive():
                                line = stream.readline()
                                assert line, "Disconnected before final response"
                                return json.loads(line)

                            def call(method, params=None):
                                send(method, method, params or {})
                                response = receive()
                                assert response["id"] == method
                                return response

                            hardware = call("hardware.get")["result"]
                            assert hardware["cpu_architecture"] and hardware["ram_bytes"] > 0
                            listing = call("models.list")["result"]
                            assert not listing["issues"] and len(listing["models"]) == 1
                            assert listing["models"][0]["model"] == "model://test"
                            assert listing["models"][0]["loaded"] is True
                            resolved = call("models.resolve", {"model": "model://test"})["result"]
                            assert resolved["manifest"]["id"] == "test" and "path" not in resolved
                            verification = call("models.verify", {"model": "model://test"})["result"]
                            assert verification["valid"] and verification["checked_files"] == 1
                            loaded = call("models.loaded")["result"][0]
                            assert loaded["model"] == "model://test" and loaded["execution"]["runtime"] == "llama.cpp"
                            if hardware["apple_silicon"] and hardware["metal_available"]:
                                assert loaded["execution"]["backend"] == "metal", loaded
                            assert call("models.load", {"model": "model://test", "path": weights})["error"]["code"] == "invalid_argument"
                            assert call("models.remove", {"model": "model://test"})["error"]["code"] == "model_in_use"
                            session = call("sessions.create", {"model": "model://test"})["result"]["session_id"]
                            assert call("models.unload", {"model": "model://test"})["error"]["code"] == "model_in_use"
                            send("chat", "chat", {"session_id": session, "prompt": "Tell a story about a bird.",
                                                   "options": {"max_tokens": 12, "temperature": 0}})
                            events = []
                            while True:
                                event = receive()
                                events.append(event)
                                if event.get("event") == "done":
                                    break
                            assert events[0]["event"] == "accepted" and events[1]["event"] == "started"
                            text = "".join(e["text"] for e in events if e.get("event") == "delta")
                            assert text and text == events[-1]["result"]["text"]
                            assert events[-1]["result"]["finish_reason"] in ("length", "stop")
                            history = call("sessions.get", {"session_id": session})["result"]
                            assert history["model"] == "model://test" and len(history["messages"]) == 2
                            assert json.loads(http_request("GET", "/health")[1])["status"] == "ok"
                            assert json.loads(http_request("GET", "/v1/models")[1])["data"][0]["id"] == "model://test"
                            completion = {"model": "model://test", "messages": [{"role": "user", "content": "Tell a story about a bird."}],
                                          "max_tokens": 12, "temperature": 0}
                            mime, data = http_request("POST", "/v1/chat/completions", completion)
                            answer = json.loads(data)
                            assert mime.startswith("application/json") and answer["object"] == "chat.completion"
                            assert answer["choices"][0]["message"]["content"] and answer["usage"]["completion_tokens"] > 0
                            completion["messages"] += [{"role": "assistant", "content": answer["choices"][0]["message"]["content"]},
                                                       {"role": "user", "content": "Continue."}]
                            completion.update(stream=True, stream_options={"include_usage": True})
                            mime, data = http_request("POST", "/v1/chat/completions", completion)
                            assert mime.startswith("text/event-stream") and data.endswith(b"data: [DONE]\n\n")
                            chunks = [json.loads(line[6:]) for line in data.splitlines() if line.startswith(b"data: ") and line != b"data: [DONE]"]
                            assert chunks[0]["choices"][0]["delta"]["role"] == "assistant"
                            assert chunks[-1]["usage"]["completion_tokens"] > 0
                            assert not any("error" in chunk for chunk in chunks)
                            assert call("stats")["result"]["sessions"] == 1  # Only the native session remains.
                            assert call("sessions.get", {"session_id": session})["result"] == history
                            assert "result" in call("sessions.close", {"session_id": session})
                            assert "result" in call("models.unload", {"model": "model://test"})
                            assert call("models.list")["result"]["models"][0]["loaded"] is False
                            assert not json.loads(http_request("GET", "/v1/models")[1])["data"]
                            if run == 1:
                                assert "result" in call("models.remove", {"model": "model://test"})
                                assert not call("models.list")["result"]["models"]
                                imported = call("models.install", {"package_directory": str(root / "retired-source")})["result"]
                                assert imported["model"] == "model://test" and imported["loaded"] is False
                                reloaded = call("models.load", {"model": imported["model"], "context_tokens": 512,
                                                                "options": {"chat_template": "chatml"}})["result"]
                                assert reloaded["loaded"] is True and reloaded["execution"]["runtime"] == "llama.cpp"
                                assert "result" in call("models.unload", {"model": imported["model"]})
                                assert "result" in call("models.remove", {"model": imported["model"]})
                                assert not call("models.list")["result"]["models"]
                            print(json.dumps({"daemon": "ok", "restart": run, "model": loaded["model"],
                                              "execution": loaded["execution"], "text": text, "http": "json+sse", "http_port": http_port}))
                    stop(process)
                    process = None
                process = launch(http_only=True)
                assert json.loads(http_request("GET", "/health")[1])["status"] == "ok"
                assert not json.loads(http_request("GET", "/v1/models")[1])["data"]
                stop(process)
                process = None
                assert (root / "retired-source" / "model.gguf").is_file()
                assert Path(weights).is_file()
            finally:
                if process is not None and process.poll() is None:
                    process.kill()
                    process.wait(timeout=5)


if __name__ == "__main__":
    main()
