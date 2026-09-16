"""Real clangd over authenticated HTTP/IPC and MCP. No LLM inference is used."""
from contextlib import contextmanager
from http.client import HTTPConnection
from pathlib import Path
import argparse
import asyncio
import json
import os
import re
import secrets
import subprocess
import tempfile
import time


def main():
    parser = argparse.ArgumentParser()
    for name in ('daemon', 'cli', 'mcp', 'clangd'):
        parser.add_argument(name, type=Path)
    parser.add_argument('--report', type=Path, required=True)
    parser.add_argument('--official-stdio', action='store_true')
    args = parser.parse_args()
    daemon, cli, mcp, clangd = (str(getattr(args, key).resolve()) for key in ('daemon', 'cli', 'mcp', 'clangd'))
    env = dict(os.environ)
    for key in ('DYLD_LIBRARY_PATH', 'DYLD_FRAMEWORK_PATH', 'DYLD_FALLBACK_LIBRARY_PATH', 'LIBRARY_PATH'):
        env.pop(key, None)
    args.report.parent.mkdir(parents=True, exist_ok=True)
    report = {'passed': False, 'real_clangd': True, 'llm_inference': False}
    with tempfile.TemporaryDirectory(prefix='lsp-wire-', dir=args.report.parent) as directory:
        root = Path(directory).resolve()
        work = root / 'work'
        work.mkdir()
        subprocess.run(['git', 'init', '-q', str(work)], check=True)
        marker = 'wire_' + secrets.token_hex(6)
        (work / 'main.cpp').write_text(f'int {marker}() {{ return 7; }}\n')
        query = {'operation': 'documentSymbol', 'filePath': 'main.cpp', 'line': 1, 'character': 1}
        token, other_token = secrets.token_urlsafe(36), secrets.token_urlsafe(36)

        def private(name, value):
            path = root / name
            path.write_text(value if isinstance(value, str) else json.dumps(value))
            path.chmod(0o600)
            return str(path)

        credentials = private('credentials', {'society': token, 'dreamscapes': other_token})
        auth = private('token', token)
        config = private('lsp.json', {'servers': {'clangd': {'command': clangd,
            'args': ['--background-index=false', '--pch-storage=memory', '--log=error', '--enable-config=false'],
            'extensionToLanguage': {'.cpp': 'cpp'}}}})

        @contextmanager
        def server(name, command, pattern):
            path = root / (name + '.log')
            with path.open('w') as log:
                process = subprocess.Popen(command, cwd=root, env=env, stdout=log, stderr=log)
                try:
                    deadline = time.monotonic() + 60
                    while True:
                        text = path.read_text()
                        assert process.poll() is None, text[-5000:]
                        found = re.search(pattern, text)
                        if found:
                            break
                        assert time.monotonic() < deadline, text[-5000:]
                        time.sleep(.02)
                    yield int(found[1])
                finally:
                    process.terminate()
                    try:
                        process.wait(timeout=15)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait()
                    args.report.with_suffix('.' + name + '.log').write_bytes(path.read_bytes())

        def post(port, endpoint, method, params, credential=token, session=None):
            payload = {'id': secrets.token_hex(8), 'method': method, 'params': params}
            headers = {'Content-Type': 'application/json', 'Accept': 'application/json, text/event-stream', 'Authorization': 'Bearer ' + credential}
            if endpoint == '/mcp':
                payload['jsonrpc'] = '2.0'
                if method.startswith('notifications/'):
                    payload.pop('id')
                if session:
                    headers.update({'Mcp-Session-Id': session, 'MCP-Protocol-Version': '2025-11-25'})
            connection = HTTPConnection('127.0.0.1', port, timeout=40)
            try:
                connection.request('POST', endpoint, json.dumps(payload), headers)
                response = connection.getresponse()
                body = response.read()
                if response.getheader('Content-Type', '').startswith('text/event-stream'):
                    frames = [json.loads(line[5:]) for line in body.splitlines() if line.startswith(b'data:') and line[5:].strip()]
                    data = next(item for item in frames if item.get('id') == payload['id'] and ('result' in item or 'error' in item))
                else:
                    data = json.loads(body) if body else {}
                return response.status, data, response.getheader('Mcp-Session-Id')
            finally:
                connection.close()

        def check(value):
            assert value['operation'] == 'documentSymbol' and value['resultCount'] == 1, value
            assert marker in value['result'] and value['data'][0]['name'] == marker, value

        def stopped(pid):
            for _ in range(250):
                try:
                    os.kill(pid, 0)
                except ProcessLookupError:
                    return True
                time.sleep(.02)
            return False

        try:
            command = [daemon, '--socket', str(root / 's'), '--http-port', '0', '--models-root', str(root / 'models'),
                '--agent-workspace', str(work), '--agent-state', str(root / 'api'), '--agent-credentials', credentials,
                '--agent-lsp-config', config, '--agent-no-apps', '--agent-no-background', '--agent-no-skills', '--agent-no-subagents', '--no-agent-profiles']
            with server('api', command, r'iiLocalLLM HTTP: http://127\.0\.0\.1:(\d+)') as port:
                def rpc(method, params=None, credential=token, expected=200):
                    status, data, _ = post(port, '/v1/rpc', method, params or {}, credential)
                    assert status == expected, (status, data)
                    return data.get('result', data)
                assert rpc('agent.info')['lsp_enabled']
                first = rpc('agent.sessions.create', {'model': 'model://missing-fixture'})['session_id']
                second = rpc('agent.sessions.create', {'model': 'model://missing-fixture'}, other_token)['session_id']
                for owner, credential in ((first, token), (second, other_token)):
                    result = rpc('agent.lsp.query', dict(query, session_id=owner), credential)
                    assert not result['is_error'], result
                    check(result['result'])
                first_pid = rpc('agent.lsp.status', {'session_id': first})['servers'][0]['pid']
                second_pid = rpc('agent.lsp.status', {'session_id': second}, other_token)['servers'][0]['pid']
                assert first_pid != second_pid
                rpc('agent.lsp.query', dict(query, session_id=first), other_token, 404)
                rpc('agent.lsp.status', {'session_id': first}, 'invalid', 401)
                assert rpc('agent.lsp.query', dict(query, session_id=first, command='bad'))['is_error']
                assert rpc('agent.lsp.query', dict(query, session_id=first, filePath=config))['is_error']
                query_file = private('query.json', query)
                result = subprocess.run([cli, '--socket', str(root / 's'), '--auth-file', auth, 'agent', 'lsp', 'query', first, query_file], cwd=root, env=env, capture_output=True, text=True, timeout=40, check=True)
                data = json.loads(result.stdout)
                assert not data['is_error'], data
                check(data['result'])
                assert rpc('agent.sessions.get', {'session_id': first})['message_count'] == 0
                report['http_ipc_cli'] = True
                report['tenant_process_isolation'] = True
            assert stopped(first_pid) and stopped(second_pid)

            mcp_args = ['--workspace', str(work), '--models', str(root / 'models'), '--model', 'model://missing-fixture', '--lsp-config', config,
                '--no-apps', '--no-background', '--no-skills', '--no-subagents', '--no-agent-profiles']
            init_args = {'protocolVersion': '2025-11-25', 'capabilities': {}, 'clientInfo': {'name': 'lsp-verifier', 'version': '1'}}
            with server('mcp', [mcp, *mcp_args, '--state', str(root / 'mcp-http'), '--http-port', '0', '--credentials', credentials], r'http://127\.0\.0\.1:(\d+)/mcp') as port:
                status, init, session = post(port, '/mcp', 'initialize', init_args)
                assert status == 200 and session and 'iisacc/lsp' in init['result']['capabilities']['experimental'], init
                post(port, '/mcp', 'notifications/initialized', {}, session=session)
                _, listed, _ = post(port, '/mcp', 'tools/list', {}, session=session)
                assert any(tool['name'] == 'LSP' for tool in listed['result']['tools'])
                _, result, _ = post(port, '/mcp', 'tools/call', {'name': 'LSP', 'arguments': query}, session=session)
                assert not result['result'].get('isError'), result
                check(result['result']['structuredContent'])
                _, state, _ = post(port, '/mcp', 'tools/call', {'name': 'iiLocalLLM.agent.lsp.status', 'arguments': {}}, session=session)
                pid = state['result']['structuredContent']['servers'][0]['pid']
                assert post(port, '/mcp', 'tools/list', {}, other_token, session)[0] == 404
                connection = HTTPConnection('127.0.0.1', port, timeout=15)
                connection.request('DELETE', '/mcp', headers={'Authorization': 'Bearer ' + token, 'Mcp-Session-Id': session, 'MCP-Protocol-Version': '2025-11-25'})
                response = connection.getresponse()
                assert response.status in (200, 204), response.read()
                response.read()
                connection.close()
                assert stopped(pid), 'Connection close retained clangd'
                report['mcp_http'] = True
                report['connection_cleanup'] = True

            if args.official_stdio:
                async def official():
                    from mcp import ClientSession, StdioServerParameters
                    from mcp.client.stdio import stdio_client
                    with args.report.with_suffix('.stdio.log').open('w') as error:
                        async with stdio_client(StdioServerParameters(command=mcp, args=[*mcp_args, '--state', str(root / 'mcp-stdio')], env=env, cwd=str(root)), errlog=error) as (reader, writer):
                            async with ClientSession(reader, writer) as client:
                                init = await client.initialize()
                                assert 'iisacc/lsp' in init.capabilities.experimental
                                result = await client.call_tool('LSP', query)
                                assert not result.isError, result
                                check(result.structuredContent)
                                state = await client.call_tool('iiLocalLLM.agent.lsp.status', {})
                                pid = state.structuredContent['servers'][0]['pid']
                        assert stopped(pid), 'Stdio close retained clangd'
                        return True
                report['official_stdio'] = asyncio.run(official())
            report['passed'] = True
        finally:
            args.report.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report))


if __name__ == '__main__':
    main()
