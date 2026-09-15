# 에이전트 HTTP·native IPC API

0.25.0은 ApiOptions.engine의 PermissionRequest 훅·permissionResponse·permissionUpdates를 모델 호출과 native 제어 도구에 적용한다. API 요청 본문으로 승인 콜백이나 갱신 권한을 등록하지 않는다. [PermissionRequest.md](PermissionRequest.md)를 따른다.


0.24.0의 `agent.sessions.clear`는 `{session_id}`로 소유 세션을 비우고 새 ID·완료 여부·진단을 반환한다. 새 세션 상한을 사전 예약하고 백그라운드 작업/알림을 보존한다. [SessionClear.md](SessionClear.md)를 따른다.

0.23.0의 `agent.sessions.end`는 `{session_id, reason?}`을 받아 소유한 세션의 접수 요청을 취소·정리하고 `{session_id, reason, ended, timed_out, diagnostics}`를 반환한다. reason 기본값은 other이다. 제어 worker를 사용하며 원문과 큐를 보존한다. 다음 명시적 실행은 resume한다. API close도 활성 세션을 정리한다. 상세 수명과 취소 계약은 [SessionEnd.md](SessionEnd.md)를 따른다.

0.20.0의 `agent.permissions.get`은 호스트가 선택한 `working_directories`와 출처별
`additional_directories`도 반환한다. 원격 입력으로 경로 권한을 추가하지 않는다.
실행·자식·철회·비공개 경로 계약은 [WorkingDirectories.md](WorkingDirectories.md)를 따른다.

`agent::Api`는 C++ `Engine`을 HTTP와 native IPC에서 함께 제공한다. 같은 인증키로 접속한 앱은 두 전송에서 같은 영속 세션을 사용한다. 앱마다 별도의 세션 저장소를 두며, 다른 앱의 세션 및 진행 중 요청 ID는 조회·취소할 수 없다. 프로토콜 식별자는 `iisacc.agent/1`이다. MCP JSON-RPC나 OpenAI Chat Completions와는 별도의 iiLocalLLM RPC 계약이다.

현재 API는 인증, 영속 대화, 연결이 유지되는 에이전트 실행·이벤트·취소, 작업·Todo 상태, 백그라운드 셸과 영속 입력 큐 제어를 제공한다. **전체 Claude 하네스 호환이나 모든 제품 연동의 완료를 뜻하지 않는다.** 백그라운드 에이전트 실행, 자동 유휴 기동·완료 알림, 파일 rewind, artifact 복제 등은 대응표의 미완료 항목이다.

## daemon 실행

먼저 툴에 공개할 `workspace`와 그 밖의 `private` 디렉터리를 준비한다. 호스트가 `private/clients.json`에 앱별로 생성한 서로 다른 난수 키를 저장한다. 값은 32~256자의 URL-safe 영문·숫자·`_`·`-`이다. 예를 들어 Python 표준 라이브러리 `secrets.token_urlsafe(36)`으로 키를 만든다. 파일에는 다음 형태의 JSON 객체를 넣는다. 아래 설명용 값은 실제 키가 아니다.

```json
{"society":"SOCIETY_RANDOM_TOKEN","dreamscapes":"DREAMSCAPES_RANDOM_TOKEN"}
```

```sh
chmod 600 private/clients.json
iiLocalLLMD --models-root Models --context-tokens 4096 \
  --socket private/llm.sock --http-port 50891 \
  --agent-workspace workspace --agent-state private/agent \
  --agent-credentials private/clients.json
```

세 가지 `--agent-*` 필수 옵션을 함께 지정해야 활성화된다. 기본 정책은 읽기 전용 도구를 허용하며 다른 도구의 확인 요청을 거부한다. 파일 수정 등 필요한 도구는 `--agent-allow Write --agent-allow Edit`처럼 호스트가 지정한다. 반복 옵션은 와일드카드도 받는다. `Bash`를 허용하면 명령은 호스트의 OS 권한으로 실행된다. 이 정책은 OS 샌드박스가 아니다.

