"""Authenticated API/IPC and MCP planning review using real executables."""
from concurrent.futures import ThreadPoolExecutor
from contextlib import contextmanager
from http.client import HTTPConnection
from pathlib import Path
import argparse
import json
import os
import re
import secrets
import subprocess
import tempfile
import time


def wait_for(check, task=None):
    deadline = time.monotonic() + 15
    while True:
        value = check()
        if value:
            return value
        if task and task.done():
            raise AssertionError(task.result())
        assert time.monotonic() < deadline, 'Plan wire deadline expired'
        time.sleep(.02)


def main():
    parser = argparse.ArgumentParser()
    for name in ('daemon', 'cli', 'mcp'):
        parser.add_argument(name, type=Path)
    parser.add_argument('--report', type=Path)
    args = parser.parse_args()
    daemon, cli, mcp = [str(getattr(args, key).resolve()) for key in ('daemon', 'cli', 'mcp')]
    env = dict(os.environ)
    for key in ('DYLD_LIBRARY_PATH', 'DYLD_FRAMEWORK_PATH', 'DYLD_FALLBACK_LIBRARY_PATH', 'LIBRARY_PATH'):
        env.pop(key, None)
    report = {'passed': False, 'binaries': [daemon, cli, mcp]}
    with tempfile.TemporaryDirectory(prefix='plan-') as directory:
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
        requests = private('requests', {'timeout_ms': 30000})

        @contextmanager
        def server(name, command, pattern):
            log_path = root / (name + '.log')
            with log_path.open('w') as log:
                process = subprocess.Popen(command, stdout=log, stderr=log, env=env)
                try:
                    def ready():
                        text = log_path.read_text()
                        assert process.poll() is None, text[-2500:]
                        return re.search(pattern, text)
                    yield int(wait_for(ready)[1])
                finally:
                    process.terminate()
                    try:
                        process.wait(timeout=10)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait()
                    if args.report:
                        args.report.with_suffix('.' + name + '.log').write_bytes(log_path.read_bytes())

        def post(port, path, method, params, credential=token, identity=None):
            body = {'id': secrets.token_hex(8), 'method': method, 'params': params}
            headers = {'Content-Type': 'application/json', 'Accept': 'application/json, text/event-stream', 'Authorization': 'Bearer ' + credential}
            if path == '/mcp':
                body['jsonrpc'] = '2.0'
                if method.startswith('notifications/'):
                    body.pop('id')
                if identity:
                    headers.update({'Mcp-Session-Id': identity, 'Mcp-Protocol-Version': '2025-11-25'})
            connection = HTTPConnection('127.0.0.1', port, timeout=35)
            try:
                connection.request('POST', path, json.dumps(body), headers)
                response = connection.getresponse()
                raw = response.read()
                if response.getheader('Content-Type', '').startswith('text/event-stream'):
                    frames = [json.loads(line[5:]) for line in raw.splitlines() if line.startswith(b'data:') and line[5:].strip()]
                    data = next(v for v in frames if v.get('id') == body['id'] and ('result' in v or 'error' in v))
                else:
                    data = json.loads(raw) if raw else {}
                return response.status, data, response.getheader('Mcp-Session-Id')
            finally:
                connection.close()

        command = [daemon, '--socket', str(root / 's'), '--http-port', '0', '--http-workers', '1', '--http-control-requests', '1',
            '--models-root', str(root / 'models'), '--agent-workspace', str(work), '--agent-state', str(root / 'api-state'),
            '--agent-credentials', credentials, '--agent-permission-requests', requests, '--agent-no-apps', '--agent-no-background',
            '--agent-no-skills', '--agent-no-subagents', '--no-agent-profiles']
        with server('api', command, r'iiLocalLLM HTTP: http://127\.0\.0\.1:(\d+)') as port:
            def rpc(method, params, credential=token, expected=200):
                status, result, _ = post(port, '/v1/rpc', method, params, credential)
                assert status == expected, (status, result)
                return result.get('result', result)
            assert rpc('agent.info', {})['plan_tools_enabled']
            owner = rpc('agent.sessions.create', {'model': 'model://fixture'})['session_id']
            enter = rpc('agent.plan.enter', {'session_id': owner})
            assert not enter['is_error'], enter
            path = Path(enter['result']['plan_file_path'])
            path.write_text('API original plan for host review')
            rpc('agent.plan.get', {'session_id': owner}, other, 404)
            rpc('agent.plan.get', {'session_id': owner}, 'invalid', 401)
            with ThreadPoolExecutor(1) as pool:
                pending = pool.submit(rpc, 'agent.plan.exit', {'session_id': owner})
                prompts = wait_for(lambda: rpc('agent.permissions.pending', {})['requests'], pending)
                assert len(prompts) == 1
                prompt = prompts[0]
                assert prompt['request']['permission_preview']['_meta']['plan']['plan'] == path.read_text(), prompt
                # The only ordinary HTTP worker is occupied by the review.
                assert rpc('agent.plan.get', {'session_id': owner})['phase'] == 'planning'
                decision = private('decision', {'request_id': prompt['request_id'], 'decision': {'behavior': 'allow', 'updatedInput': {'plan': 'API_HOST_EDIT'}}})
                accepted = subprocess.run([cli, '--socket', str(root / 's'), '--auth-file', auth, 'rpc', 'agent.permissions.respond', decision], env=env, text=True, capture_output=True, timeout=10)
                assert accepted.returncode == 0 and json.loads(accepted.stdout)['accepted'], accepted.stderr
                result = pending.result(timeout=10)
                assert not result['is_error'] and result['result']['plan_was_edited'], result
            state = rpc('agent.plan.get', {'session_id': owner})
            assert state['phase'] == 'approved' and state['approval_current'] and path.read_text() == 'API_HOST_EDIT'
            fork = rpc('agent.sessions.fork', {'session_id': owner})['session_id']
            copied = rpc('agent.plan.get', {'session_id': fork})
            assert copied['phase'] == 'planning' and copied['plan'] == 'API_HOST_EDIT' and copied['plan_file_path'] != str(path)
            report['api_ipc'] = {'review_preview': True, 'host_edit': True, 'owner_isolation': True, 'reserved_status': True, 'fork_review_required': True}

        command = [mcp, '--workspace', str(work), '--model', 'model://fixture', '--models', str(root / 'models'), '--state', str(root / 'mcp-state'),
            '--http-port', '0', '--credentials', credentials, '--permission-requests', requests, '--max-streams', '1', '--max-control-streams', '1',
            '--no-apps', '--no-background', '--no-agent-profiles', '--allow', 'Write']
        with server('mcp', command, r'http://127\.0\.0\.1:(\d+)/mcp') as port:
            def initialize(credential):
                status, value, identity = post(port, '/mcp', 'initialize', {'protocolVersion': '2025-11-25', 'capabilities': {}, 'clientInfo': {'name': 'plan-wire', 'version': '1'}}, credential)
                assert status == 200 and identity, value
                notified, _, _ = post(port, '/mcp', 'notifications/initialized', {}, credential, identity)
                assert notified == 202
                return value['result'], identity
            init, identity = initialize(token)
            _, foreign = initialize(other)
            assert init['capabilities']['experimental']['iisacc/planMode']['statusMethod'] == 'iisacc/plan/status'
            def rpc(method, params=None, foreign_owner=False):
                status, value, _ = post(port, '/mcp', method, params or {}, other if foreign_owner else token, foreign if foreign_owner else identity)
                assert status == 200, (status, value)
                return value
            def tool(name, arguments=None, foreign_owner=False):
                return rpc('tools/call', {'name': name, 'arguments': arguments or {}}, foreign_owner)['result']
            names = [v['name'] for v in rpc('tools/list')['result']['tools']]
            assert 'EnterPlanMode' in names and 'ExitPlanMode' in names and 'iisacc/plan/status' not in names
            enter = tool('EnterPlanMode')
            assert not enter['isError'], enter
            path = Path(enter['structuredContent']['plan_file_path'])
            assert not tool('Write', {'path': str(path), 'content': 'MCP review baseline'})['isError']
            assert tool('Write', {'path': 'forbidden.txt', 'content': 'blocked'})['isError']
            assert tool('Read', {'path': str(path)}, True)['isError']
            with ThreadPoolExecutor(1) as pool:
                for stale in (True, False):
                    pending = pool.submit(tool, 'ExitPlanMode')
                    prompt = wait_for(lambda: rpc('iisacc/permissions/pending')['result']['requests'], pending)[0]
                    assert rpc('iisacc/plan/status')['result']['phase'] == 'planning'
                    assert not rpc('iisacc/permissions/pending', foreign_owner=True)['result']['requests']
                    decision = {'request_id': prompt['request_id'], 'decision': {'behavior': 'allow'}}
                    assert 'error' in rpc('iisacc/permissions/respond', decision, True)
                    if stale:
                        path.write_text('MCP plan changed while pending')
                    else:
                        decision['decision']['updatedInput'] = {'plan': 'MCP_HOST_EDIT'}
                    assert rpc('iisacc/permissions/respond', decision)['result']['accepted']
                    result = pending.result(timeout=10)
                    assert bool(result['isError']) == stale, result
            state = rpc('iisacc/plan/status')['result']
            assert state['approval_current'] and path.read_text() == 'MCP_HOST_EDIT'
            assert not tool('Write', {'path': 'after.txt', 'content': 'APPROVED'})['isError']
            assert 'error' in rpc('iisacc/plan/status', {'session_id': state['session_id']})
            assert not (work / 'forbidden.txt').exists() and (work / 'after.txt').read_text() == 'APPROVED'
            report['mcp_http'] = {'native_tool_names': True, 'owned_file_write': True, 'stale_review_rejected': True, 'host_edit': True,
                'reserved_status': True, 'foreign_prompt_and_file_rejected': True, 'post_approval_policy': True}
    report['passed'] = True
    if args.report:
        args.report.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report))


if __name__ == '__main__':
    main()
