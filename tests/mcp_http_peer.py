"""Independent MCP HTTP wire peer; no Qt or MCP SDK dependency."""
import json
import os
import queue
import socket
import sys
import threading
import time
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

mode = sys.argv[1] if len(sys.argv) > 1 else "normal"
lock = threading.RLock()
state = dict(initializations=0, headersValid=True, resumePosts=0, resumeGets=0,
             expirePosts=0, disconnectPosts=0, droppedPosts=0, cancelled=0, redirectTargetHits=0)
sessions = set()
initialized = set()
reverse = {}
waiting = {}
resumes = {}
notifications = queue.Queue()


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *_):
        pass

    def reply(self, status, body=None, headers=None):
        raw = b"" if body is None else json.dumps(body, ensure_ascii=False).encode()
        self.send_response(status)
        self.send_header("Content-Length", str(len(raw)))
        if body is not None:
            self.send_header("Content-Type", "application/json")
        for k, v in (headers or {}).items():
            self.send_header(k, v)
        self.end_headers()
        if raw:
            self.wfile.write(raw)

    def authorized(self):
        if self.headers.get("Authorization") != "Bearer test-credential":
            self.reply(401, headers={"WWW-Authenticate": 'Bearer realm="fixture"'})
            return False
        return True

    def valid_session(self):
        with lock:
            if self.headers.get("Mcp-Session-Id") not in sessions:
                self.reply(404)
                return False
            valid = self.headers.get("Mcp-Protocol-Version") == "2025-11-25"
            state["headersValid"] &= valid
        if not valid:
            self.reply(400)
        return valid

    def stream(self, headers=None):
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream; charset=utf-8")
        self.send_header("Connection", "close")
        for k, v in (headers or {}).items():
            self.send_header(k, v)
        self.end_headers()
        self.close_connection = True

    def event(self, message, identifier=None, fragmented=False):
        raw = (("id: " + identifier + "\r\n") if identifier else "")
        raw += "event: message\r\ndata: " + json.dumps(message, ensure_ascii=False) + "\r\n\r\n"
        encoded = raw.encode()
        if fragmented:
            for b in encoded:
                self.wfile.write(bytes([b]))
                self.wfile.flush()
        else:
            self.wfile.write(encoded)
            self.wfile.flush()

    def do_POST(self):
        try:
            self.post()
        except (BrokenPipeError, ConnectionResetError, TimeoutError):
            pass

    def post(self):
        data = self.rfile.read(int(self.headers.get("Content-Length", 0)))
        if not self.authorized():
            return
        if self.path == "/redirect-target":
            with lock:
                state["redirectTargetHits"] += 1
            self.reply(500)
            return
        message = json.loads(data)
        method = message.get("method")
        params = message.get("params", {})
        identifier = message.get("id")
        if os.environ.get("MCP_HTTP_TRACE"):
            print("POST", method, identifier, len(data), file=sys.stderr, flush=True)
        accept = self.headers.get("Accept", "")
        valid = "application/json" in accept and "text/event-stream" in accept
        with lock:
            state["headersValid"] &= valid
        if not valid:
            self.reply(406)
            return
        if method == "initialize":
            assert not self.headers.get("Mcp-Session-Id")
            sid = uuid.uuid4().hex
            with lock:
                sessions.add(sid)
                state["initializations"] += 1
            result = {"jsonrpc": "2.0", "id": identifier, "result": {
                "protocolVersion": "2025-11-25", "capabilities": {"tools": {}},
                "serverInfo": {"name": "independent-http-peer", "version": "1"}}}
            if mode == "resume-initialize":
                cursor = uuid.uuid4().hex
                with lock:
                    resumes[cursor] = result
                self.stream({"Mcp-Session-Id": sid})
                self.wfile.write(f"id: {cursor}\ndata:\nretry: 20\n\n".encode())
                self.wfile.flush()
            else:
                self.reply(200, result, {"Mcp-Session-Id": sid})
            return
        if not self.valid_session():
            return
        sid = self.headers["Mcp-Session-Id"]
        if method == "notifications/initialized":
            time.sleep(.03)  # Deliberately expose clients that race subsequent POSTs.
            initialized.add(sid)
            self.reply(202)
            return
        if sid not in initialized:
            self.reply(409)
            return
        if method is None:
            with lock:
                q = reverse.get(identifier)
            if q:
                q.put(message)
            self.reply(202)
            return
        if method.startswith("notifications/"):
            if method == "notifications/cancelled":
                with lock:
                    state["cancelled"] += 1
                    event = waiting.get(params["requestId"])
                if event:
                    event.set()
            self.reply(202)
            return
        response = lambda result: {"jsonrpc": "2.0", "id": identifier, "result": result}
        if method == "tools/list":
            self.reply(200, response({"tools": [{"name": "echo", "inputSchema": {
                "type": "object", "properties": {"value": {"type": "string"}}, "required": ["value"]},
                "outputSchema": {"type": "object", "properties": {"value": {"type": "string"}}}}]}))
        elif method == "tools/call":
            value = params["arguments"]["value"]
            self.reply(200, response({"content": [{"type": "text", "text": value}], "structuredContent": {"value": value}}))
        elif method == "test/state":
            with lock:
                self.reply(200, response(dict(state)))
        elif method == "test/progress":
            self.stream()
            self.wfile.write(b"\xef\xbb\xbf: comment\r\n\r\n")
            for progress in [1, 2]:
                self.event({"jsonrpc": "2.0", "method": "notifications/progress", "params": {
                    "progressToken": params["_meta"]["progressToken"], "progress": progress}}, fragmented=True)
            self.event(response({"text": "분할된 UTF-8"}), fragmented=True)
        elif method == "test/reverse":
            self.stream()
            answers = []
            for request_method in ["sampling/createMessage", "roots/list"]:
                rid = uuid.uuid4().hex
                q = queue.Queue()
                with lock:
                    reverse[rid] = q
                self.event({"jsonrpc": "2.0", "id": rid, "method": request_method, "params": {}})
                answer = q.get(timeout=4)
                answers.append(answer)
                with lock:
                    del reverse[rid]
            self.event(response({"value": params["value"], "sample": answers[0].get("result", {}).get("content", {}).get("text"),
                                 "roots": answers[1].get("result", {}).get("roots")}))
        elif method in ["test/resume", "test/resume-long"]:
            cursor = uuid.uuid4().hex
            with lock:
                state["resumePosts"] += 1
                resumes[cursor] = response({"resumed": True})
            self.stream()
            retry = "9999999999999999999999999" if method.endswith("-long") else "80"
            self.wfile.write(f"id: {cursor}\ndata:\nretry: {retry}\n\n".encode())
            self.wfile.flush()
        elif method == "test/slow-headers":
            time.sleep(.3)
            self.reply(200, response({"ok": True}))
        elif method == "test/invalid-event-id":
            self.stream()
            self.wfile.write(b"id: \xff\ndata:\n\n")
            self.wfile.flush()
        elif method == "test/notify":
            notifications.put({"jsonrpc": "2.0", "method": "notifications/tools/list_changed"})
            self.reply(200, response({}))
        elif method == "test/wait":
            done = threading.Event()
            with lock:
                waiting[identifier] = done
            self.stream()
            if "_meta" in params:
                self.event({"jsonrpc": "2.0", "method": "notifications/progress", "params": {
                    "progressToken": params["_meta"]["progressToken"], "progress": 1}})
            else:
                self.wfile.flush()
            done.wait(5)
        elif method == "test/drop":
            with lock:
                state["droppedPosts"] += 1
            self.connection.shutdown(socket.SHUT_RDWR)
            self.connection.close()
            self.close_connection = True
        elif method == "test/session":
            self.reply(200, response({"session": sid}))
        elif method == "test/disconnect":
            with lock:
                state["disconnectPosts"] += 1
            self.stream()
            self.wfile.flush()
        elif method == "test/expire":
            with lock:
                sessions.discard(sid)
                state["expirePosts"] += 1
            self.reply(404)
        elif method == "test/redirect":
            self.reply(307, headers={"Location": "/redirect-target"})
        elif method == "test/oversize":
            self.reply(200, response({"value": "x" * 10000}))
        else:
            self.reply(200, response({}))

    def do_GET(self):
        try:
            if not self.authorized() or not self.valid_session():
                return
            cursor = self.headers.get("Last-Event-ID")
            if cursor:
                with lock:
                    result = resumes.pop(cursor, None)
                    state["resumeGets"] += 1
                if result is None:
                    self.reply(400)
                    return
                self.stream()
                self.event(result, identifier=uuid.uuid4().hex)
            else:
                self.stream()
                self.wfile.write(b": connected\n\n")
                self.wfile.flush()
                while True:
                    try:
                        message = notifications.get(timeout=.5)
                        self.event(message)
                    except queue.Empty:
                        self.wfile.write(b": heartbeat\n\n")
                        self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError):
            pass

    def do_DELETE(self):
        if self.authorized() and self.valid_session():
            with lock:
                sessions.discard(self.headers["Mcp-Session-Id"])
            self.reply(200)


class PeerServer(ThreadingHTTPServer):
    # Eight concurrent RPCs also open reverse-response and nested-request channels.
    # The stdlib default backlog of five rejects this test's TCP connection burst.
    request_queue_size = 128


server = PeerServer(("127.0.0.1", 0), Handler)
server.daemon_threads = True
scheme = "http"
if mode == "tls":
    import ssl
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(sys.argv[2], sys.argv[3])
    server.socket = context.wrap_socket(server.socket, server_side=True)
    scheme = "https"
print(f"{scheme}://127.0.0.1:{server.server_port}/mcp", flush=True)
server.serve_forever()
