# 로컬 팀 실행

0.48은 이름 있는 팀원이 독립된 C++ Engine 대화에서 실행되고, 같은 팀의 Task 목록과 메시지를 공유하며 유휴 상태에서 후속 메시지를 처리하는 기능을 제공한다. 기존 일회성 Subagents와 함께 사용할 수 있다. 전체 Claude Code 팀 기능과의 호환성은 아직 partial이다.

참조는 분석본 `c8cd253554319f32ff64ff7000636199f720c9bc`의 `TeamCreateTool`, `TeamDeleteTool`, `SendMessageTool`, `AgentTool` 및 `utils/swarm/inProcessRunner.ts`이다. 현재 참조의 SendMessage 입력은 `to`, `message`, 선택 `summary`이며, 과거 예제의 `recipient`/`content` 형식을 수용했다고 주장하지 않는다.

## C++ 구성과 수명

기존 Engine, TaskStore, InputQueue와 Qt의 JSON·원자적 파일 저장·잠금을 재사용한다. 생산 의존성은 추가하지 않는다. Python은 전송 검증에만 사용한다.

```cpp
agent::TeamsOptions config;
config.workingDirectory = workspace;
options.taskToolsEnabled = true; // 공유 Task 도구를 제공할 때 켠다.
auto teams = std::make_shared<agent::Teams>(model, registry, policy, options, config);
agent::Teams::attach(options, teams);
agent::Engine engine(model, registry, policy, options);
```

Subagents도 사용할 때는 Subagents::attach 다음 Teams::attach를 호출한다. Teams는 부모 sessionsDirectory 아래 teams에 상태를 저장한다. 상태 경로와 작업 디렉터리는 서로 포함할 수 없다. 심볼릭 링크 상태 경로·기록·잠금은 거부한다. 한 저장소는 하나의 coordinator만 소유한다.

각 팀원은 독립된 대화와 실행 스레드를 유지한다. 부모 Engine의 실행 슬롯을 기다리며 교착되지 않는다. 기본 한도는 팀 32개, 보존 팀원 합계 32명, 한 번에 32턴·300초, 팀원당 64회 실행, 수신자당 메시지 256개이다. 모든 제한은 호스트가 설정한다. 취소와 제한 시간은 Model·Tool·Hook의 협력적 취소를 전제로 한다. 강제 프로세스 격리는 제공하지 않는다.

부모 Engine를 먼저 닫고 Teams::close로 팀원 취소·join을 완료한다. close는 반복 가능하다. 작업자 콜백에서 소유 객체를 닫거나 파괴하면 안 된다. API와 MCP 호스트는 소유 세션 종료 시 팀원을 중단한다.

## 실행·메시지 계약

| 도구 | 동작 |
| --- | --- |
| TeamCreate | `team_name`, 선택 `description`/`agent_type`. 부모당 하나의 팀을 만들고 새로운 작업 목록을 배정한다. 이름 충돌은 다른 이름으로 해결한다. |
| Agent | `name`과 `prompt`로 팀원을 시작한다. 선택 `team_name`은 현재 팀과 같아야 한다. `subagent_type`, 허용된 `model`, `max_turns`, 제한을 강화하는 `mode`를 지원한다. 이름 없는 호출은 기존 Subagents로 전달한다. |
| SendMessage | `to`에 팀원 이름 또는 `*`를 지정한다. 문자열 `message`는 비어 있지 않은 `summary`가 필요하다. 발신자는 세션에서 결정하며 인자로 위조할 수 없다. |
| TeamStatus / TeamInbox | 호출 세션의 팀 상태와 해당 수신자의 메일함을 조회한다. Inbox는 `offset`, `limit`으로 페이지를 나눈다. |
| TeamWait | `timeout_ms` 동안 팀원이 유휴 상태가 되기를 기다린다. 대기 중 취소할 수 있다. 대기 종료 때 보존된 리더 알림의 입력 큐 전달을 재시도한다. |
| TeamStop | 리더가 `name`으로 팀원을 취소하고 종료를 기다린다. |
| TeamDelete | 활동 중인 팀원이 있으면 거부한다. 유휴 팀원은 중단한 뒤 팀 기록·대기 팀 메시지·공유 Task 내용을 제거한다. 대화와 아티팩트는 보존한다. |