키 파일은 workspace 밖의 일반 파일이어야 한다. Unix에서는 현재 사용자 소유이고 그룹·기타 사용자 권한이 없어야 하며, symlink 파일은 거부한다. Windows에서는 이 Unix 권한 검사 대신 호스트가 파일 ACL을 관리해야 한다. `agent::Api`는 키의 SHA-256 해시로 인증하고 키를 모델 입력이나 이벤트에 넣지 않는다. 같은 OS 사용자 권한의 악성 프로세스로부터 키 파일을 격리하는 기능은 아니다.

서버는 고정된 canonical workspace를 사용한다. 클라이언트는 요청으로 작업 경로를 바꿀 수 없다. state와 workspace는 서로 부모·자식 관계일 수 없으며 `/`를 workspace로 지정할 수도 없다. 상태 디렉터리는 소유자 전용 권한으로 생성·설정하고 프로세스 소유 잠금을 둔다. `<state>/<SHA256(client_id)>/sessions/<uuid>/transcript.jsonl`에 기록하므로 인증키가 바뀌어도 client ID를 유지하면 기록이 이어진다. client ID가 바뀌면 다른 저장소를 사용한다.

키 파일의 경로·권한·JSON 검사는 Service의 GPU 초기화 전에 수행한다. 유효한 설정으로 시작하면 기존 하드웨어 검사가 Metal 셰이더를 컴파일할 수 있다. 설치본에서 약 21초의 시작 지연을 관측했으므로 호스트는 listening 출력으로 준비 완료를 확인한다. 시험 스크립트는 시작 대기 60초와 RPC 응답 대기를 분리하고 실제 시작 시간을 기록한다. 이는 API 요청 기한을 늘리거나 드라이버 초기화가 항상 60초 안에 끝남을 보장하는 설정이 아니다.

기본 모델 관리·Chat Completions의 기존 인증 계약은 유지된다. 이 키 검사는 새 에이전트 RPC에 적용된다. HTTP는 계속 `127.0.0.1`에만 열리고 기존 Host/Origin 검사를 적용하며, native IPC는 현재 사용자 전용 소켓이다.

## 메서드

모든 메서드는 알 수 없는 매개변수와 잘못된 타입을 거부한다. 세션 생성은 모델 식별자를 저장하며 실제 모델 지원 여부·로딩은 실행 시 기존 Service가 확인한다.

| 메서드 | params | 결과 |
|---|---|---|
| `agent.info` | `{}` | protocol, client_id, 지원 methods, max_turns, working_directory, project_context_enabled |
| `agent.sessions.create` | 필수 `model`, 선택 `system` | 새 session_id와 세션 메타데이터 |
| `agent.sessions.list` | 선택 `cursor`, `limit`(기본 32, 1~100) | session_id 목록, 다음 페이지가 있을 때 next_cursor |
| `agent.sessions.get` | 필수 `session_id`, 선택 `offset`(기본 0), `limit`(기본 32, 0~100) | 세션 메타데이터, 전체 message_count, 요청 범위 messages, 필요 시 next_offset |
| `agent.sessions.fork` | 필수 `session_id`, 선택 `through_message_id` | 새 session_id, 복사한 message_count, parent_session_id |
| `agent.context.get` | 필수 `session_id`; 선택 `context_paths` | 현재 지침 파일·본문·SHA-256·적용 경로·fingerprint |
| `agent.run` | 필수 `session_id`, `prompt`; 선택 `options`, `max_turns`, `context_paths` | RunResult: run_id, session_id, status, text, usage, 필요한 경우 error |
| `agent.cancel` | `request_id` | cancel_requested: true |
| `agent.status` | `request_id` | method, session_id, queued/running, cancel_requested |

