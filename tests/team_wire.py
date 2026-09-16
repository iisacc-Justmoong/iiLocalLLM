"""Team controls through real HTTP, IPC/CLI, MCP HTTP and official MCP stdio.

This test intentionally uses an absent model and verifies explicit worker failure.
Native successful inference is qualified separately by team_runtime_smoke.cpp.
"""
from contextlib import contextmanager
from http.client import HTTPConnection
from pathlib import Path
import argparse
import asyncio
import importlib.metadata
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
    parser.add_argument('--report', required=True, type=Path)
    parser.add_argument('--official-stdio', action='store_true')
    args = parser.parse_args()
    daemon, cli, mcp = [str(getattr(args, k).resolve()) for k in ('daemon', 'cli', 'mcp')]
    env = dict(os.environ)
    for key in ('DYLD_LIBRARY_PATH', 'DYLD_FRAMEWORK_PATH', 'DYLD_FALLBACK_LIBRARY_PATH', 'LIBRARY_PATH'):
        env.pop(key, None)
    args.report.parent.mkdir(parents=True, exist_ok=True)
    report = {'passed': False, 'native_inference': False, 'binaries': [daemon, cli, mcp]}
    with tempfile.TemporaryDirectory(prefix='teams-wire-', dir=args.report.parent) as directory:
        root = Path(directory).resolve()
        work = root / 'work'
        work.mkdir()
        token, other = secrets.token_urlsafe(36), secrets.token_urlsafe(36)

        def private(name, value):
            path = root / name
            path.write_text(value if isinstance(value, str) else json.dumps(value))
            path.chmod(0o600)
            return str(path)

        credentials = private('credentials.json', {'society': token, 'dreamscapes': other})
        auth = private('token', token)

        @contextmanager
        def server(name, command, pattern):
            logpath = root / (name + '.log')
            with logpath.open('w') as log:
                process = subprocess.Popen(command, cwd=root, env=env, stdout=log, stderr=log)
                try:
                    deadline = time.monotonic() + 45
                    while True:
                        output = logpath.read_text()
                        assert process.poll() is None, output[-5000:]
                        match = re.search(pattern, output)
                        if match:
                            break
                        assert time.monotonic() < deadline, output[-5000:]
                        time.sleep(.02)
                    yield int(match[1])
                finally:
                    process.terminate()
                    try:
                        process.wait(timeout=15)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait()
                    args.report.with_suffix('.' + name + '.log').write_bytes(logpath.read_bytes())

        def post(port, endpoint, method, params, credential=token, session=None):
            payload = {'id': secrets.token_hex(6), 'method': method, 'params': params}
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
                    data = next(v for v in frames if v.get('id') == payload['id'] and ('result' in v or 'error' in v))
                else:
                    data = json.loads(body) if body else {}
                return response.status, data, response.getheader('Mcp-Session-Id')
            finally:
                connection.close()

        try:
            command = [daemon, '--socket', str(root / 's'), '--http-port', '0', '--models-root', str(root / 'models'),
                '--agent-workspace', str(work), '--agent-state', str(root / 'api'), '--agent-credentials', credentials,
                '--agent-allow', '*', '--agent-no-worktrees', '--agent-no-apps', '--agent-no-skills', '--no-agent-profiles']
            with server('api', command, r'iiLocalLLM HTTP: http://127\.0\.0\.1:(\d+)') as port:
                def rpc(method, params=None):
                    status, value, _ = post(port, '/v1/rpc', method, params or {})
                    assert status == 200 and not value.get('error'), (status, value)
                    return value['result']
                assert rpc('agent.info')['teams_enabled']
                assert rpc('agent.info')['team_auto_task_claim_enabled']
                owner = rpc('agent.sessions.create', {'model': 'model://absent-team-fixture'})['session_id']
                created = rpc('agent.teams.create', {'session_id': owner, 'team_name': 'wire'})
                assert not created['is_error'] and created['result']['team_name'] == 'wire'
                assert rpc('agent.teams.status', {'session_id': owner})['result']['team']['auto_task_claim_enabled']
                for extra in ({}, {'summary': ''}):
                    invalid = rpc('agent.teams.send', {'session_id': owner, 'to': 'team-lead', 'message': 'INVALID_SUMMARY_MARKER', **extra})
                    assert invalid['is_error'], invalid
                assert all(v['message'] != 'INVALID_SUMMARY_MARKER' for v in rpc('agent.teams.inbox', {'session_id': owner})['result']['messages'])
                report['http_conditional_summary'] = True
                assert post(port, '/v1/rpc', 'agent.teams.status', {'session_id': owner}, other)[0] != 200
                task = rpc('agent.tasks.create', {'session_id': owner, 'subject': 'Shared wire task', 'description': 'Transport proof'})
                assert not task['is_error']
                params = private('send.json', {'to': 'team-lead', 'summary': 'IPC route', 'message': 'IPC_TEAM_MARKER'})
                output = subprocess.run([cli, '--socket', str(root / 's'), '--auth-file', auth, 'agent', 'teams', 'send', owner, params], cwd=root, env=env, capture_output=True, text=True, timeout=40, check=True)
                sent = json.loads(output.stdout)
                assert not sent['is_error'] and sent['result']['success'], sent
                inbox = rpc('agent.teams.inbox', {'session_id': owner})['result']['messages']
                assert any(v['message'] == 'IPC_TEAM_MARKER' for v in inbox)
                started = rpc('agent.teams.spawn', {'session_id': owner, 'name': 'reader', 'prompt': 'Report your result'})
                assert not started['is_error'] and started['result']['status'] == 'async_launched'
                waited = rpc('agent.teams.wait', {'session_id': owner, 'timeout_ms': 10000})['result']
                assert waited['idle'] and any(m['name'] == 'reader' and m['status'] == 'failed' for m in waited['team']['members']), waited
                assert not rpc('agent.teams.delete', {'session_id': owner})['is_error']
                assert rpc('agent.tasks.list', {'session_id': owner})['result']['tasks'] == []
                report.update(http=True, ipc_cli=True, worker_failure_reported=True, authenticated_isolation=True)

            with server('api-no-claim', [*command, '--agent-no-team-task-claim'], r'iiLocalLLM HTTP: http://127\.0\.0\.1:(\d+)') as port:
                status, disabled, _ = post(port, '/v1/rpc', 'agent.info', {})
                assert status == 200 and disabled['result']['teams_enabled'] and not disabled['result']['team_auto_task_claim_enabled'], disabled
                report['daemon_task_claim_opt_out'] = True

            common = ['--workspace', str(work), '--models', str(root / 'models'), '--model', 'model://absent-team-fixture',
                '--allow', '*', '--no-worktrees', '--no-apps', '--no-skills', '--no-agent-profiles']
            init_args = {'protocolVersion': '2025-11-25', 'capabilities': {}, 'clientInfo': {'name': 'team-verifier', 'version': '1'}}
            with server('mcp-http', [mcp, *common, '--state', str(root / 'mcp-http'), '--http-port', '0', '--credentials', credentials], r'http://127\.0\.0\.1:(\d+)/mcp') as port:
                status, init, session = post(port, '/mcp', 'initialize', init_args)
                assert status == 200 and session and 'iisacc/teams' in init['result']['capabilities']['experimental'], init
                post(port, '/mcp', 'notifications/initialized', {}, session=session)

                def call(name, values, failure=False):
                    _, value, _ = post(port, '/mcp', 'tools/call', {'name': name, 'arguments': values}, session=session)
                    assert not value.get('error') and value['result'].get('isError', False) is failure, value
                    return value['result'].get('structuredContent', {})

                _, listed, _ = post(port, '/mcp', 'tools/list', {}, session=session)
                team_tools = [t for t in listed['result']['tools'] if t['name'].startswith('iiLocalLLM.agent.teams.')]
                assert len(team_tools) == 8 and all('session_id' not in t['inputSchema']['properties'] for t in team_tools)
                assert call('iiLocalLLM.agent.teams.create', {'team_name': 'mcp'})['team_name'] == 'mcp'
                assert call('iiLocalLLM.agent.teams.status', {})['team']['auto_task_claim_enabled']
                call('iiLocalLLM.agent.teams.send', {'to': 'team-lead', 'message': 'INVALID_SUMMARY_MARKER'}, failure=True)
                call('iiLocalLLM.agent.teams.send', {'to': 'team-lead', 'summary': 'MCP transport', 'message': 'MCP_TEAM_MARKER'})
                _, controlled, _ = post(port, '/mcp', 'iisacc/teams/inbox', {}, session=session)
                assert any(v['message'] == 'MCP_TEAM_MARKER' for v in controlled['result']['structuredContent']['messages'])
                call('iiLocalLLM.agent.teams.status', {'session_id': 'forged'}, failure=True)
                assert post(port, '/mcp', 'iisacc/teams/status', {}, other, session)[0] == 404
                call('iiLocalLLM.agent.teams.delete', {})
                report['mcp_http'] = True
                report['mcp_control_path'] = True
            if args.official_stdio:
                from jsonschema import Draft202012Validator
                from mcp import ClientSession, StdioServerParameters
                from mcp.client.stdio import stdio_client

                async def official():
                    parameters = StdioServerParameters(command=mcp, args=[*common, '--state', str(root / 'mcp-stdio'), '--no-team-task-claim'], env=env, cwd=str(root))
                    with (root / 'stdio.log').open('w') as log:
                        async with stdio_client(parameters, errlog=log) as (read, write):
                            async with ClientSession(read, write) as client:
                                init = await client.initialize()
                                assert 'iisacc/teams' in init.capabilities.experimental
                                listed = await client.list_tools()
                                schema = next(t.inputSchema for t in listed.tools if t.name == 'iiLocalLLM.agent.teams.send')
                                Draft202012Validator.check_schema(schema)
                                validator = Draft202012Validator(schema)
                                plain = {'to': 'team-lead', 'message': 'Observed'}
                                assert not validator.is_valid(plain)
                                assert not validator.is_valid({**plain, 'summary': ''})
                                assert validator.is_valid({**plain, 'summary': 'File observed'})
                                assert validator.is_valid({'to': 'worker', 'message': {'type': 'shutdown_request'}})
                                for protocol in (
                                    {'type': 'shutdown_request', 'request_id': 'invented'},
                                    {'type': 'shutdown_request', 'approve': True},
                                    {'type': 'shutdown_response', 'request_id': 'invented', 'approve': True},
                                ):
                                    assert not validator.is_valid({'to': 'worker', 'message': protocol})
                                report['official_conditional_schema'] = True
                                report['official_leader_protocol_schema'] = True
                                created = await client.call_tool('iiLocalLLM.agent.teams.create', {'team_name': 'stdio'})
                                assert not created.isError and created.structuredContent['team_name'] == 'stdio'
                                status = await client.call_tool('iiLocalLLM.agent.teams.status', {})
                                assert not status.isError and not status.structuredContent['team']['auto_task_claim_enabled']
                                report['mcp_task_claim_opt_out'] = True
                                sent = await client.call_tool('iiLocalLLM.agent.teams.send', {'to': 'team-lead', 'summary': 'Official client', 'message': 'STDIO_TEAM_MARKER'})
                                assert not sent.isError and sent.structuredContent['success']
                                inbox = await client.call_tool('iiLocalLLM.agent.teams.inbox', {})
                                assert any(v['message'] == 'STDIO_TEAM_MARKER' for v in inbox.structuredContent['messages'])
                                deleted = await client.call_tool('iiLocalLLM.agent.teams.delete', {})
                                assert not deleted.isError
                    args.report.with_suffix('.stdio.log').write_bytes((root / 'stdio.log').read_bytes())
                    return True
                report['official_stdio'] = asyncio.run(official())
                report['mcp_client_version'] = importlib.metadata.version('mcp')
            report['passed'] = True
        finally:
            args.report.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report))


if __name__ == '__main__':
    main()