팀 이름은 영문·숫자 외 문자를 하이픈으로 바꾸고 소문자로 정규화한다. 팀원 이름은 `@`를 하이픈으로 바꾼 뒤 영문·숫자·하이픈·밑줄 64자 이내로 제한한다. 첫 문자는 영문·숫자여야 한다. 이름 비교에서 대소문자 중복을 거부하며 `team-lead`와 `host`는 예약어이다. 원본의 모든 이름 정규화 규칙을 구현한 것은 아니다.

Teams::attach는 호스트의 taskToolsEnabled 설정을 바꾸지 않는다. 비활성화하면 부모와 팀원 모두 Task 도구를 제공하지 않는다. 데몬의 --agent-no-tasks와 MCP의 --no-tasks도 유지된다.

팀원은 항상 비동기로 실행된다. 후속 문자열 메시지는 같은 대화에 전달되고 실제 모델 실행을 다시 시작한다. 브로드캐스트는 발신자를 제외한다. 중단된 수신자나 이전 호스트 활성화의 팀원은 명시적 오류로 처리한다. 별도 프로세스의 주소, `uds:`/`bridge:` 수신자는 지원하지 않는다.

공유 Task는 UUID 기반 전용 namespace를 사용한다. 부모와 팀원, Task lifecycle 검증 에이전트가 같은 목록에 접근한다. 다른 부모·팀·인증 클라이언트의 목록은 분리된다. TeamDelete의 TaskStore::retire는 내용 삭제와 함께 tombstone을 남겨 오래된 도구·다른 프로세스가 목록을 재생성하지 못하게 한다.

팀원의 도구는 선택한 프로필, 부모 필터와 현재 권한 정책을 함께 만족해야 한다. 각 후속 실행 전에 부모의 세션 권한을 상속한다. 팀원의 도구·모델·프로필 본문은 시작 시의 스냅샷이다. 일반 Subagents와 달리 현재 팀원 실행은 프로필의 skills/initialPrompt 등 추가 시작 설정을 거부한다. 팀원이 다시 Agent/TeamCreate/TeamDelete를 호출하여 범위를 넓힐 수 없다. ReadOnly 프로필은 문자열 메시지를 보낼 수 있지만 구조화된 종료 메시지는 읽기 전용 작업으로 취급하지 않는다.

## 종료·저장·복구

리더의 구조화된 `message: {type: "shutdown_request", reason?: "..."}`에는 호스트가 request_id를 부여한다. 해당 팀원만 team-lead에게 `{type: "shutdown_response", request_id, approve, reason?}`를 보낼 수 있다. 다른 팀원의 요청 ID, 이미 처리한 ID, 이유 없는 거부는 실패한다. 승인은 성공 도구 결과를 대화에 보존하고 이후 모델 호출과 도구 권한을 차단한다. 거부는 실행을 계속 허용한다. 구조화된 브로드캐스트와 아직 구현하지 않은 plan_approval_response는 거부한다.

메시지는 팀 outbox를 먼저 저장한 뒤 InputQueue로 전달한다. 큐가 가득 차면 `stored: true`, `success: false`와 개별 `queued: false`/`delivery_error`로 보존 여부와 전달 여부를 구분한다. 전송 재시도는 동일 입력 ID를 사용한다. 이미 소비된 메시지만 메일함에서 밀어내며, 미소비 메시지는 한도에 도달하면 새 메시지를 거부하여 보존한다. Engine의 ID 검사는 중복 transcript 입력을 억제한다. 프로세스 중단 전후의 외부 도구 실행까지 exactly-once로 보장하지 않는다.

호스트 재시작 시 이전 팀·팀원·Task·메일함은 읽을 수 있다. 이전 작업을 자동 재실행하지 않으며 활성 상태였던 팀원은 interrupted로 표시한다. 중단·재시작한 팀원의 자동 재개는 아직 지원하지 않는다. 기존 팀을 정리한 뒤 새 팀원을 시작할 수 있다.

