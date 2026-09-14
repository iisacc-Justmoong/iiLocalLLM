"""HTTP CLI configuration and private-credential boundaries, using stdlib HTTP."""
import json
import http.client
import os
from pathlib import Path
import secrets
import select
import subprocess
import sys
import tempfile
from urllib.parse import urlsplit

binary = str(Path(sys.argv[1]).resolve())
with tempfile.TemporaryDirectory(prefix="mcp-http-cli-") as directory:
    root = Path(directory)
    workspace = root / "workspace"
    workspace.mkdir()
    credentials = root / "credentials.json"
    token = secrets.token_urlsafe(32)
    arguments = [binary, "--workspace", str(workspace), "--http-port", "0", "--credentials", str(credentials), "--state", str(root / "state")]

    def reject(extra=(), override=None):
        process = subprocess.run(override or (arguments + list(extra)), capture_output=True, timeout=10)
        assert process.returncode != 0 and not process.stdout, (process.returncode, process.stdout, process.stderr)
        assert token.encode() not in process.stderr, "Credential leaked to diagnostics"

    def write(value, mode=0o600):
        credentials.write_text(json.dumps(value))
        credentials.chmod(mode)

    for value in ({}, {"fixture": "short"}, {"fixture\n": token}, {"fixture": token + "\n"},
                  {"a": token, "b": token}, {"fixture": 12}, []):
        write(value)
        reject(["--model", "model://not-installed", "--models", str(root / "missing-models")])
        assert not (root / "missing-models").exists(), "Invalid credentials reached Service initialization"
    write({"fixture": token}, 0o644)
    reject()
    write({"fixture": token})
    reject(["--apps-dir", str(root / "apps"), "--no-apps"])
    reject(["--apps-dir", ""])
    inside = workspace / "credentials.json"
    inside.write_bytes(credentials.read_bytes()); inside.chmod(0o600)
    alias = root / "alias.json"
    alias.symlink_to(credentials)
    for path in (inside, alias):
        changed = list(arguments)
        changed[changed.index("--credentials") + 1] = str(path)
        reject(override=changed)
    reject(["--sessions", str(root / "sessions")])
    reject(["--artifacts", str(root / "artifacts")])
    changed = list(arguments)
    changed[changed.index("--state") + 1] = str(workspace / "state")
    reject(override=changed)
    for remove in ("--credentials", "--state", "--http-port"):
        changed = list(arguments)
        index = changed.index(remove)
        del changed[index:index + 2]
        reject(override=changed)
    second_token = secrets.token_urlsafe(32)
    write({"fixture": token, "second-app": second_token})
    process = subprocess.Popen(arguments, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        assert select.select([process.stdout], [], [], 10)[0], "HTTP CLI startup timed out"
        endpoint = json.loads(process.stdout.readline())["endpoint"]
        assert endpoint.startswith("http://127.0.0.1:") and endpoint.endswith("/mcp")
        assert os.stat(root / "state").st_mode & 0o077 == 0
        reject()  # A second process cannot own the same state.
        url = urlsplit(endpoint)
        def post(message, key, session=None):
            headers = {"Accept": "application/json, text/event-stream", "Content-Type": "application/json"}
            if key:
                headers["Authorization"] = "Bearer " + key
            if session:
                headers.update({"Mcp-Session-Id": session, "Mcp-Protocol-Version": "2025-11-25"})
            connection = http.client.HTTPConnection(url.hostname, url.port, timeout=5)
            try:
                connection.request("POST", url.path, json.dumps(message), headers)
                response = connection.getresponse()
                return response.status, response.getheader("Mcp-Session-Id"), response.read()
            finally:
                connection.close()
        initialize = {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {
            "protocolVersion": "2025-11-25", "capabilities": {}, "clientInfo": {"name": "second-app", "version": "1"}}}
        assert post(initialize, None)[0] == 401
        assert post(initialize, "invalid")[0] == 401
        first_status, first_session, _ = post(initialize, token)
        second_status, second_session, _ = post(initialize, second_token)
        assert first_status == second_status == 200 and first_session != second_session
        initialized = {"jsonrpc": "2.0", "method": "notifications/initialized"}
        assert post(initialized, second_token, first_session)[0] == 404
        assert post(initialized, token, first_session)[0] == 202
        assert post(initialized, second_token, second_session)[0] == 202
    finally:
        process.terminate()
        out, err = process.communicate(timeout=10)
        assert process.returncode == 0 and token.encode() not in out + err
print("MCP HTTP CLI private credentials, early validation, disjoint state, ownership and graceful shutdown passed")