`options`는 기존 `GenerationOptions` JSON 계약이다. `max_turns`는 호스트 최대값 이하이다. 목록 cursor는 정렬된 UUID이며 동시에 세션을 만들 때의 스냅샷을 보장하지 않는다. `get(limit=0)`은 메시지 본문을 제외한 메타데이터를 읽는다. 실행 중에는 transcript 잠금을 유지하므로 같은 세션의 get/fork는 `model_in_use`가 될 수 있다. 진행 정보는 이벤트나 `agent.status`로 읽는다.

분기는 지정한 메시지까지 포함하고, 생략하면 전체 transcript를 복사한다. 원본 세션과 별개의 UUID·기록 파일을 사용한다. 실행 중이거나 아직 결과가 없는 도구 호출 경계는 거부한다. **artifact가 있는 세션은 `runtime_unavailable`로 거부한다.** 현재 기본 분기는 artifact 파일 복제·참조 변환이나 workspace 파일 복원까지 수행하지 않는다. parent_session_id는 응답 값이며 영속 분기 계보 기능은 아직 없다. 새 transcript는 크기·프로토콜 검증을 거쳐 한 번에 게시하며 실패한 복사본을 세션 목록에 남기지 않는다.

## HTTP

`POST /v1/rpc`, `Content-Type: application/json`, `Authorization: Bearer <token>`을 사용한다. 키를 JSON 본문에 넣을 수 없다. 요청 `id`는 클라이언트가 정한 1~128자 문자열이다. `params`를 생략하면 `{}`, `stream`을 생략하면 false이다.

```json
{"id":"create-1","method":"agent.sessions.create","params":{"model":"model://my-model"}}
```

일반 응답은 `{"id":"create-1","request_id":"server-uuid","result":{...}}`이다. 잘못된 키는 HTTP 401과 WWW-Authenticate, 다른 앱의 세션은 404, 사용 중인 세션은 409, 포화·용량 제한은 429, 요청 기한은 504를 반환한다. 인증·파싱 등 수락 전 오류는 error만 포함할 수 있다. 수락된 실행의 모델·도구 실패는 HTTP 200의 RunResult에서 `status`와 `error`로 표현될 수 있으므로 둘 다 검사한다.

`stream:true`이면 SSE `data:` 프레임으로 전달한다.

```text
data: {"id":"run-1","request_id":"server-uuid","event":"accepted"}

data: {"id":"run-1","event":"rpc","data":{"event":"started","run_id":"run-uuid","session_id":"session-uuid","sequence":1,"text":"","data":{}}}

data: {"id":"run-1","request_id":"server-uuid","event":"done","result":{...}}

data: [DONE]
```

`rpc.data`는 에이전트 Event 원본에 요청별 sequence를 더한 객체다. 위 예시는 필드 일부를 생략했다. tool_call_id·메시지·도구 결과 등도 기존 Event 계약을 보존한다. 최종 응답은 앞선 이벤트를 추월하지 않는다. 출력 제한·시간 초과로 스트림이 실패하면 done 프레임의 error를 확인한다. 헤더를 보낸 이후에는 HTTP 상태 코드를 바꿀 수 없다. 연결이 끊기면 종료 프레임 전송을 보장하지 않는다.

## Native IPC와 CLI

같은 메서드를 기존 줄 단위 JSON 소켓에 보내고, 최상위 `auth`에 키를 넣는다.

```json
{"id":"run-1","method":"agent.run","auth":"APP_TOKEN","params":{"session_id":"session-uuid","prompt":"Read README.md and explain it","max_turns":8}}
```

먼저 accepted/request_id, 이어서 `event:"rpc"`의 이벤트들을 받는다. native IPC의 마지막 줄은 기존 계약에 따라 `{"id":"run-1","result":{...}}` 또는 error이며 HTTP의 done 표시를 사용하지 않는다. request_id는 수락한 RPC ID, 내부 run_id는 Engine 실행 ID로 서로 다르다. 취소·조회에는 request_id를 사용한다.

