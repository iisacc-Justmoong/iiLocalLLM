"""Qualify native notebook edits over real transports against Jupyter's schema.

Test-only dependencies: nbformat==5.11.1 and (for --official-stdio) mcp==1.26.0.
The production tools use C++/Qt and do not execute notebook code.
"""
from contextlib import contextmanager
from copy import deepcopy
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
import nbformat


def validate_notebook(value):
    original = deepcopy(value)
    # validate()/read() may normalize IDs. iter_validate explicitly avoids repair.
    errors = list(nbformat.validator.iter_validate(value, version=4, version_minor=value['nbformat_minor']))
    assert not errors, [str(error) for error in errors]
    if value['nbformat_minor'] >= 5:
        ids = [cell['id'] for cell in value['cells']]
        assert len(ids) == len(set(ids))
    assert original == value, 'The independent validator mutated its input'


def main():
    parser = argparse.ArgumentParser()
    for name in ('daemon', 'cli', 'mcp'):
        parser.add_argument(name, type=Path)
    parser.add_argument('--report', type=Path, required=True)
    parser.add_argument('--official-stdio', action='store_true')
    parser.add_argument('--checkpoints', action='store_true')
    args = parser.parse_args()
    daemon, cli, mcp = (str(getattr(args, key).resolve()) for key in ('daemon', 'cli', 'mcp'))
    env = dict(os.environ)
    for key in ('DYLD_LIBRARY_PATH', 'DYLD_FRAMEWORK_PATH', 'DYLD_FALLBACK_LIBRARY_PATH', 'LIBRARY_PATH'):
        env.pop(key, None)
    args.report.parent.mkdir(parents=True, exist_ok=True)
    report = {'passed': False, 'llm_inference': False, 'nbformat_version': nbformat.__version__, 'validated_documents': 0}
    with tempfile.TemporaryDirectory(prefix='notebook-wire-', dir=args.report.parent) as directory:
        root = Path(directory).resolve()
        work = root / 'work'
        work.mkdir()
        marker = 'wire_' + secrets.token_hex(6)
        notebook = {'nbformat': 4, 'nbformat_minor': 5, 'metadata': {'custom': marker}, 'cells': [
            {'cell_type': 'code', 'id': 'main', 'source': ['x = 0\n'], 'metadata': {'keep': True}, 'execution_count': 3,
                'outputs': [{'output_type': 'stream', 'name': 'stdout', 'text': 'old output'}]},
            {'cell_type': 'markdown', 'id': 'cell-0', 'source': '# Keep', 'metadata': {},
                'attachments': {'image': {'image/png': 'aGVsbG8='}}}]}
        path = work / 'book.ipynb'
        path.write_text(json.dumps(notebook))
        token, other_token = secrets.token_urlsafe(36), secrets.token_urlsafe(36)

        def verify():
            value = json.loads(path.read_text())
            validate_notebook(value)
            assert value['metadata']['custom'] == marker
            report['validated_documents'] += 1
            return value

        def private(name, value):
            target = root / name
            target.write_text(value if isinstance(value, str) else json.dumps(value))
            target.chmod(0o600)
            return str(target)

        credentials = private('credentials', {'society': token, 'dreamscapes': other_token})
        auth = private('token', token)

        @contextmanager
        def server(name, command, pattern):
            logpath = root / (name + '.log')
            with logpath.open('w') as log:
                process = subprocess.Popen(command, cwd=root, env=env, stdout=log, stderr=log)
                try:
                    deadline = time.monotonic() + 60
                    while True:
                        output = logpath.read_text()
                        assert process.poll() is None, output[-5000:]
                        found = re.search(pattern, output)
                        if found:
                            break
                        assert time.monotonic() < deadline, output[-5000:]
                        time.sleep(.02)
                    yield int(found[1])
                finally:
                    process.terminate()
                    try:
                        process.wait(timeout=15)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait()
                    args.report.with_suffix('.' + name + '.log').write_bytes(logpath.read_bytes())

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
                '--agent-allow', 'NotebookEdit', '--agent-no-worktrees', '--agent-no-apps', '--agent-no-skills', '--agent-no-subagents', '--no-agent-profiles']
            if args.checkpoints:
                command += ['--agent-allow', 'RewindFiles', '--agent-allow', 'Write']
            with server('api', command, r'iiLocalLLM HTTP: http://127\.0\.0\.1:(\d+)') as port:
                def rpc(method, params=None, credential=token, expected=200):
                    status, data, _ = post(port, '/v1/rpc', method, params or {}, credential)
                    assert status == expected, (status, data)
                    return data.get('result', data)
                assert rpc('agent.info')['notebooks_enabled']
                owner = rpc('agent.sessions.create', {'model': 'model://missing-fixture'})['session_id']
                before_api = path.read_bytes()
                if args.checkpoints:
                    assert rpc('agent.info')['file_checkpoints_enabled']
                    point = rpc('agent.checkpoints.create', {'session_id': owner})['message_id']
                observed = {'session_id': owner, 'notebook_path': 'book.ipynb'}
                edit = {**observed, 'cell_id': 'main', 'new_source': 'checked = ' + repr(marker)}
                assert rpc('agent.notebooks.edit', edit)['is_error']
                assert not rpc('agent.notebooks.read', observed)['is_error']
                result = rpc('agent.notebooks.edit', edit)
                assert not result['is_error'], result
                assert verify()['cells'][0]['source'] == edit['new_source']
                assert json.loads(Path(result['result']['backup_path']).read_text()) == notebook
                rpc('agent.notebooks.edit', edit, other_token, 404)
                rpc('agent.notebooks.read', observed, 'invalid', 401)
                path.write_text(path.read_text() + '\n')
                assert rpc('agent.notebooks.edit', edit)['is_error']
                assert not rpc('agent.notebooks.read', observed)['is_error']
                params = private('edit.json', {'notebook_path': 'book.ipynb', 'cell_id': 'main', 'cell_type': 'markdown', 'new_source': '# ' + marker})
                result = subprocess.run([cli, '--socket', str(root / 's'), '--auth-file', auth, 'agent', 'notebooks', 'edit', owner, params], cwd=root, env=env, capture_output=True, text=True, timeout=40, check=True)
                assert not json.loads(result.stdout)['is_error'], result.stdout
                assert verify()['cells'][0]['cell_type'] == 'markdown'
                assert rpc('agent.sessions.get', {'session_id': owner})['message_count'] == 0
                outside = root / 'outside.ipynb'
                outside.write_text(json.dumps(notebook))
                assert rpc('agent.notebooks.read', {**observed, 'notebook_path': str(outside)})['is_error']
                legacy = work / 'legacy.ipynb'
                legacy.write_text(json.dumps({'nbformat': 4, 'nbformat_minor': 4, 'metadata': {}, 'cells': [
                    {'cell_type': 'markdown', 'source': ['# Old'], 'metadata': {}, 'attachments': {'text': {'text/plain': 'preserve until conversion'}}}]}))
                legacy_args = {'session_id': owner, 'notebook_path': 'legacy.ipynb'}
                assert not rpc('agent.notebooks.read', legacy_args)['is_error']
                assert not rpc('agent.notebooks.edit', {**legacy_args, 'cell_id': 'cell-0', 'cell_type': 'code', 'new_source': 'x = 1'})['is_error']
                legacy_value = json.loads(legacy.read_text())
                validate_notebook(legacy_value)
                assert 'id' not in legacy_value['cells'][0] and 'attachments' not in legacy_value['cells'][0]
                assert not rpc('agent.notebooks.edit', {**legacy_args, 'cell_type': 'markdown', 'edit_mode': 'insert', 'new_source': '# First'})['is_error']
                legacy_value = json.loads(legacy.read_text())
                validate_notebook(legacy_value)
                assert legacy_value['cells'][0]['source'] == '# First' and all('id' not in cell for cell in legacy_value['cells'])
                report['validated_documents'] += 2
                report['legacy_notebook'] = legacy_value
                report.update(http=True, ipc_cli=True, owner_isolation=True, stale_read_rejected=True, outside_scope_rejected=True, transcript_unchanged=True)
                if args.checkpoints:
                    assert rpc('agent.checkpoints.list', {'session_id': owner})['snapshots']
                    rpc('agent.checkpoints.list', {'session_id': owner}, other_token, 404)
                    edited = path.read_bytes()
                    preview = rpc('agent.checkpoints.rewind', {'session_id': owner, 'message_id': point, 'dry_run': True})
                    assert not preview['is_error'] and path.read_bytes() == edited, preview
                    params = private('rewind.json', {'message_id': point})
                    output = subprocess.run([cli, '--socket', str(root / 's'), '--auth-file', auth, 'agent', 'checkpoints', 'rewind', owner, params], cwd=root, env=env, capture_output=True, text=True, timeout=40, check=True)
                    restored = json.loads(output.stdout)
                    assert not restored['is_error'] and restored['result']['complete'] and path.read_bytes() == before_api
                    assert rpc('agent.notebooks.edit', edit)['is_error'], 'Rewind must invalidate prior reads'
                    report['checkpoint_http_and_ipc'] = True

            mcp_args = ['--workspace', str(work), '--models', str(root / 'models'), '--model', 'model://missing-fixture',
                '--allow', 'NotebookEdit', '--no-worktrees', '--no-apps', '--no-skills', '--no-subagents', '--no-agent-profiles']
            init_args = {'protocolVersion': '2025-11-25', 'capabilities': {}, 'clientInfo': {'name': 'notebook-verifier', 'version': '1'}}
            if args.checkpoints:
                for rule in ('Write', 'RewindFiles'):
                    mcp_args += ['--allow', rule]
            with server('mcp-http', [mcp, *mcp_args, '--state', str(root / 'mcp-http'), '--http-port', '0', '--credentials', credentials], r'http://127\.0\.0\.1:(\d+)/mcp') as port:
                status, init, session = post(port, '/mcp', 'initialize', init_args)
                assert status == 200 and session and 'iisacc/notebooks' in init['result']['capabilities']['experimental'], init
                post(port, '/mcp', 'notifications/initialized', {}, session=session)
                def call(name, values, failure=False):
                    _, value, _ = post(port, '/mcp', 'tools/call', {'name': name, 'arguments': values}, session=session)
                    assert not value.get('error') and value['result'].get('isError', False) is failure, value
                    return value['result']['structuredContent']
                before_mcp = path.read_bytes()
                if args.checkpoints:
                    assert 'iisacc/fileCheckpoints' in init['result']['capabilities']['experimental']
                    point = call('iiLocalLLM.agent.checkpoints.create', {})['message_id']
                _, listed, _ = post(port, '/mcp', 'tools/list', {}, session=session)
                assert {'Read', 'NotebookEdit'} <= {tool['name'] for tool in listed['result']['tools']}
                edit = {'notebook_path': 'book.ipynb', 'new_source': 'x = 2', 'cell_id': 'main', 'cell_type': 'code'}
                call('NotebookEdit', edit, failure=True)
                call('Read', {'path': 'book.ipynb'})
                call('NotebookEdit', edit)
                assert verify()['cells'][0]['cell_type'] == 'code'
                inserted = call('NotebookEdit', {'notebook_path': 'book.ipynb', 'new_source': '# New', 'cell_type': 'markdown', 'edit_mode': 'insert', 'cell_id': 'main'})
                assert verify()['cells'][1]['id'] == inserted['cell_id']
                call('NotebookEdit', {'notebook_path': 'book.ipynb', 'new_source': '', 'edit_mode': 'delete', 'cell_id': inserted['cell_id']})
                assert len(verify()['cells']) == 2
                assert post(port, '/mcp', 'tools/list', {}, other_token, session)[0] == 404
                report['mcp_http'] = True
                if args.checkpoints:
                    preview = call('iiLocalLLM.agent.checkpoints.rewind', {'message_id': point, 'dry_run': True})
                    assert preview['dryRun'] and preview['filesChanged']
                    result = call('iiLocalLLM.agent.checkpoints.rewind', {'message_id': point})
                    assert result['complete'] and path.read_bytes() == before_mcp
                    report['checkpoint_mcp_http'] = True

            if args.official_stdio:
                async def official():
                    from mcp import ClientSession, StdioServerParameters
                    from mcp.client.stdio import stdio_client
                    with args.report.with_suffix('.stdio.log').open('w') as error:
                        async with stdio_client(StdioServerParameters(command=mcp, args=[*mcp_args, '--state', str(root / 'mcp-stdio')], env=env, cwd=str(root)), errlog=error) as (reader, writer):
                            async with ClientSession(reader, writer) as client:
                                init = await client.initialize()
                                assert 'iisacc/notebooks' in init.capabilities.experimental
                                before_stdio = path.read_bytes()
                                if args.checkpoints:
                                    assert 'iisacc/fileCheckpoints' in init.capabilities.experimental
                                    point = (await client.call_tool('iiLocalLLM.agent.checkpoints.create', {})).structuredContent['message_id']
                                result = await client.call_tool('Read', {'path': 'book.ipynb'})
                                assert not result.isError, result
                                result = await client.call_tool('NotebookEdit', {'notebook_path': 'book.ipynb', 'cell_id': 'cell-0', 'new_source': '# ' + marker})
                                assert not result.isError, result
                                assert result.structuredContent['cell_type'] == 'markdown'
                                assert verify()['cells'][1]['source'] == '# ' + marker
                                if args.checkpoints:
                                    restored = await client.call_tool('iiLocalLLM.agent.checkpoints.rewind', {'message_id': point})
                                    assert not restored.isError and restored.structuredContent['complete'] and path.read_bytes() == before_stdio
                                    report['checkpoint_official_stdio'] = True
                        return True
                report['official_stdio'] = asyncio.run(official())
                report['mcp_client_version'] = importlib.metadata.version('mcp')
            # Export the exact final document, never the validator's normalized copy.
            report['notebook'] = verify()
            report['passed'] = True
        finally:
            args.report.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report))


if __name__ == '__main__':
    main()
