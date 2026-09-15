"""Actual authenticated daemon HTTP/IPC and MCP HTTP question/answer exchange."""
from concurrent.futures import ThreadPoolExecutor
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
    parser.add_argument('--report', type=Path)
    parser.add_argument('--official-stdio', action='store_true')
    args = parser.parse_args()
    binaries = [str(getattr(args, name).resolve()) for name in ('daemon', 'cli', 'mcp')]
    daemon, cli, mcp = binaries
    report = {'passed': False, 'binaries': binaries}
    env = dict(os.environ)
    for name in ('DYLD_LIBRARY_PATH', 'DYLD_FRAMEWORK_PATH', 'DYLD_FALLBACK_LIBRARY_PATH', 'LIBRARY_PATH'):
        env.pop(name, None)
    question = {'questions': [{'question': 'Which platforms?', 'header': 'Platforms', 'multiSelect': True,
        'options': [{'label': 'Qt', 'description': 'Desktop', 'preview': '**Qt**'}, {'label': 'Web', 'description': 'Browser'}]}],
        'metadata': {'source': 'society.preferences'}}
    answered = dict(question, answers={'Which platforms?': 'Qt, Web, custom'},
        annotations={'Which platforms?': {'notes': '사용자 선택', 'preview': '**Selected**'}})
    decision = {'behavior': 'allow', 'updatedInput': answered}
    token, other = secrets.token_urlsafe(36), secrets.token_urlsafe(36)
    with tempfile.TemporaryDirectory(prefix='question-') as directory:
        root = Path(directory)
        work = root / 'work'
        work.mkdir()

        def private(name, value):
            path = root / name
            path.write_text(value if isinstance(value, str) else json.dumps(value))
            path.chmod(0o600)
            return str(path)

        credentials = private('credentials', {'society': token, 'dreamscapes': other})
        auth = private('auth', token)
        limits = private('limits', {'timeout_ms': 30000})

        def until(check, pending=None):
            deadline = time.monotonic() + 20
            while True:
                value = check()
                if value:
                    return value
                if pending and pending.done():
                    raise AssertionError(pending.result())
                assert time.monotonic() < deadline, 'Question wire deadline expired'
                time.sleep(.02)

        @contextmanager
        def server(name, command, pattern):
            path = root / (name + '.log')
            with path.open('w') as log:
                process = subprocess.Popen(command, stdout=log, stderr=log, env=env)
                try:
                    def ready():
                        text = path.read_text()
                        assert process.poll() is None, text[-3000:]
                        return re.search(pattern, text)
                    yield int(until(ready)[1])
                finally:
                    process.terminate()
                    try:
                        process.wait(timeout=10)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait()
                    if args.report:
                        args.report.with_suffix('.' + name + '.log').write_bytes(path.read_bytes())

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
                    value = next(v for v in frames if v.get('id') == body['id'] and ('result' in v or 'error' in v))
                else:
                    value = json.loads(raw) if raw else {}
                return response.status, value, response.getheader('Mcp-Session-Id')
            finally:
                connection.close()

        command = [daemon, '--socket', str(root / 's'), '--http-port', '0', '--http-workers', '1', '--http-control-requests', '1',
            '--models-root', str(root / 'models'), '--agent-workspace', str(work), '--agent-state', str(root / 'api'),
            '--agent-credentials', credentials, '--agent-permission-requests', limits, '--agent-no-apps', '--agent-no-background',
            '--agent-no-skills', '--agent-no-subagents', '--no-agent-profiles']
        with server('api', command, r'iiLocalLLM HTTP: http://127\.0\.0\.1:(\d+)') as port:
            def rpc(method, params=None, credential=token, expected=200):
                status, value, _ = post(port, '/v1/rpc', method, params or {}, credential)
                assert status == expected, (status, value)
                return value.get('result', value)
            assert rpc('agent.info')['user_questions_enabled']
            owner = rpc('agent.sessions.create', {'model': 'model://fixture'})['session_id']
            rpc('agent.questions.ask', dict(question, session_id=owner), other, 404)
            rpc('agent.questions.ask', dict(question, session_id=owner), 'invalid', 401)
            assert rpc('agent.questions.ask', dict(answered, session_id=owner))['is_error']
            with ThreadPoolExecutor(1) as pool:
                pending = pool.submit(rpc, 'agent.questions.ask', dict(question, session_id=owner))
                prompts = until(lambda: rpc('agent.permissions.pending')['requests'], pending)
                prompt = prompts[0]
                assert prompt['session_id'] == owner and prompt['request']['permission_preview']['_meta']['user_question']['input'] == question
                assert not rpc('agent.permissions.pending', credential=other)['requests']
                payload = {'request_id': prompt['request_id'], 'decision': decision}
                rpc('agent.permissions.respond', payload, other, 404)
                path = private('answer', payload)
                response = subprocess.run([cli, '--socket', str(root / 's'), '--auth-file', auth, 'rpc', 'agent.permissions.respond', path],
                    text=True, capture_output=True, timeout=15, env=env)
                assert response.returncode == 0 and json.loads(response.stdout)['accepted'], response.stderr
                result = pending.result(timeout=15)
                assert not result['is_error'] and result['result']['answers'] == answered['answers'], result
                assert result['result']['annotations'] == answered['annotations']
                assert rpc('agent.permissions.respond', payload)['accepted']
                rpc('agent.permissions.respond', dict(payload, decision={'behavior': 'deny'}), expected=409)
            report['api_ipc'] = {'passed': True, 'host_answers': True, 'annotations': True, 'owner_isolation': True, 'reserved_control': True, 'replay': True}

        command = [mcp, '--workspace', str(work), '--model', 'model://fixture', '--models', str(root / 'models'), '--state', str(root / 'mcp'),
            '--http-port', '0', '--credentials', credentials, '--permission-requests', limits, '--max-streams', '1', '--max-control-streams', '1',
            '--no-apps', '--no-background', '--no-agent-profiles']
        with server('mcp', command, r'http://127\.0\.0\.1:(\d+)/mcp') as port:
            def initialize(credential):
                status, value, identity = post(port, '/mcp', 'initialize', {'protocolVersion': '2025-11-25', 'capabilities': {}, 'clientInfo': {'name': 'question-wire', 'version': '1'}}, credential)
                assert status == 200 and identity, value
                assert post(port, '/mcp', 'notifications/initialized', {}, credential, identity)[0] == 202
                return value['result'], identity
            init, identity = initialize(token)
            _, foreign = initialize(other)
            capability = init['capabilities']['experimental']['iisacc/userQuestions']
            assert capability['tool'] == 'AskUserQuestion' and capability['permissionRequests'] and capability['previewFormat'] == 'markdown'
            def rpc(method, params=None, foreign_owner=False):
                status, value, _ = post(port, '/mcp', method, params or {}, other if foreign_owner else token, foreign if foreign_owner else identity)
                assert status == 200, (status, value)
                return value
            tools = rpc('tools/list')['result']['tools']
            definition = next(t for t in tools if t['name'] == 'AskUserQuestion')
            assert definition['annotations']['readOnlyHint'] and 'answers' in definition['outputSchema']['properties']
            with ThreadPoolExecutor(1) as pool:
                pending = pool.submit(rpc, 'tools/call', {'name': 'AskUserQuestion', 'arguments': question})
                prompts = until(lambda: rpc('iisacc/permissions/pending')['result']['requests'], pending)
                prompt = prompts[0]
                assert not rpc('iisacc/permissions/pending', foreign_owner=True)['result']['requests']
                payload = {'request_id': prompt['request_id'], 'decision': decision}
                assert 'error' in rpc('iisacc/permissions/respond', payload, True)
                assert rpc('iisacc/permissions/respond', payload)['result']['accepted']
                result = pending.result(timeout=15)['result']
                assert not result['isError'] and result['structuredContent']['answers'] == answered['answers'], result
                assert result['structuredContent']['annotations'] == answered['annotations']
            report['mcp_http'] = {'passed': True, 'capability': capability, 'owner_isolation': True, 'reserved_control': True, 'host_answers': True}
        if args.official_stdio:
            from mcp import ClientSession, StdioServerParameters, types
            from mcp.client.stdio import stdio_client

            class ControlParams(types.RequestParams):
                model_config = {'extra': 'allow'}

            async def official():
                parameters = StdioServerParameters(command=mcp, args=['--workspace', str(work), '--model', 'model://fixture',
                    '--models', str(root / 'models'), '--state', str(root / 'stdio'), '--permission-requests', limits,
                    '--no-apps', '--no-background', '--no-agent-profiles'], env=env)
                async with stdio_client(parameters) as (reader, writer):
                    async with ClientSession(reader, writer) as session:
                        async with asyncio.timeout(30):
                            initialized = await session.initialize()
                        assert initialized.capabilities.experimental['iisacc/userQuestions']['tool'] == 'AskUserQuestion'

                        async def control(method, body):
                            result = await session.send_request(types.Request(method=method, params=ControlParams(**body)), types.Result)
                            return result.model_dump(by_alias=True, exclude_none=True)

                        pending = asyncio.create_task(session.call_tool('AskUserQuestion', question))
                        try:
                            async with asyncio.timeout(20):
                                while True:
                                    prompts = (await control('iisacc/permissions/pending', {}))['requests']
                                    if prompts:
                                        break
                                    assert not pending.done(), pending.result()
                                    await asyncio.sleep(.01)
                                payload = {'request_id': prompts[0]['request_id'], 'decision': decision}
                                assert (await control('iisacc/permissions/respond', payload))['accepted']
                                result = await pending
                                assert not result.isError and result.structuredContent['answers'] == answered['answers']
                        finally:
                            if not pending.done():
                                pending.cancel()
                                await asyncio.gather(pending, return_exceptions=True)
            asyncio.run(official())
            report['official_stdio'] = {'passed': True, 'python_mcp': '1.26.0', 'host_answers': True}
    report['passed'] = True
    if args.report:
        args.report.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report))


if __name__ == '__main__':
    main()