클라이언트 키 하나만 담은 별도 일반 텍스트 파일을 만든 뒤 CLI로 호출할 수도 있다. 키 파일에는 JSON 객체를 넣지 않는다.

```sh
chmod 600 private/society-token
iillm --socket private/llm.sock --auth-file private/society-token rpc agent.info
iillm --socket private/llm.sock --auth-file private/society-token rpc agent.sessions.create create.json
iillm --socket private/llm.sock --auth-file private/society-token --json rpc agent.run run.json
```

`--json`은 진행 이벤트와 최종 결과를 줄 단위 JSON으로 출력한다. 생략하면 최종 결과만 JSON으로 출력한다. Ctrl+C·클라이언트 기한 종료는 소켓을 닫아 실행 취소를 전달한다. iillm은 계속 Qt Core/Network만 링크하는 얇은 클라이언트이며 추론은 daemon에서 수행한다.

CLI의 입력 파일에는 params 객체만 넣는다. 예를 들어 create.json은 `{"model":"model://my-model"}`이고 run.json은 `{"session_id":"session-uuid","prompt":"Read README.md and explain it","max_turns":8}`이다.

## C++ 앱에 연결

```cpp
#include <agent/Api.h>
#include <HttpApiServer.h>
#include <LocalIpcServer.h>

auto registry = std::make_shared<iiLocalLLM::agent::ToolRegistry>();
iiLocalLLM::agent::registerWorkspaceTools(*registry, workspace);
iiLocalLLM::agent::ApiOptions options;
options.workingDirectory = workspace;
options.stateDirectory = privateState;
options.clientTokens = {{"society", societyToken}, {"dreamscapes", dreamscapesToken}};
auto api = std::make_shared<iiLocalLLM::agent::Api>(
    std::make_shared<iiLocalLLM::agent::ServiceModel>(service), registry, policy, options);
iiLocalLLM::LocalIpcServer ipc(service);
iiLocalLLM::HttpApiServer http(service);
ipc.setRpcHandler(api);
http.setRpcHandler(api);
// 이후 listen(). 호스트 종료 순서는 두 전송 close(), api->close(), Service 정리.
```

앱 고유 기능은 기존 ToolRegistry에 등록하거나 `mcpTools`로 연결한다. 호스트가 API 인스턴스 하나를 두 전송에 주입해야 인증·세션·진행 요청을 공유한다. 전송은 하위 `RpcHandler` 인터페이스만 참조하며 `agent::Api`를 참조하지 않는다. handler 교체는 서버가 닫힌 동안만 가능하다. 같은 API의 모델·registry·policy는 모든 등록 클라이언트가 공유하므로 도구 권한이 다른 앱은 별도 API/서버로 제공한다.

## 수명·제한·복구

ApiOptions 기본값은 동시 요청 8개, 대기 32개, 앱별 세션 1,024개, 최대 32턴, 요청·결과 JSON 4 MiB, 요청 기한 300초다. 이벤트도 각각 4 MiB 및 요청당 200,000개로 제한한다. 전송의 기본 입력 제한은 1 MiB, 출력 버퍼는 4 MiB이므로 실제 허용치는 양쪽 제한을 따른다. 큰 세션 조회는 limit을 줄여 페이지로 읽는다.

daemon의 API 동시 요청 6개·대기 0개와 HTTP 전송 용량은 독립이다. 0.28은 일반 HTTP 응답 기본 8개, 제어 응답 기본 2개를 허용한다. agent.permissions.pending/respond 및 agent.cancel/status는 별도 제어 용량을 사용한다. 일반 요청이 승인을 기다려도 이 네 메서드는 처리할 수 있으며 모든 응답의 용량은 전송이 끝날 때 회수된다. --http-workers/--http-control-requests 및 C++ 한도는 [ControlCapacity.md](ControlCapacity.md)를 따른다. native 연결 자체가 포화되면 같은 앱의 새 연결이나 기존 connection-local cancel을 사용한다.

