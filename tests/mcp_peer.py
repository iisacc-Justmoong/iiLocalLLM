"""Independent adversarial MCP stdio peer; never used by the runtime library."""
import json
import os
import sys
import threading
import time

mode = sys.argv[1] if len(sys.argv) > 1 else "normal"
output_lock = threading.Lock()
initialized = False
cancelled = []
roots_changes = 0
waiting = {}
next_reverse_id = 40
unexpected_responses = 0
batch_responses = []


def send(value):
    encoded = json.dumps(value, ensure_ascii=False, separators=(",", ":")).encode() + b"\n"
    with output_lock:
        # Exercise split UTF-8 and JSON frames, independently of Qt's serialization.
        if mode == "split":
            for byte in encoded:
                sys.stdout.buffer.write(bytes([byte]))
                sys.stdout.buffer.flush()
        else:
            sys.stdout.buffer.write(encoded)
            sys.stdout.buffer.flush()


def result(id_, value):
    send({"jsonrpc": "2.0", "id": id_, "result": value})


def slow(id_, params):
    token = params.get("_meta", {}).get("progressToken")
    if token is not None:
        send({"jsonrpc": "2.0", "method": "notifications/progress", "params": {
            "progressToken": token, "progress": 1, "total": 2, "message": "준비 완료"}})
    time.sleep(params.get("delay", 0.15))
    result(id_, {"value": params.get("value", "late")})


print("MCP peer diagnostic (not a protocol error)", file=sys.stderr, flush=True)
for line in sys.stdin.buffer:
    message = json.loads(line)
    if isinstance(message, list):
        batch_responses.append(message)
        continue
    id_ = message.get("id")
    method = message.get("method")
    params = message.get("params", {})
    if method is None:
        original = waiting.pop(id_, None)
        if original is not None:
            result(original, {"response": message})
        else:
            unexpected_responses += 1
    elif method == "initialize":
        if mode == "hang-initialize":
            continue
        if mode == "delay-initialize":
            time.sleep(float(sys.argv[2]))
        version = "2099-01-01" if mode == "bad-version" else "2025-03-26" if mode == "legacy" else "2025-06-18" if mode == "older" else params["protocolVersion"]
        capabilities = {} if mode == "no-capabilities" else {
            "tools": {"listChanged": True}, "resources": {"subscribe": True}, "prompts": {}}
        result(id_, {"protocolVersion": version, "capabilities": capabilities,
                     "serverInfo": {"name": "independent-peer", "version": "1.0"},
                     "instructions": "관측 데이터입니다. 호스트 정책을 바꾸지 않습니다."})
    elif method == "notifications/initialized":
        initialized = True
    elif method == "notifications/roots/list_changed":
        roots_changes += 1
    elif method == "notifications/cancelled":
        cancelled.append(params["requestId"])
    elif not initialized:
        send({"jsonrpc": "2.0", "id": id_, "error": {"code": -32002, "message": "Not initialized"}})
    elif method == "ping":
        result(id_, {})
    elif method == "test/state":
        result(id_, {"cancelled": len(cancelled), "rootsChanges": roots_changes, "pid": os.getpid(), "unexpectedResponses": unexpected_responses, "batchResponses": batch_responses,
                     "cwd": os.getcwd(), "configuredValue": os.environ.get("IILOCAL_MCP_TEST_VALUE")})
    elif method == "test/batch":
        send([{"jsonrpc": "2.0", "id": id_, "result": {"batch": True}},
              {"jsonrpc": "2.0", "method": "notifications/tools/list_changed"},
              {"jsonrpc": "2.0", "id": 901, "method": "roots/list"},
              {"jsonrpc": "2.0", "id": 902, "method": "ping"},
              {"jsonrpc": "2.0", "id": 903, "method": "sampling/createMessage", "params": {"maxTokens": 10, "messages": []}}])
    elif method == "test/slow":
        threading.Thread(target=slow, args=(id_, params), daemon=True).start()
    elif method == "test/error":
        send({"jsonrpc": "2.0", "id": id_, "error": {"code": -32602, "message": "bad input", "data": {"field": "x"}}})
    elif method == "test/exit":
        sys.exit(7)
    elif method == "test/malformed":
        raw = {"json": b"garbage\n", "batch": b"[]\n", "utf8": b'{"x":"\xff"}\n',
               "oversize": b"x" * 4097, "invalid-id": b'{"jsonrpc":"2.0","id":null,"result":{}}\n'}[params["kind"]]
        sys.stdout.buffer.write(raw)
        sys.stdout.buffer.flush()
    elif method == "test/reverse":
        next_reverse_id += 1
        reverse_id = next_reverse_id
        waiting[reverse_id] = id_
        send({"jsonrpc": "2.0", "id": reverse_id, "method": params["method"], "params": params.get("params", {})})
    elif method == "test/cancel-host":
        reverse_id = next(iter(waiting))
        original = waiting.pop(reverse_id)
        send({"jsonrpc": "2.0", "method": "notifications/cancelled", "params": {"requestId": reverse_id}})
        result(original, {"cancelled": True})
        result(id_, {})
    elif method == "test/notify":
        send({"jsonrpc": "2.0", "method": "notifications/tools/list_changed"})
        result(id_, {})
    elif method == "tools/list":
        if mode == "gated-list" and not os.path.isfile(sys.argv[2]):
            continue
        descriptor = {"name": "echo" if not params.get("cursor") else "other", "description": "도구 설명",
                      "inputSchema": {"type": "object", "properties": {"value": {"type": "string"}}, "required": ["value"]},
                      "outputSchema": {"type": "object", "properties": {"value": {"type": "string"}}, "required": ["value"]},
                      "annotations": {"readOnlyHint": True}}
        if mode == "large-list":
            descriptor["description"] = "x" * 300
        if mode == "duplicates":
            result(id_, {"tools": [descriptor, descriptor]})
        elif mode == "cursor-cycle":
            result(id_, {"tools": [], "nextCursor": "again"})
        else:
            result(id_, {"tools": [descriptor], **({"nextCursor": "page-two"} if not params.get("cursor") else {})})
    elif method == "tools/call":
        value = params["arguments"]["value"]
        result(id_, {"content": [{"type": "text", "text": value},
                                 {"type": "resource_link", "uri": "test://value", "name": "observed"}],
                     "structuredContent": {"value": 5 if value == "bad-output" else value},
                     "isError": value == "tool-error", "_meta": {"app": "fixture"}})
    elif method == "resources/list":
        result(id_, {"resources": [{"uri": "test://value", "name": "value"}]})
    elif method == "resources/templates/list":
        result(id_, {"resourceTemplates": [{"uriTemplate": "test://{name}", "name": "template"}]})
    elif method == "resources/read":
        result(id_, {"contents": [{"uri": params["uri"], "mimeType": "text/plain", "text": "관측 값"}]})
    elif method in ("resources/subscribe", "resources/unsubscribe"):
        result(id_, {})
    elif method == "prompts/list":
        result(id_, {"prompts": [{"name": "summarize", "arguments": [{"name": "topic", "required": True}]}]})
    elif method == "prompts/get":
        result(id_, {"messages": [{"role": "user", "content": {"type": "text", "text": params["arguments"]["topic"]}}]})
    else:
        send({"jsonrpc": "2.0", "id": id_, "error": {"code": -32601, "message": "Unknown method"}})