부모 clear는 팀 리더와 작업 목록 소유권을 새 세션으로 옮기고 대기 팀 알림을 이전한다. 리더 변경 의도를 먼저 저장하므로 알림 이전이 중단되어도 다음 전달 또는 재시작에서 같은 ID로 재시도한다. fork에는 팀 소유권을 복제하지 않는다. 팀 삭제는 여러 파일의 단일 트랜잭션이 아니며 중간 실패 때 deleting 상태를 보존하고 재시도할 수 있다.

## API·MCP·CLI

ApiOptions::teamsEnabled와 TeamsOptions로 명시적으로 켤 수 있다. 각 인증 앱은 별도의 Teams 저장소를 갖는다. agent.teams의 create/delete/status/inbox/send/spawn/wait/stop 메서드는 부모 session_id를 필요로 한다. 팀원 session_id를 제출해 발신자를 가장할 수 없다.

팀 전용 spawn은 name과 prompt를 필수로 요구한다. name을 생략해 일반 Subagents 실행으로 우회하지 않는다. 모델에 제공되는 공용 Agent 도구는 이름 없는 일반 Subagents 호출도 계속 지원한다.

MCP는 `iiLocalLLM.agent.teams.<동작>` 도구 8개를 제공한다. 외부 session_id 인자는 없으며 연결의 대화로 범위를 고정한다. `iisacc/teams/status`, `inbox`, `send`, `stop` JSON-RPC 제어 메서드는 별도 제어 용량을 사용하되 같은 권한·훅 경로를 거친다. initialize의 `iisacc/teams` capability로 발견한다.

CLI는 `iillm --auth-file TOKEN_FILE agent teams ACTION SESSION [PARAMS_JSON_FILE]` 형식이다. 데몬은 agent API를 켜면 팀을 기본 제공하고 `--agent-no-teams`로 끈다. MCP 실행 파일은 agent와 비공개 `--state`가 있을 때 기본 제공하며 `--no-teams`로 끈다. 기존 `--agent-subagent-options`의 생성 옵션과 호스트 프로필 구성을 데몬 팀원에도 적용한다.

## 남은 범위와 검증

리더가 이미 실행 중이면 메시지는 Engine 입력 경계에서 소비된다. 유휴 리더를 자동 실행하는 기능은 아직 없다. 호스트는 기존 inputs.run/runQueued를 사용한다. 같은 활성화 안의 유휴 팀원은 자동으로 후속 메시지를 처리한다.

중단한 팀원의 재개, 프로필 전체 시작 설정, 실시간 프로필 갱신, 팀 계획 승인, 별도 프로세스/tmux, 원격 팀원, worktree 격리, 전체 팀 UI·Society/Dreamscapes 소비자·모바일 검증이 남아 있다. 따라서 전체 하네스 완료로 판정하지 않는다.

참조의 inProcessRunner는 종료 요청, 리더 메시지, 동료 메시지 순으로 처리 우선순위를 정한다. 현재 구현은 모든 팀 메시지를 같은 next 우선순위의 FIFO로 전달하며 이 우선순위 구분과 유휴 상태의 미배정 Task 자동 선택은 남아 있다. 또한 참조는 팀원의 최종 답변을 리더에게 자동 전송하지 않지만 현재 호스트의 idle_notification에는 최대 8,192자의 실행 결과가 포함된다. SendMessage의 명시적 메시지와 이 호스트 알림은 별도 기록이다.

`tests/team_tests.cpp`는 실행·대화 유지·공유 Task·권한·종료·이전·복구·보존을 검증한다. `agent_api_tests.cpp`와 `mcp_server_tests.cpp`는 인증과 연결 범위를 검증한다. `team_wire.py`는 실제 HTTP/IPC/MCP 실행 파일을 검사하며 없는 모델의 실패 보고까지 확인한다. 실제 모델의 성공은 별도 `team_runtime_smoke.cpp`의 Read·TaskCreate·SendMessage 결과와 변경된 파일에 대한 후속 실행으로 검증한다. 최종 빌드·테스트·설치·추론 수치는 Verification.md에 별도로 기록한다.