실행은 호출 연결에 붙어 있다. 연결 해제·출력 버퍼 초과·기한 종료·API 종료는 취소를 전달하며, 수락 슬롯은 작업이 실제로 종료될 때 해제한다. `Api::close()`는 실행을 취소하고 작업자를 join한다. 모델·도구·이벤트 함수는 취소에 협조해야 하고 콜백 안에서 close·파괴 또는 자기 future 대기를 호출하면 안 된다. 메타데이터의 파일 게시나 도구 부작용은 취소로 롤백되지 않는다.

status는 현재 프로세스에서 진행 중인 요청만 조회한다. 완료 후·재시작 후 request_id는 NotFound가 될 수 있다. transcript는 재시작 후 같은 세션 ID로 이어갈 수 있지만 에이전트 실행 응답 재전송, 영속 실행 핸들, idempotency key는 아직 없다. 별도 셸 작업 ID와 출력 기록은 아래 `agent.shell` 계약으로 보존한다. 연결이 끊긴 에이전트 실행을 자동 재시도하지 말고 세션 기록에서 결과를 확인한다. 이전 프로세스에서 도구 결과를 남기지 못한 호출은 기존 Engine의 결과 미확인 처리로 닫는다.

검증은 `tests/agent_api_tests.cpp`, `tests/agent_transport_tests.cpp`, `tests/agent_api_smoke.py`, 설치 소비자 `tests/consumer/api.cpp`에 있다. 실제 수치와 모델·설치 검증 결과는 [Verification.md](Verification.md)에 기록한다.

## 프로젝트 지침

0.5.0부터 고정 workspace 안의 CLAUDE.md·AGENTS.md·`.claude/rules`를 매 모델 호출 전에 읽는다. `context_paths`는 최대 128개 문자열이며 작업 루트 안의 파일 경로다. 아직 생성하지 않은 파일도 지정할 수 있다. `agent.context.get`에 지정한 경로는 조회에만 사용하고, `agent.run`에 지정한 경로는 세션에 기록하여 이어지는 실행과 분기에 유지한다. 다른 앱의 세션 조회는 기존과 같이 거부한다. 호스트만 `--agent-no-project-context`로 자동 로딩을 끄거나 `--agent-context-exclude PATTERN`을 반복해 제외 패턴을 설정할 수 있다. RPC로 루트·제외·상한을 변경할 수 없다.

변경된 스냅샷은 `instructions_loaded` 이벤트로 경로·해시·패턴을 알린다. 이벤트에는 지침 본문을 넣지 않는다. 본문은 인증된 조회 결과와 모델 입력에만 포함한다. 자동 지침은 파일의 직접 읽기 이력이나 수정 권한을 만들지 않는다. 상세 범위·순서·상한·미구현 항목은 [ProjectContext.md](ProjectContext.md)를 참조한다.

## 수동 대화 압축 (0.6.0)

`agent.sessions.compact`는 `session_id`, 선택적 `instructions`, `options`를 받는다. 기존 실행과 동일한 인증·세션 독점·큐·요청 기한·상태 조회·취소를 사용하며 `RunResult`를 반환한다. 성공 시 `turns`는 0, `text`는 요약이다. `compaction_started`, `compaction_progress`, `compacted` 이벤트를 기존 스트림으로 전달한다. `agent.sessions.get`에는 원본 메시지 페이지와 `compaction_count`, 최근 `compaction`이 포함된다. `agent.info.auto_compact_enabled`로 호스트 설정을 확인한다. 자동 압축은 기본 켜짐이며 `--agent-no-auto-compact`로 끌 수 있다. 자세한 예산·영속성·실패 계약은 [Compaction.md](Compaction.md)를 따른다.

## 0.9.0 MCP 연결 관리와 도구 검색

