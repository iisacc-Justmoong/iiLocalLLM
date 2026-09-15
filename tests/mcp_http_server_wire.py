"""Independent stdlib HTTP peer; never imports iiLocalLLM's client implementation."""
import contextlib
import http.client
import json
import select
import subprocess
import sys
import time
import unittest
from urllib.parse import urlsplit

EXECUTABLE = sys.argv.pop(1)


def rpc(identity, method, params=None):
    return {"jsonrpc": "2.0", "id": identity, "method": method, "params": params or {}}


def notice(method, params=None):
    return {"jsonrpc": "2.0", "method": method, "params": params or {}}


def event(response):
    fields = {}
    while True:
        line = response.readline()
        if not line:
            return None
        if line in (b"\n", b"\r\n"):
            if fields:
                return fields
            continue
        key, _, value = line.decode("utf-8").rstrip("\r\n").partition(":")
        if key:
            fields[key] = value.removeprefix(" ")


def messages(response):
    output = []
    while (item := event(response)) is not None:
        if item.get("data"):
            output.append(json.loads(item["data"]))
    return output


@contextlib.contextmanager
def fixture(**options):
    process = subprocess.Popen([EXECUTABLE, json.dumps(options)], stdin=subprocess.PIPE,
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        if not select.select([process.stdout], [], [], 10)[0]:
            raise AssertionError("HTTP fixture startup timed out")
        endpoint = json.loads(process.stdout.readline())["endpoint"]
        yield Peer(endpoint)
    finally:
        try:
            out, err = process.communicate("stop\n", timeout=10)
        except subprocess.TimeoutExpired:
            process.kill()
            out, err = process.communicate()
            raise AssertionError(f"HTTP fixture shutdown hung: {err}")
        if process.returncode:
            raise AssertionError(f"HTTP fixture exited {process.returncode}: {err} {out}")


class Peer:
    def __init__(self, endpoint):
        self.url = urlsplit(endpoint)
        self.session = None
        self.version = "2025-11-25"
        self.credential = "alpha-fixture"
        self.sequence = 100

    def open(self, method, body=None, headers=None, token=True, session=True):
        values = {"Accept": "application/json, text/event-stream", "Content-Type": "application/json"}
        if token:
            values["Authorization"] = "Bearer " + self.credential
        if session and self.session:
            values.update({"Mcp-Session-Id": self.session, "Mcp-Protocol-Version": self.version})
        values.update(headers or {})
        connection = http.client.HTTPConnection(self.url.hostname, self.url.port, timeout=5)
        connection.request(method, self.url.path, body if isinstance(body, bytes) else json.dumps(body) if body is not None else None, values)
        response = connection.getresponse()
        return connection, response

    def status(self, method, body=None, **kwargs):
        connection, response = self.open(method, body, **kwargs)
        try:
            return response.status, dict(response.getheaders()), response.read()
        finally:
            response.close()
            connection.close()

    def initialize(self, version="2025-11-25", batch=False):
        self.version = version
        message = rpc(1, "initialize", {"protocolVersion": version, "capabilities": {},
                                       "clientInfo": {"name": "stdlib-peer", "version": "1"}})
        status, headers, body = self.status("POST", [message] if batch else message, session=False)
        if status != 200:
            raise AssertionError(f"initialize: {status} {body!r}")
        self.session = next(value for key, value in headers.items() if key.lower() == "mcp-session-id")
        result = json.loads(body)
        assert (result[0] if batch else result)["result"]["protocolVersion"] == version
        assert self.status("POST", notice("notifications/initialized"))[0] == 202
        return self

    def call(self, method, params=None):
        self.sequence += 1
        connection, response = self.open("POST", rpc(self.sequence, method, params))
        try:
            assert response.status == 200, (response.status, response.read())
            replies = messages(response)
            assert len(replies) == 1 and replies[0]["id"] == self.sequence, replies
            return replies[0]["result"]
        finally:
            response.close()
            connection.close()


class HttpServerWireTests(unittest.TestCase):
    def test_control_capacity_and_legacy_batches_cannot_promote_work(self):
        with fixture(maxStreams=1, maxControlStreams=1, maxStreamsPerSession=2) as peer:
            peer.initialize("2025-03-26")
            second = Peer(peer.url.geturl()).initialize()
            ordinary, work = peer.open("POST", rpc(2, "test/wait"))
            self.assertEqual(work.status, 200)
            event(work)
            # A mixed batch, a method-name prefix or client metadata cannot claim control capacity.
            self.assertEqual(peer.status("POST", [rpc(3, "ping"), rpc(4, "test/echo")])[0], 429)
            self.assertEqual(peer.status("POST", rpc(5, "test/controlUnknown", {"_meta": {"control": True}}))[0], 429)
            connection, response = peer.open("POST", [rpc(6, "ping"), rpc(7, "ping"), notice("notifications/roots/list_changed")])
            self.assertEqual(response.status, 200)
            self.assertEqual({x["id"] for x in messages(response)[0]}, {6, 7})
            response.close(); connection.close()
            control, held = peer.open("POST", rpc(8, "test/controlWait"))
            self.assertEqual(held.status, 200)
            event(held)
            self.assertEqual(peer.status("POST", rpc(9, "ping"))[0], 429)
            self.assertEqual(second.status("POST", rpc(9, "ping"))[0], 429)
            self.assertEqual(peer.status("POST", rpc(10, "test/echo"))[0], 429)
            self.assertEqual(peer.status("POST", notice("notifications/cancelled", {"requestId": 2}))[0], 202)
            self.assertEqual(messages(work), [])
            work.close(); ordinary.close()
            self.assertEqual(peer.call("test/echo")["principal"], "alpha")
            self.assertEqual(peer.status("POST", notice("notifications/cancelled", {"requestId": 8}))[0], 202)
            self.assertEqual(messages(held), [])
            held.close(); control.close()
            for _ in range(3):
                self.assertEqual(peer.call("ping"), {})

    def test_disconnected_control_keeps_its_retained_quota_until_cancelled(self):
        with fixture(maxStreams=1, maxControlStreams=1, maxStreamsPerSession=2) as peer:
            peer.initialize()
            connection, response = peer.open("POST", rpc(2, "test/controlWait"))
            self.assertEqual(response.status, 200)
            event(response)
            response.close(); connection.close()
            time.sleep(0.15)  # At least one fixture heartbeat detects the closed socket.
            self.assertEqual(peer.call("test/echo")["principal"], "alpha")
            status, _, body = peer.status("POST", rpc(3, "ping"))
            self.assertEqual(status, 429)
            self.assertIn(b"retained stream capacity", body)
            self.assertEqual(peer.status("POST", notice("notifications/cancelled", {"requestId": 2}))[0], 202)
            deadline = time.monotonic() + 2
            while True:
                peer.sequence += 1
                status, _, body = peer.status("POST", rpc(peer.sequence, "ping"))
                if status == 200:
                    break
                self.assertEqual(status, 429)
                self.assertLess(time.monotonic(), deadline, body)
                time.sleep(0.01)

    def test_resumed_control_uses_its_original_capacity_and_owner(self):
        with fixture(maxStreams=1, maxControlStreams=1, maxStreamsPerSession=3) as peer:
            peer.initialize()
            foreign = Peer(peer.url.geturl()).initialize()
            prior, completed = peer.open("POST", rpc(10, "test/echo"))
            normal_cursor = event(completed)["id"]
            self.assertEqual(messages(completed)[0]["id"], 10)
            completed.close(); prior.close()
            ordinary, work = peer.open("POST", rpc(2, "test/wait"))
            self.assertEqual(work.status, 200)
            event(work)
            self.assertEqual(peer.status("GET", headers={"Last-Event-ID": normal_cursor})[0], 429)
            connection, response = peer.open("POST", rpc(3, "test/controlDelay", {"label": "once", "milliseconds": 500}))
            self.assertEqual(response.status, 200)
            cursor = event(response)["id"]
            response.close(); connection.close()
            self.assertEqual(foreign.status("GET", headers={"Last-Event-ID": cursor})[0], 410)
            deadline = time.monotonic() + 2
            while True:
                connection, response = peer.open("GET", headers={"Last-Event-ID": cursor})
                if response.status == 200:
                    break
                self.assertEqual(response.status, 429)
                response.read(); response.close(); connection.close()
                self.assertLess(time.monotonic(), deadline)
                time.sleep(0.01)
            self.assertEqual(messages(response), [{"jsonrpc": "2.0", "id": 3, "result": {"label": "once"}}])
            response.close(); connection.close()
            self.assertEqual(peer.status("POST", rpc(4, "test/echo"))[0], 429)
            self.assertEqual(peer.status("POST", notice("notifications/cancelled", {"requestId": 2}))[0], 202)
            self.assertEqual(messages(work), [])
            work.close(); ordinary.close()
            self.assertEqual(peer.call("test/stats")["once"], 1)

    def test_auth_origin_and_envelope_validation(self):
        with fixture(maxRequestBytes=1024) as peer:
            status, headers, body = peer.status("POST", rpc(1, "ping"), token=False)
            self.assertEqual(status, 401)
            self.assertIn("Bearer", headers["WWW-Authenticate"])
            self.assertEqual(peer.status("POST", rpc(1, "ping"), headers={"Origin": "https://evil.example"})[0], 403)
            self.assertEqual(peer.status("POST", rpc(1, "ping"), headers={"Host": "evil.example"})[0], 403)
            self.assertEqual(peer.status("POST", rpc(1, "ping"), headers={"Accept": "application/json"})[0], 406)
            self.assertEqual(peer.status("POST", rpc(1, "ping"), headers={"Content-Type": "text/plain"})[0], 415)
            self.assertEqual(peer.status("POST", b'{"value":"\xff"}')[0], 400)
            self.assertEqual(peer.status("POST", b"x" * 2048)[0], 413)
            self.assertEqual(peer.status("POST", rpc(1, "ping"))[0], 400)
            self.assertEqual(peer.status("GET")[0], 400)
            self.assertEqual(peer.status("HEAD")[0], 405)
            peer.credential = "provider-error"
            status, _, body = peer.status("POST", rpc(1, "ping"))
            self.assertEqual(status, 503)
            self.assertNotIn(b"sensitive", body)
            peer.credential = "alpha-fixture"
            peer.initialize()
            for method in ("POST", "GET", "DELETE"):
                self.assertEqual(peer.status(method, rpc(2, "ping") if method == "POST" else None,
                    headers={"Mcp-Protocol-Version": "2026-03-01"})[0], 400)
                peer.credential = "beta-fixture"
                self.assertEqual(peer.status(method, rpc(2, "ping") if method == "POST" else None)[0], 404)
                peer.credential = "alpha-fixture"
            self.assertEqual(peer.call("test/echo", {"text": "한국어"})["value"]["text"], "한국어")
            self.assertEqual(peer.status("POST", [rpc(9, "ping")])[0], 400)
            status, _, body = peer.status("POST", notice("notifications/roots/list_changed"))
            self.assertEqual((status, body), (202, b""))
            self.assertEqual(peer.status("DELETE")[0], 200)
            self.assertEqual(peer.status("POST", rpc(10, "ping"))[0], 404)

    def test_cors_preflight(self):
        with fixture() as peer:
            headers = {"Origin": "https://fixture.example", "Access-Control-Request-Method": "POST",
                       "Access-Control-Request-Headers": "Content-Type, Authorization, MCP-Session-Id"}
            status, returned, body = peer.status("OPTIONS", headers=headers, token=False)
            self.assertEqual((status, body), (204, b""))
            self.assertEqual(returned["Access-Control-Allow-Origin"], headers["Origin"])
            headers["Access-Control-Request-Headers"] = "x-not-approved"
            self.assertEqual(peer.status("OPTIONS", headers=headers, token=False)[0], 400)

    def test_disconnect_replays_only_original_response_without_reexecution(self):
        with fixture() as peer:
            peer.initialize()
            connection, response = peer.open("POST", rpc(2, "test/delay", {"label": "once", "milliseconds": 180}))
            self.assertEqual(response.status, 200)
            cursor = event(response)["id"]
            response.close()
            connection.close()
            unrelated = peer.call("test/echo", {"text": "unrelated"})
            self.assertEqual(unrelated["value"]["text"], "unrelated")
            connection, response = peer.open("GET", headers={"Last-Event-ID": cursor})
            self.assertEqual(response.status, 200)
            self.assertEqual(messages(response), [{"jsonrpc": "2.0", "id": 2, "result": {"label": "once"}}])
            response.close()
            connection.close()
            self.assertEqual(peer.call("test/stats")["once"], 1)

    def test_notifications_and_shared_progress_tokens_stay_on_their_streams(self):
        with fixture() as peer:
            peer.initialize()
            background, inbox = peer.open("GET")
            self.assertEqual(inbox.status, 200)
            self.assertEqual(event(inbox)["data"], "")
            first, a = peer.open("POST", rpc(2, "test/delay", {"label": "a", "_meta": {"progressToken": "same"}}))
            second, b = peer.open("POST", rpc(3, "test/delay", {"label": "b", "_meta": {"progressToken": "same"}}))
            peer.call("test/notify")
            self.assertEqual(json.loads(event(inbox)["data"])["method"], "notifications/tools/list_changed")
            for response, identity, label in ((a, 2, "a"), (b, 3, "b")):
                frames = messages(response)
                self.assertEqual([x["params"]["progress"] for x in frames[:-1]], [1, 2])
                self.assertEqual(frames[-1], {"jsonrpc": "2.0", "id": identity, "result": {"label": label}})
                response.close()
            first.close(); second.close(); inbox.close(); background.close()

    def test_reverse_rpc_and_cancellation_retain_the_parent_channel(self):
        with fixture() as peer:
            peer.initialize()
            connection, response = peer.open("POST", rpc(2, "test/reverse"))
            event(response)
            reverse = json.loads(event(response)["data"])
            self.assertEqual(reverse["method"], "ping")
            self.assertEqual(peer.status("POST", {"jsonrpc": "2.0", "id": reverse["id"], "result": {"value": "reverse"}})[0], 202)
            self.assertEqual(messages(response)[0]["result"]["value"], "reverse")
            response.close(); connection.close()
            connection, response = peer.open("POST", rpc(3, "test/reverse"))
            event(response)
            reverse = json.loads(event(response)["data"])
            self.assertEqual(peer.status("POST", notice("notifications/cancelled", {"requestId": 3}))[0], 202)
            frames = messages(response)
            self.assertEqual(frames, [notice("notifications/cancelled", {"requestId": reverse["id"], "reason": "Request cancelled or timed out"})])
            response.close(); connection.close()

    def test_legacy_batch_cancellation_preserves_invalid_entry_response(self):
        with fixture() as peer:
            peer.initialize("2025-03-26", batch=True)
            connection, response = peer.open("POST", [rpc(2, "test/wait", {"_meta": {"progressToken": "start"}}), 42])
            event(response)
            self.assertEqual(json.loads(event(response)["data"])["method"], "notifications/progress")
            self.assertEqual(peer.status("POST", notice("notifications/cancelled", {"requestId": 2}))[0], 202)
            frames = messages(response)
            self.assertEqual(len(frames), 1, frames)
            self.assertIsInstance(frames[0], list)
            self.assertEqual(frames[0][0]["error"]["code"], -32600)
            response.close(); connection.close()

    def test_history_expiration_and_session_capacity(self):
        with fixture(maxSessions=1, streamRetentionMs=40, sessionIdleTimeoutMs=1000) as peer:
            peer.initialize()
            second = Peer(peer.url.geturl())
            self.assertEqual(second.status("POST", rpc(1, "initialize", {"protocolVersion": peer.version,
                "clientInfo": {"name": "second", "version": "1"}, "capabilities": {}}))[0], 429)
            connection, response = peer.open("POST", rpc(2, "ping"))
            cursor = event(response)["id"]
            self.assertEqual(messages(response)[0]["id"], 2)
            response.close(); connection.close()
            time.sleep(0.12)
            self.assertEqual(peer.status("GET", headers={"Last-Event-ID": cursor})[0], 410)
            self.assertEqual(peer.call("test/echo")["principal"], "alpha")
        with fixture(maxHistoryEvents=2) as peer:
            peer.initialize()
            connection, response = peer.open("POST", rpc(2, "test/delay", {"_meta": {"progressToken": "x"}}))
            cursor = event(response)["id"]
            response.close(); connection.close()
            time.sleep(0.25)
            self.assertEqual(peer.status("GET", headers={"Last-Event-ID": cursor})[0], 410)
        with fixture(sessionIdleTimeoutMs=50) as peer:
            peer.initialize()
            time.sleep(0.15)
            self.assertEqual(peer.status("POST", rpc(2, "ping"))[0], 404)


if __name__ == "__main__":
    unittest.main(verbosity=2)
