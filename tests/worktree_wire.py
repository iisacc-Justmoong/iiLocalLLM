"""Real Git worktrees over authenticated HTTP/IPC and MCP. No LLM inference is used."""
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
    for name in ('daemon', 'cli', 'mcp'):
        parser.add_argument(name, type=Path)
    parser.add_argument('--report', type=Path, required=True)
    parser.add_argument('--official-stdio', action='store_true')
    args = parser.parse_args()
    daemon, cli, mcp = (str(getattr(args, key).resolve()) for key in ('daemon', 'cli', 'mcp'))
    env = dict(os.environ)
    for key in ('DYLD_LIBRARY_PATH', 'DYLD_FRAMEWORK_PATH', 'DYLD_FALLBACK_LIBRARY_PATH', 'LIBRARY_PATH'):
        env.pop(key, None)
    args.report.parent.mkdir(parents=True, exist_ok=True)
    report = {'passed': False, 'real_git': True, 'llm_inference': False}
    with tempfile.TemporaryDirectory(prefix='worktree-wire-', dir=args.report.parent) as directory:
        root = Path(directory).resolve()
        work = root / 'work'
        work.mkdir()
        def git(*values):
            return subprocess.run(['git', '-C', str(work), *values], check=True, capture_output=True, text=True).stdout.strip()
        git('init', '-q', '-b', 'main')
        git('config', 'user.name', 'Fixture')
        git('config', 'user.email', 'fixture@example.invalid')
        marker = 'wire_' + secrets.token_hex(6)
        (work / 'marker.txt').write_text(marker)
        git('add', '.')
        git('commit', '-qm', 'initial')
        token, other_token = secrets.token_urlsafe(36), secrets.token_urlsafe(36)

        def private(name, value):
            path = root / name
            path.write_text(value if isinstance(value, str) else json.dumps(value))
            path.chmod(0o600)
            return str(path)

        credentials = private('credentials', {'society': token, 'dreamscapes': other_token})
        auth = private('token', token)
        config = private('worktrees.json', {'directory': str(root / 'worktrees'), 'fetch_missing_base': False})

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

        try:
            command = [daemon, '--socket', str(root / 's'), '--http-port', '0', '--models-root', str(root / 'models'),
                '--agent-workspace', str(work), '--agent-state', str(root / 'api'), '--agent-credentials', credentials,
                '--agent-worktree-config', config, '--agent-allow', 'EnterWorktree', '--agent-allow', 'ExitWorktree',
                '--agent-allow', 'Bash', '--agent-no-apps', '--agent-no-skills', '--agent-no-subagents', '--no-agent-profiles']
            with server('api', command, r'iiLocalLLM HTTP: http://127\.0\.0\.1:(\d+)') as port:
                def rpc(method, params=None, credential=token, expected=200):
                    status, data, _ = post(port, '/v1/rpc', method, params or {}, credential)
                    assert status == expected, (status, data)
                    return data.get('result', data)
                assert rpc('agent.info')['worktrees_enabled']
                owner = rpc('agent.sessions.create', {'model': 'model://missing-fixture'})['session_id']
                entered = rpc('agent.worktrees.enter', {'session_id': owner, 'name': 'api'})
                assert not entered['is_error'], entered
                path = Path(entered['result']['worktreePath'])
                assert path != work and (path / 'marker.txt').read_text() == marker
                assert rpc('agent.sessions.get', {'session_id': owner})['working_directory'] == str(path)
                rpc('agent.worktrees.exit', {'session_id': owner, 'action': 'remove'}, other_token, 404)
                rpc('agent.worktrees.status', {'session_id': owner}, 'invalid', 401)
                assert rpc('agent.worktrees.exit', {'session_id': owner, 'action': 'keep', 'directory': str(work)})['is_error']
                shell = rpc('agent.shell.start', {'session_id': owner, 'command': 'pwd'})
                assert not shell['is_error'], shell
                task = shell['result']['task_id']
                output = rpc('agent.shell.output', {'session_id': owner, 'task_id': task, 'block': True})
                assert str(path) in json.dumps(output), output
                (path / 'new.txt').write_text('preserve me')
                assert rpc('agent.worktrees.exit', {'session_id': owner, 'action': 'remove'})['is_error']
                assert (path / 'new.txt').exists()
                params = private('exit.json', {'action': 'keep'})
                result = subprocess.run([cli, '--socket', str(root / 's'), '--auth-file', auth, 'agent', 'worktrees', 'exit', owner, params], cwd=root, env=env, capture_output=True, text=True, timeout=40, check=True)
                assert not json.loads(result.stdout)['is_error'], result.stdout
                assert rpc('agent.worktrees.status', {'session_id': owner})['active'] is False
                assert rpc('agent.sessions.get', {'session_id': owner})['message_count'] == 0
                assert not rpc('agent.worktrees.enter', {'session_id': owner, 'name': 'api'})['is_error']
                assert not rpc('agent.worktrees.exit', {'session_id': owner, 'action': 'remove', 'discard_changes': True})['is_error']
                assert not path.exists() and not git('branch', '--list', 'worktree-api')
                assert (work / 'marker.txt').read_text() == marker and not (work / 'new.txt').exists()
                report.update(http=True, ipc_cli=True, owner_isolation=True, dirty_removal_protection=True, native_shell_scope=True)

            mcp_args = ['--workspace', str(work), '--models', str(root / 'models'), '--model', 'model://missing-fixture',
                '--worktree-config', config, '--allow', 'EnterWorktree', '--allow', 'ExitWorktree', '--allow', 'Write', '--allow', 'Bash',
                '--no-apps', '--no-skills', '--no-subagents', '--no-agent-profiles']
            init_args = {'protocolVersion': '2025-11-25', 'capabilities': {}, 'clientInfo': {'name': 'worktree-verifier', 'version': '1'}}
            with server('mcp-http', [mcp, *mcp_args, '--state', str(root / 'mcp-http'), '--http-port', '0', '--credentials', credentials], r'http://127\.0\.0\.1:(\d+)/mcp') as port:
                status, init, session = post(port, '/mcp', 'initialize', init_args)
                assert status == 200 and session and 'iisacc/worktrees' in init['result']['capabilities']['experimental'], init
                post(port, '/mcp', 'notifications/initialized', {}, session=session)
                def call(name, values):
                    _, value, _ = post(port, '/mcp', 'tools/call', {'name': name, 'arguments': values}, session=session)
                    assert not value.get('error') and not value['result'].get('isError'), value
                    return value['result']['structuredContent']
                _, listed, _ = post(port, '/mcp', 'tools/list', {}, session=session)
                assert {'EnterWorktree', 'ExitWorktree', 'iiLocalLLM.agent.worktrees.status'} <= {t['name'] for t in listed['result']['tools']}
                path = Path(call('EnterWorktree', {'name': 'mcp-http'})['worktreePath'])
                call('Write', {'path': 'mcp.txt', 'content': marker})
                assert (path / 'mcp.txt').read_text() == marker and not (work / 'mcp.txt').exists()
                assert call('iiLocalLLM.agent.worktrees.status', {})['workingDirectory'] == str(path)
                assert post(port, '/mcp', 'tools/list', {}, other_token, session)[0] == 404
                call('ExitWorktree', {'action': 'keep'})
                assert path.exists()
                call('EnterWorktree', {'name': 'mcp-http'})
                call('ExitWorktree', {'action': 'remove', 'discard_changes': True})
                assert not path.exists()
                report['mcp_http'] = True

            if args.official_stdio:
                async def official():
                    from mcp import ClientSession, StdioServerParameters
                    from mcp.client.stdio import stdio_client
                    with args.report.with_suffix('.stdio.log').open('w') as error:
                        async with stdio_client(StdioServerParameters(command=mcp, args=[*mcp_args, '--state', str(root / 'mcp-stdio')], env=env, cwd=str(root)), errlog=error) as (reader, writer):
                            async with ClientSession(reader, writer) as client:
                                init = await client.initialize()
                                assert 'iisacc/worktrees' in init.capabilities.experimental
                                result = await client.call_tool('EnterWorktree', {'name': 'stdio'})
                                assert not result.isError, result
                                path = Path(result.structuredContent['worktreePath'])
                                result = await client.call_tool('Write', {'path': 'stdio.txt', 'content': marker})
                                assert not result.isError and (path / 'stdio.txt').read_text() == marker, result
                                result = await client.call_tool('ExitWorktree', {'action': 'remove', 'discard_changes': True})
                                assert not result.isError and not path.exists(), result
                        return True
                report['official_stdio'] = asyncio.run(official())
            report['passed'] = True
        finally:
            args.report.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report))


if __name__ == '__main__':
    main()
