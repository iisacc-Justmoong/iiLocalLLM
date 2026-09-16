"""Anonymous real GET/artifact transport and permission checks; inference is separate."""
from contextlib import contextmanager
from http.client import HTTPConnection
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import argparse
import asyncio
import hashlib
import json
import os
import re
import secrets
import subprocess
import tempfile
import threading
import time


def main():
    parser = argparse.ArgumentParser()
    for name in ('daemon', 'cli', 'mcp'):
        parser.add_argument(name, type=Path)
    parser.add_argument('--report', type=Path, required=True)
    parser.add_argument('--official-stdio', action='store_true')
    args = parser.parse_args()
    daemon, cli, mcp = (str(getattr(args, k).resolve()) for k in ('daemon', 'cli', 'mcp'))
    env = dict(os.environ)
    for key in ('DYLD_LIBRARY_PATH', 'DYLD_FRAMEWORK_PATH', 'DYLD_FALLBACK_LIBRARY_PATH', 'LIBRARY_PATH'):
        env.pop(key, None)
    args.report.parent.mkdir(parents=True, exist_ok=True)
    report = {'passed': False, 'binary_transport_only': True, 'binaries': [daemon, cli, mcp]}
    with tempfile.TemporaryDirectory(prefix='web-wire-', dir=args.report.parent) as directory:
        root = Path(directory)
        work = root / 'work'
        work.mkdir()
        body = b'%PDF-1.7\nWIRE_' + secrets.token_hex(16).encode() + b'\x00\n'
        received = []

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *_):
                pass

            def do_GET(self):
                received.append(dict(self.headers))
                if self.path == '/cross':
                    self.send_response(302)
                    self.send_header('Location', f'http://localhost:{self.server.server_port}/binary')
                    self.end_headers()
                    return
                self.send_response(200)
                self.send_header('Content-Type', 'application/pdf')
                self.send_header('Content-Length', str(len(body)))
                self.end_headers()
                self.wfile.write(body)

        web = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        worker = threading.Thread(target=web.serve_forever, daemon=True)
        worker.start()
        origin = f'http://127.0.0.1:{web.server_port}'
        fetch_args = {'url': origin + '/binary', 'prompt': 'Inspect this document'}
        token, second_token = secrets.token_urlsafe(36), secrets.token_urlsafe(36)

        def private(name, data):
            path = root / name
            path.write_text(data if isinstance(data, str) else json.dumps(data))
            path.chmod(0o600)
            return str(path)

        credentials = private('credentials', {'society': token, 'dreamscapes': second_token})
        auth = private('auth', token)

        @contextmanager
        def server(name, command, pattern):
            log_path = root / (name + '.log')
            with log_path.open('w') as log:
                p = subprocess.Popen(command, cwd=root, env=env, stdout=log, stderr=log)
                try:
                    deadline = time.monotonic() + 60
                    while True:
                        text = log_path.read_text()
                        assert p.poll() is None, text[-5000:]
                        match = re.search(pattern, text)
                        if match:
                            break
                        assert time.monotonic() < deadline, text[-5000:]
                        time.sleep(.02)
                    yield int(match[1])
                finally:
                    p.terminate()
                    try:
                        p.wait(timeout=15)
                    except subprocess.TimeoutExpired:
                        p.kill()
                        p.wait()
                    args.report.with_suffix('.' + name + '.log').write_bytes(log_path.read_bytes())

        def post(port, endpoint, method, params, credential=token, session=None):
            payload = {'id': secrets.token_hex(8), 'method': method, 'params': params}
            headers = {'Content-Type': 'application/json', 'Accept': 'application/json, text/event-stream', 'Authorization': 'Bearer ' + credential}
            if endpoint == '/mcp':
                payload['jsonrpc'] = '2.0'
                if method.startswith('notifications/'):
                    payload.pop('id')
                if session:
                    headers.update({'Mcp-Session-Id': session, 'MCP-Protocol-Version': '2025-11-25'})
            conn = HTTPConnection('127.0.0.1', port, timeout=20)
            try:
                conn.request('POST', endpoint, json.dumps(payload), headers)
                res = conn.getresponse()
                data = res.read()
                if res.getheader('Content-Type', '').startswith('text/event-stream'):
                    frames = [json.loads(line[5:]) for line in data.splitlines() if line.startswith(b'data:') and line[5:].strip()]
                    value = next(x for x in frames if x.get('id') == payload['id'] and ('result' in x or 'error' in x))
                else:
                    value = json.loads(data) if data else {}
                return res.status, value, res.getheader('Mcp-Session-Id')
            finally:
                conn.close()

        def check(data):
            assert data['code'] == 200 and data['bytes'] == len(body), data
            assert data['sha256'] == hashlib.sha256(body).hexdigest(), data
            path = Path(data['artifact']['path'])
            assert path.is_relative_to(root) and path.read_bytes() == body
            assert path.stat().st_mode & 0o077 == 0
            return str(path)

        try:
            daemon_args = [daemon, '--socket', str(root / 's'), '--http-port', '0', '--models-root', str(root / 'models'),
                '--agent-workspace', str(work), '--agent-state', str(root / 'api'), '--agent-credentials', credentials,
                '--agent-allow', 'WebFetch(domain:127.0.0.1)', '--agent-web-private-origin', origin,
                '--agent-no-apps', '--agent-no-background', '--agent-no-skills', '--agent-no-subagents', '--no-agent-profiles']
            with server('api', daemon_args, r'iiLocalLLM HTTP: http://127\.0\.0\.1:(\d+)') as port:
                def rpc(method, params=None, credential=token, expected=200):
                    status, value, _ = post(port, '/v1/rpc', method, params or {}, credential)
                    assert status == expected, (status, value)
                    return value.get('result', value)

                assert rpc('agent.info')['web_fetch_enabled']
                owner = rpc('agent.sessions.create', {'model': 'model://missing-fixture'})['session_id']
                other = rpc('agent.sessions.create', {'model': 'model://missing-fixture'}, second_token)['session_id']
                parameters = dict(fetch_args, session_id=owner)
                reply = rpc('agent.web.fetch', parameters)
                assert not reply['is_error'], reply
                first_path = check(reply['result'])
                reply = rpc('agent.web.fetch', dict(fetch_args, session_id=other), second_token)
                assert not reply['is_error'] and first_path != check(reply['result'])
                rpc('agent.web.fetch', parameters, second_token, 404)
                rpc('agent.web.fetch', parameters, 'invalid', 401)
                before = len(received)
                bad = rpc('agent.web.fetch', dict(parameters, headers={'Authorization': 'SHOULD_NOT_SEND'}))
                assert bad['is_error'] and len(received) == before
                redirect = rpc('agent.web.fetch', dict(parameters, url=origin + '/cross'))
                assert redirect['result']['redirect_url'].startswith('http://localhost:') and len(received) == before + 1
                denied = rpc('agent.web.fetch', dict(parameters, url=f'http://localhost:{web.server_port}/binary'))
                assert denied['is_error'] and len(received) == before + 1
                path = private('fetch.json', fetch_args)
                result = subprocess.run([cli, '--socket', str(root / 's'), '--auth-file', auth, 'agent', 'web', 'fetch', owner, path],
                    env=env, cwd=root, capture_output=True, text=True, timeout=20, check=True)
                assert not json.loads(result.stdout)['is_error']
                check(json.loads(result.stdout)['result'])
                assert rpc('agent.sessions.get', {'session_id': owner})['message_count'] == 0
                report['http_ipc_cli'] = True

            mcp_args = ['--workspace', str(work), '--models', str(root / 'models'), '--model', 'model://missing-fixture',
                '--allow', 'WebFetch(domain:127.0.0.1)', '--web-private-origin', origin,
                '--no-apps', '--no-background', '--no-skills', '--no-subagents', '--no-agent-profiles']
            init_args = {'protocolVersion': '2025-11-25', 'capabilities': {}, 'clientInfo': {'name': 'web-fetch-verifier', 'version': '1'}}
            with server('mcp', [mcp, *mcp_args, '--state', str(root / 'mcp-http'), '--http-port', '0', '--credentials', credentials], r'http://127\.0\.0\.1:(\d+)/mcp') as port:
                status, init, session = post(port, '/mcp', 'initialize', init_args)
                assert status == 200 and session and 'iisacc/webFetch' in init['result']['capabilities']['experimental'], init
                post(port, '/mcp', 'notifications/initialized', {}, session=session)
                _, listed, _ = post(port, '/mcp', 'tools/list', {}, session=session)
                assert any(t['name'] == 'WebFetch' for t in listed['result']['tools'])
                _, reply, _ = post(port, '/mcp', 'tools/call', {'name': 'WebFetch', 'arguments': fetch_args}, session=session)
                assert not reply['result'].get('isError'), reply
                check(reply['result']['structuredContent'])
                status, _, _ = post(port, '/mcp', 'tools/list', {}, credential=second_token, session=session)
                assert status == 404
                report['mcp_http'] = True

            if args.official_stdio:
                async def official():
                    from mcp import ClientSession, StdioServerParameters
                    from mcp.client.stdio import stdio_client
                    with args.report.with_suffix('.stdio.log').open('w') as err:
                        async with stdio_client(StdioServerParameters(command=mcp, args=[*mcp_args, '--state', str(root / 'mcp-stdio')], env=env, cwd=str(root)), errlog=err) as (reader, writer):
                            async with ClientSession(reader, writer) as client:
                                info = await client.initialize()
                                assert 'iisacc/webFetch' in info.capabilities.experimental
                                result = await client.call_tool('WebFetch', fetch_args)
                                assert not result.isError, result
                                check(result.structuredContent)
                                return True
                report['official_stdio'] = asyncio.run(official())
            assert all('Cookie' not in r and 'Authorization' not in r for r in received)
            report.update(passed=True, anonymous_requests=len(received), owner_isolation=True, redirects_reauthorized=True)
        finally:
            web.shutdown()
            web.server_close()
            worker.join(timeout=5)
            args.report.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report))


if __name__ == '__main__':
    main()