호스트가 지정한 MCP 설정 파일의 연결·복구, 대화별 `ToolSearch`, 인증된 `agent.mcp.status` 및 CLI 옵션을 추가했다. 설정과 실행 권한, 수명 및 미지원 범위는 [ToolDiscovery.md](ToolDiscovery.md)를 참조한다.

## 작업·Todo API (0.11.0)

agent.tasks.create/get/list/update/claim 및 agent.todos.write/get을 추가한다. session_id와 도구별 인수를 받으며 {text,result,is_error}를 반환한다. 인증된 앱이 소유한 대화와 현재 workspace를 검사하고 모델 실행 중에도 접근한다. daemon에서는 기본 활성화이며 --agent-no-tasks로 끈다. TaskCreated/TaskCompleted와 일반 권한·훅이 동일하게 적용된다. [Tasks.md](Tasks.md)에 상태·페이징·revision·오류 계약과 CLI 사용법을 기록한다.

## 백그라운드 셸 API (0.12.0)

`agent.shell.start/output/stop/list`는 `session_id`와 해당 셸 도구의 인수를 받으며 `{text,result,is_error}`를 반환한다. start는 `Bash` 권한을 확인하고 `run_in_background=true`를 호스트가 지정한다. output은 TaskOutput, stop은 TaskStop, list는 ShellTaskList에 대응한다. `agent.info.background_tasks_enabled`로 사용 여부를 확인하며 데스크톱 POSIX daemon의 `--agent-no-background`로 끌 수 있다.

실행 중인 에이전트와 독립적으로 조회·중단한다. 출력 조회의 남은 API 기한이 요청한 대기 시간보다 짧으면 그 기한으로 제한하고, 아직 실행 중일 경우 timeout 오류를 반환한다. 조회 취소·기한 만료는 셸을 종료하지 않는다. 다른 앱이나 대화의 작업 ID는 접근할 수 없다. 재시작 기록, 바이트 페이징과 명시적 종료 계약은 [BackgroundTasks.md](BackgroundTasks.md)를 참조한다.

## 입력 큐 API (0.13.0)

`agent.info`의 `input_queue_enabled`와 methods로 확인한다. 모든 호출은 기존 앱 인증·workspace·세션 소유권을 적용한다.

| 메서드 | params | 결과 |
|---|---|---|
| `agent.inputs.enqueue` | session_id, text; 선택 kind·priority·context_paths | input, revision, 실행이 있으면 active_run_id |
| `agent.inputs.list` | session_id; 선택 offset(0 이상), limit(1~100, 기본 32) | count, inputs, revision, 선택 next_offset |
| `agent.inputs.remove` | session_id, input_id | removed, input_id, revision |
| `agent.inputs.run` | session_id; 선택 options·max_turns·context_paths | 기존 RunResult와 실행 이벤트 |

등록·조회·삭제는 실행 중 transcript 잠금을 요청하지 않고 불변 세션 메타데이터로 소유권을 확인한다. 별도 제어 풀은 기본 동시 2개·대기 16개이며 C++ `ApiOptions.maxConcurrentInputControls/maxQueuedInputControls`로 지정한다. 풀 포화는 QueueFull이다. HTTP/native 전송 슬롯도 별도로 확보해야 한다. run은 일반 API 실행 풀을 사용한다.

```json
{"id":"steer","method":"agent.inputs.enqueue","params":{"session_id":"SESSION","text":"현재 작업을 중단하고 새 파일을 읽어라","priority":"now"}}
```

입력 큐의 now/next/later, 저장·복구, runQueued와 입력 전달 이벤트는 [InputQueue.md](InputQueue.md)를 따른다. 빈 큐 run은 RunResult의 not_found이며 placeholder 메시지를 추가하지 않는다. 새 요청으로 재등록하면 새 ID가 생기므로 응답 유실 시 자동 재등록으로 중복 방지를 보장하지 않는다.

