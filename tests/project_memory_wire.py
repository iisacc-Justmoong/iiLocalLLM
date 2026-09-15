"""Real HTTP, native IPC and MCP transports for project-memory operations."""
from contextlib import contextmanager
from http.client import HTTPConnection
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
import time


def main():
    parser = argparse.ArgumentParser()
    for name in ('daemon', 'cli', 'mcp'):
        parser.add_argument(name, type=Path)
    parser.add_argument('--report', type=Path, required=True)
    parser.add_argument('--official-stdio', action='store_true')
    args = parser.parse_args()
    binaries = [str(getattr(args, name).resolve()) for name in ('daemon', 'cli', 'mcp')]
    daemon, cli, mcp = binaries
    report = {'passed': False, 'binaries': binaries}
    env = dict(os.environ)
    for name in ('DYLD_LIBRARY_PATH', 'DYLD_FRAMEWORK_PATH', 'DYLD_FALLBACK_LIBRARY_PATH', 'LIBRARY_PATH'):
        env.pop(name, None)
    args.report.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='memory-wire-', dir=args.report.parent) as directory:
        root = Path(directory)
        work = root / 'work'
        work.mkdir()
        token, other = secrets.token_urlsafe(36), secrets.token_urlsafe(36)

        def private(name, value):
            path = root / name
            path.write_text(value if isinstance(value, str) else json.dumps(value))
            path.chmod(0o600)
            return str(path)

        credentials = private('credentials', {'society': token, 'dreamscapes': other})
        auth = private('auth', token)

        @contextmanager
        def server(name, command, pattern):
            log_path = root / (name + '.log')
            with log_path.open('w') as log:
                process = subprocess.Popen(command, stdout=log, stderr=log, env=env)
                try:
                    deadline = time.monotonic() + 30
                    while True:
                        text = log_path.read_text()
                        assert process.poll() is None, text[-4000:]
                        match = re.search(pattern, text)
                        if match:
                            break
                        assert time.monotonic() < deadline, 'Server startup deadline expired'
                        time.sleep(.02)
                    yield int(match[1])
                finally:
                    process.terminate()
                    try:
                        process.wait(timeout=10)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait()
                    args.report.with_suffix('.' + name + '.log').write_bytes(log_path.read_bytes())

        def post(port, endpoint, method, params, credential=token, session=None):
            payload = {'id': secrets.token_hex(8), 'method': method, 'params': params}
            headers = {'Content-Type': 'application/json', 'Accept': 'application/json, text/event-stream', 'Authorization': 'Bearer ' + credential}
            if endpoint == '/mcp':
                payload['jsonrpc'] = '2.0'
                if method.startswith('notifications/'):
                    payload.pop('id')
                if session:
                    headers['Mcp-Session-Id'] = session
                    headers['MCP-Protocol-Version'] = '2025-11-25'
            connection = HTTPConnection('127.0.0.1', port, timeout=20)
            try:
                connection.request('POST', endpoint, json.dumps(payload), headers)
                response = connection.getresponse()
                raw = response.read()
                if response.getheader('Content-Type', '').startswith('text/event-stream'):
                    frames = [json.loads(line[5:]) for line in raw.splitlines() if line.startswith(b'data:') and line[5:].strip()]
                    value = next(frame for frame in frames if frame.get('id') == payload['id'] and ('result' in frame or 'error' in frame))
                else:
                    value = json.loads(raw) if raw else {}
                return response.status, value, response.getheader('Mcp-Session-Id')
            finally:
                connection.close()

        daemon_command = [daemon, '--socket', str(root / 's'), '--http-port', '0', '--models-root', str(root / 'models'),
            '--agent-workspace', str(work), '--agent-state', str(root / 'api'), '--agent-credentials', credentials,
            '--agent-allow', 'Write', '--agent-allow', 'Edit', '--agent-allow', 'MemoryForget',
            '--agent-no-apps', '--agent-no-background', '--agent-no-skills', '--agent-no-subagents', '--no-agent-profiles']
        try:
            with server('api', daemon_command, r'iiLocalLLM HTTP: http://127\.0\.0\.1:(\d+)') as port:
                def rpc(method, params=None, credential=token, expected=200):
                    status, result, _ = post(port, '/v1/rpc', method, params or {}, credential)
                    assert status == expected, (status, result)
                    return result.get('result', result)

                assert rpc('agent.info')['project_memory_enabled']
                owner = rpc('agent.sessions.create', {'model': 'model://fixture'})['session_id']
                second = rpc('agent.sessions.create', {'model': 'model://fixture'}, other)['session_id']
                first_memory = rpc('agent.memory.get', {'session_id': owner})
                second_memory = rpc('agent.memory.get', {'session_id': second}, other)
                assert first_memory['directory'] != second_memory['directory']
                index = first_memory['index_path']
                note = 'Persistent API / IPC note: MEMORY_WIRE_853'
                params = private('write.json', {'path': index, 'content': note})
                written = subprocess.run([cli, '--socket', str(root / 's'), '--auth-file', auth, 'agent', 'memory', 'write', owner, params],
                    text=True, capture_output=True, timeout=20, env=env)
                assert written.returncode == 0 and not json.loads(written.stdout)['is_error'], written.stderr
                read = rpc('agent.memory.read', {'session_id': owner, 'path': index})
                assert read['text'] == note and read['result']['sha256'] == hashlib.sha256(note.encode()).hexdigest()
                rpc('agent.memory.get', {'session_id': owner}, other, 404)
                rpc('agent.memory.get', {'session_id': owner}, 'invalid', 401)
                rpc('agent.memory.read', {'session_id': second, 'path': index}, other, 400)
                assert rpc('agent.memory.get', {'session_id': second}, other)['index'] == ''
                assert rpc('agent.memory.forget', {'session_id': owner, 'path': index, 'sha256': '0' * 64})['is_error']
                assert Path(index).exists()
                gone = rpc('agent.memory.forget', {'session_id': owner, 'path': index, 'sha256': read['result']['sha256']})
                assert not gone['is_error'] and not Path(index).exists()
                assert Path(gone['result']['backup_path']).read_text() == note
                report['http_ipc'] = {'passed': True, 'tenant_isolation': True, 'cli_write': True, 'stale_delete_denied': True, 'backup_verified': True}

            command = [mcp, '--workspace', str(work), '--model', 'model://fixture', '--models', str(root / 'models'),
                '--state', str(root / 'mcp'), '--http-port', '0', '--credentials', credentials, '--allow', 'Write', '--allow', 'MemoryForget',
                '--no-apps', '--no-background', '--no-skills', '--no-subagents', '--no-agent-profiles']
            with server('mcp', command, r'http://127\.0\.0\.1:(\d+)/mcp') as port:
                status, initialized, session = post(port, '/mcp', 'initialize', {'protocolVersion': '2025-11-25', 'capabilities': {},
                    'clientInfo': {'name': 'memory-wire', 'version': '1'}})
                assert status == 200 and session and 'result' in initialized
                assert post(port, '/mcp', 'notifications/initialized', {}, session=session)[0] == 202

                def tool(name, arguments=None):
                    status, value, _ = post(port, '/mcp', 'tools/call', {'name': name, 'arguments': arguments or {}}, session=session)
                    assert status == 200 and 'result' in value, value
                    return value['result']

                state = tool('iiLocalLLM.agent.memory.get')['structuredContent']
                index = state['index_path']
                assert not tool('Write', {'path': index, 'content': 'MCP_MEMORY_617'})['isError']
                read = tool('Read', {'path': index})
                assert not read['isError'] and 'MCP_MEMORY_617' in json.dumps(read)
                assert tool('iiLocalLLM.agent.memory.get')['structuredContent']['index'] == 'MCP_MEMORY_617'
                assert not tool('MemoryForget', {'path': index, 'sha256': read['structuredContent']['sha256']})['isError']
                assert not Path(index).exists()
                report['mcp_http'] = {'passed': True, 'index': True, 'native_tools': True}

            if args.official_stdio:
                from mcp import ClientSession, StdioServerParameters
                from mcp.client.stdio import stdio_client

                async def official():
                    params = StdioServerParameters(command=mcp, args=['--workspace', str(work), '--model', 'model://fixture',
                        '--models', str(root / 'models'), '--state', str(root / 'stdio'), '--allow', 'Write', '--no-apps',
                        '--no-background', '--no-skills', '--no-subagents', '--no-agent-profiles'], env=env)
                    async with stdio_client(params) as (read_stream, write_stream):
                        async with ClientSession(read_stream, write_stream) as client:
                            await client.initialize()
                            state = await client.call_tool('iiLocalLLM.agent.memory.get', {})
                            assert not state.isError
                            index = state.structuredContent['index_path']
                            result = await client.call_tool('Write', {'path': index, 'content': 'OFFICIAL_STDIO_MEMORY'})
                            assert not result.isError
                            result = await client.call_tool('Read', {'path': index})
                            assert not result.isError and 'OFFICIAL_STDIO_MEMORY' in str(result.content)
                asyncio.run(official())
                report['official_stdio'] = {'passed': True}
            report['passed'] = True
        finally:
            args.report.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report))


if __name__ == '__main__':
    main()