로컬 스킬 목록은 인증된 `agent.skills.list`로 조회한다. `agent.run`에 `skill`과 `skill_arguments`를 주면 `prompt`를 생략할 수 있다. `agent.info.skills_enabled`와 데몬의 `--agent-skills-dir`/`--agent-no-skills`를 지원한다. [스킬 계약](Skills.md)을 참조한다.

0.15.0은 C++ 서브에이전트와 `agent.agents.run/output/stop/list`, 대응 MCP·CLI 경로를 추가한다. 소유권·취소·분기·재개와 설정 계약은 [Subagents.md](Subagents.md)를 따른다.

0.16.0의 `agent.agents.profiles`(MCP: `iiLocalLLM.agent.agents.profiles`, CLI: `agent agents profiles SESSION`)는 프로파일 메타데이터·출처·가려진 정의·오류를 반환한다. C++ 호스트는 `Subagents::attach`로 현재 프로파일을 각 턴에 연결한다. 독립 데몬과 MCP 서버의 `--agent-profiles FILE`·`--no-agent-profiles`, 모델 사용 범위와 훅 계약은 [AgentProfiles.md](AgentProfiles.md)를 따른다.

0.17.0에서는 `context: fork` 스킬을 같은 API·MCP·CLI 호출로 별도 자식에서 실행한다. 직접 호출은 자식 결과를 반환하고 모델의 `Skill` 호출은 후속 부모 턴에 결과를 전달한다. 본문 분리·권한·모델·사용량·큐 입력과 참조 차이는 [Skills.md](Skills.md)의 별도 자식 실행 계약을 따른다.

0.18.0의 스킬 allowed-tools와 인자 권한 규칙은 [Permissions.md](Permissions.md)를 따른다. API·IPC·MCP 입력은 allowed_tools/prompt_metadata 같은 호스트 전용 권한·출처 필드를 받지 않는다. 모델 Skill의 PermissionRequested 이벤트에는 고정된 permission_preview가 있다. 원격 권한 응답 중개는 0.27의 PermissionRequests.md 계약으로 추가되었다.

0.19.0의 `agent.permissions.get`은 인증된 `session_id`만 받아 현재 호스트 권한 snapshot을 반환한다. 다른 앱의 세션은 조회할 수 없으며 원격 mode·rule·설정 파일 변경은 허용하지 않는다. `iiLocalLLMD --agent-permission-settings FILE`의 출처·문법·미지원 범위는 [PermissionSettings.md](PermissionSettings.md)를 따른다.

0.21.0은 `--agent-hooks FILE`로 호스트 명령 훅을 연결한다. `agent.info.hooks_enabled`는 활성 여부이고 원격 등록/변경은 제공하지 않는다. 기존 `hook` 스트림 이벤트에 명령 SHA·종료 결과·제한된 출력 진단을 전달한다. 생성/완료 작업의 게시 전 차단과 실제 에이전트 도구/종료 처리에 동일하게 적용한다. [CommandHooks.md](CommandHooks.md)를 따른다.

0.22의 agent.run과 agent.inputs.run은 동일한 UserPromptSubmit·SessionStart 계약을 적용한다. 입력 차단은 RunResult status=failed/error_code=invalid_argument, continue:false는 cancelled이며 원본 메시지에 판정을 보존한다. 큐에서 확인한 차단 입력은 자동 재실행하지 않는다. 최초 활성화/새 Engine의 재개/압축 문맥을 적용하고 원격 userPrompt·sessionStartHooks·prompt_metadata는 허용하지 않는다. 기존 인증·소유권·hook 이벤트를 유지한다. [InputLifecycle.md](InputLifecycle.md)를 참조한다.

## 0.27 앱 권한 요청

인증 API/native IPC와 MCP 연결별 승인 채널, 독립 제어 처리, 훅·앱 경쟁, 취소·기한·중복 응답을 추가한다. [PermissionRequests.md](PermissionRequests.md)에 활성화·요청·응답 계약과 참조 차이를 기술한다. 전체 하네스와 실제 앱 통합 완료를 의미하지 않는다.
