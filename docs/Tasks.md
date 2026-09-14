# 영속 작업·Todo 관리

`agent::TaskStore`는 C++ 작업 목록 저장소이다. `TaskCreate`, `TaskGet`, `TaskList`, `TaskUpdate`, `TaskClaim`, `TodoWrite`, `TodoRead`를 같은 입력 검증과 트랜잭션으로 처리한다. 작업 상태 변경은 작업 실행이나 성공 검증을 뜻하지 않는다. 0.12.0의 [백그라운드 셸 실행](BackgroundTasks.md)과 팀 mailbox·계획 승인·입력 큐는 이 저장소와 별도의 하네스 기능이다.

## 계약과 참조 범위

분석 기준은 Exhen/claude-code-2.1.88의 고정 커밋 `c8cd253554319f32ff64ff7000636199f720c9bc`에서 관찰한 TaskCreate/Get/List/Update, TodoWrite 및 작업 선점 동작이다. TypeScript 구현은 복사하지 않는다. 현재 [Claude Code 공개 문서의 작업 목록](https://code.claude.com/docs/en/interactive-mode#task-list)은 참고 자료이며 고정 분석본과 동일한 버전이라고 가정하지 않는다.

| 도구 | 주요 입력과 결과 |
|---|---|
| TaskCreate | `subject`, `description`, 선택 `activeForm`, `metadata`. `pending` 작업과 숫자 문자열 ID 생성 |
| TaskGet | `taskId`. 전체 작업, 전체 의존 관계, `unresolvedBlockedBy` 반환 |
| TaskList | 선택 `offset`, `limit`(1..100, 기본 100). 숫자 ID 순서, 요약 목록, 미완료 `blockedBy`, `total`, 필요 시 `nextOffset` |
| TaskUpdate | `taskId`와 변경 필드. 상태는 `pending`, `in_progress`, `completed`, 삭제 동작 `deleted`. `owner`의 빈 문자열은 담당자 해제 |
| TaskClaim | `taskId`, `owner`, 선택 `checkOwnerBusy`. 담당자·완료 상태·미완료 선행 작업을 한 잠금 안에서 검사하고 `in_progress`로 선점 |
| TodoWrite | `todos`: `content`, `activeForm`, `status`를 가진 객체 배열로 전체 교체 |
| TodoRead | 선택 `offset`, `limit`. 현재 Todo 목록과 페이지 정보 |

모든 수정 도구는 선택 `expectedRevision`을 지원한다. 현재 목록의 `revision`과 다르면 효과 없이 실패한다. 호출자는 최신 목록을 읽고 수정 의도를 다시 판단해야 한다. 생성 ID는 삭제 후에도 재사용하지 않는다. 변경 없는 수정은 revision과 저장 파일을 바꾸지 않는다.

`TaskUpdate.metadata`는 최상위 키 단위 병합이며 null 값은 해당 키를 삭제한다. 중첩 객체는 통째로 교체한다. `addBlocks`, `addBlockedBy`, `removeBlocks`, `removeBlockedBy`는 양방향 참조를 함께 변경한다. 누락된 ID·자기 참조·순환을 거부한다. 제거 후 추가 순서로 적용한다. 삭제는 다른 필드 변경과 함께 요청할 수 없으며 모든 연결 참조를 같은 트랜잭션에서 제거한다. TaskList의 blocker는 완료된 선행 작업을 제외하지만 TaskGet은 전체 관계를 보존한다. 일반 상태 수정은 실제 진행을 기록하는 기능이며 미완료 blocker가 있다고 거부하지 않는다. `TaskClaim`은 blocker를 검사한다.

선점 실패는 예외가 아니라 `success:false`와 `already_claimed`, `completed`, `blocked`, `owner_busy` 중 이유를 반환한다. 동일 담당자의 이미 선점된 작업 재확인은 성공하며 상태가 같으면 revision을 바꾸지 않는다. `owner`는 작업 배정용 문자열이다. 계정 인증이나 OS 사용자 검증을 대체하지 않는다.

완료된 Todo도 명시적으로 삭제할 때까지 보존한다. 분석본의 완료 목록 UI 자동 숨김은 적용하지 않는다. Todo의 진행 항목을 하나로 강제하지 않는다. 실제 여러 작업의 병행 상태를 기록할 수 있다.

## 저장·동시성·실패

호스트가 선택한 목록 ID는 1..128자의 ASCII 영문·숫자·점·밑줄·하이픈이며 첫 글자는 영문 또는 숫자이다. 경로로 해석하지 않는다. 저장소는 목록마다 `board.json` 한 파일과 `board.lock`을 사용한다. JSON에는 schema `iisacc.agent.tasks/1`, `listId`, `revision`, `nextId`, `tasks`, `todos`가 있다.

Qt 6.8.3의 [QLockFile](https://doc.qt.io/qt-6.8/qlockfile.html)로 별도 객체·스레드·프로세스의 쓰기를 직렬화한다. 잠금 대기는 기본 5초이고 취소를 검사한다. 살아 있는 작성자의 잠금을 시간 경과만으로 지우지 않는다. [QSaveFile](https://doc.qt.io/qt-6.8/qsavefile.html)의 임시 파일과 commit을 사용하고 직접 덮어쓰기 fallback을 끈다. 파일은 소유자 읽기·쓰기, 디렉터리는 소유자 전용으로 만든다. 상태·잠금·목록 디렉터리 symlink를 거부한다. 이것은 협력하는 같은 OS 사용자 프로세스의 저장 계약이며 악성 동등 권한 프로세스에 대한 OS 샌드박스가 아니다.

기본 상한은 작업 1,000개, Todo 1,000개, 파일 4 MiB이다. 제목 1,024자, 설명 65,536자, activeForm 1,024자, 담당자 128자, metadata 256키·64 KiB를 제한한다. 저장 파일이 손상되거나 상한을 넘으면 기존 데이터를 유지하고 오류를 반환한다. 저장 성공 뒤 응답 전달이 끊기면 호출자는 revision과 작업 목록을 조회해야 한다. 생성·수정을 자동 재실행하지 않는다. revision 비교는 중복 생성에 대한 멱등 키를 대체하지 않는다.

문자열 길이는 JSON Schema의 Unicode 코드 포인트 기준이며 UTF-16 저장 단위 수가 아니다. 입력과 저장 후 재조회에서 같은 스키마를 사용하므로 보충 평면 문자·이모지가 포함된 담당자도 같은 상한으로 보존한다.

## C++와 에이전트

```cpp
#include <agent/TaskStore.h>
#include <agent/Engine.h>

auto tasks = std::make_shared<iiLocalLLM::agent::TaskStore>(privateDirectory);
auto created = tasks->execute("host-selected-list", "TaskCreate",
    {{"subject", "Verify package"}, {"description", "Build and test an installed consumer"}});
// 다른 호스트 프로세스도 같은 privateDirectory와 목록 ID로 접근할 수 있다.

iiLocalLLM::agent::EngineOptions options;
options.sessionsDirectory = privateSessions;
options.taskToolsEnabled = true;
// options.taskToolsDeferred = false; // 처음부터 모델에 전체 스키마를 제공할 때
```

임베디드 Engine에서는 명시적으로 켠다. 활성화하면 대화마다 별도의 작업 목록을 사용하고 기존 ToolSearch·JSON Schema·정책·훅·ToolRunner를 거친다. `runTaskTool(sessionId, name, arguments)`는 모델 없이 동일 경로를 호출한다. 실행 중 transcript 잠금과 독립적이므로 진행 상태를 조회·수정할 수 있다. RulePolicy는 `builtin.task`로 식별된 내부 계획 상태 수정을 Plan 모드에서도 허용하지만 명시적 deny/ask 규칙이 우선한다. MCP의 임의 annotation은 이 권한을 부여하지 않는다.

`HookKind::TaskCreated`, `TaskCompleted`는 작업 생성 및 완료 전환의 게시 직전에 호출한다. `HookInput.text`/`result.data`에 게시 예정 작업이 제공된다. block/예외/취소면 전체 트랜잭션을 저장하지 않는다. 이름의 Created/Completed는 생명주기 종류이며 저장 후 알림이라는 뜻이 아니다. 원시 TaskStore 사용자는 `TaskCommitCallback`으로 같은 게시 전 검사를 제공할 수 있다. 이 콜백은 목록 잠금을 가진 상태이므로 같은 저장소로 재진입하지 않아야 한다. 콜백이 외부에 낸 효과는 저장 트랜잭션으로 되돌릴 수 없다.

각 모델 턴에 최신 revision, 작업/Todo 수, 최대 각 32개 항목의 짧은 상태를 컨텍스트로 제공한다. 이 상태는 원본 대화의 메시지로 위조하지 않으며 입력 예산 측정에 포함한다. 재시작·resume·compaction 후에도 저장소에서 다시 읽는다. fork는 새 대화에 빈 작업 목록을 만든다. 빈 목록도 현재 컨텍스트에 명시하여 복사된 부모의 과거 도구 결과와 혼동하지 않게 한다. 부모의 진행 중 작업을 완료·재배정하거나 공유하지 않는다. 호스트가 명시적으로 공유 작업 목록을 원하면 독립 TaskStore와 `taskTools(store, listId)`를 등록한다.

## API·CLI·MCP

daemon의 에이전트 API는 기본 활성화하며 `--agent-no-tasks`로 끈다. `agent.info.task_tools_enabled`로 확인한다. 인증된 클라이언트별 Engine 저장소 안에서 대화 소유권을 검사한다.

`agent.tasks.create/get/list/update/claim`, `agent.todos.write/get`은 위 도구 입력에 `session_id`를 더한 객체를 받는다. 응답은 `{text, result, is_error}`이다. 입력 검증·정책·훅 실패는 `is_error:true`, 인증·세션 소유권·기능 비활성화는 기존 RPC 오류 계약을 사용한다. 호출 중에는 기존 ToolRunner 이벤트를 전송한다.

```sh
iillm --auth-file /private/app-token agent tasks list SESSION_ID
iillm --auth-file /private/app-token agent tasks create SESSION_ID /private/create.json
iillm --auth-file /private/app-token agent todos write SESSION_ID /private/todos.json
```

CLI 입력 파일에는 `session_id`를 넣지 않는다. 위치 인수로만 대상을 지정한다. 도구 오류가 있으면 JSON을 출력하고 종료 코드 1을 반환한다. 일반 `rpc` 명령에서도 같은 API를 호출할 수 있다.

`iillm-mcp`는 기본적으로 일곱 도구를 MCP 목록에 공개하며 `--no-tasks`로 끈다. 모델이 설정되어 있으면 현재 MCP 연결의 에이전트 대화와 같은 작업 목록을 사용한다. `new_session:true`는 새 빈 목록으로 전환한다. 모델 없는 서버는 연결마다 독립 목록을 만든다. 원격 인수로 다른 목록/대화 ID를 지정할 수 없다. MCP 연결 종료 후 새 연결은 이전 목록의 재개 권한을 얻지 않는다. 영속 대화의 재개·관리에는 인증된 agent API 또는 C++ 호스트가 필요하다.

이 도구들은 일반 MCP `tools/call`로 실행한다. MCP의 비동기 `tasks` 프로토콜은 별도 구현 범위이다.

직접 C++ MCP 서버를 구성할 때 `McpServerOptions.taskStore`로 모델 없는 저장소를 제공하거나, 작업 기능을 켠 Engine을 제공한다. 두 방식을 함께 지정하지 않는다. 서버의 기존 실행 잠금 때문에 직접 MCP 작업 호출은 같은 연결의 진행 중 agent.run 완료까지 기다릴 수 있다. 실행 중 작업 조회가 필요하면 agent API를 사용한다.

검증 결과와 남은 범위는 [Verification.md](Verification.md), [HarnessParity.md](HarnessParity.md)에 별도로 기록한다. 새 실행 모델·Python 서버·데이터베이스는 도입하지 않는다.

## 네이티브 모델 적합성 검사

고정 Qwen2.5 0.5B 검사는 계속 유지한다. 별도로 `IILOCALLLM_TEST_AGENT_CATALOG`에 설치 모델 카탈로그, `IILOCALLLM_TEST_AGENT_MODEL`에 그 안의 model:// URI를 지정하면 eager 및 ToolSearch 경로를 모두 검사한다. 모델 패키지는 검사용 임시 카탈로그에 hard link(불가능하면 복사)하고 manifest의 모든 파일 해시를 검증한다. 원본 카탈로그를 재설치하거나 실행 중인 사용자 daemon을 사용하지 않는다.

이 검사는 미리보기에 없는 임의 작업 설명의 실제 조회, 작업 한 건 생성, 담당자·상태 수정과 영속 결과를 요구한다. 모델 응답을 대신 만들거나 호출을 호스트가 추가하지 않는다. eager 검사는 일곱 작업 도구를 모두 공개한다. deferred 검사는 각 작업에서 ToolSearch와 실제 호출을 모두 요구한다. 0.5B 설정은 temperature 0·턴당 512토큰, 별도 카탈로그 설정은 temperature 0.6·top_k 20·top_p 0.95·턴당 2,048토큰이다. 둘 다 seed 0, 컨텍스트 8,192, 실행당 최대 6턴을 사용한다. 고정 소형 모델의 실패를 별도 모델의 성공으로 덮어쓰지 않는다.

이번 추가 검증 모델은 기존 registry에 있는 [Qwen3-8B-GGUF](https://huggingface.co/Qwen/Qwen3-8B-GGUF)의 Q4_K_M이다. Apache-2.0, 고정 revision `7c41481f57cb95916b40956ab2f0b139b296d974`, 5,027,783,488바이트, SHA-256 `d98cdcbd03e17ce47681435b5150e34c1417f50b5c0019dd560e4882c5745785`를 공식 Hub API에서 재확인했다. 가중치는 SDK 배포물에 포함하지 않는다.

`IILOCALLLM_TEST_AGENT_NO_THINK=ON`은 카탈로그 검사에만 [Qwen3가 문서화한 사용자 입력 `/no_think`](https://huggingface.co/Qwen/Qwen3-8B#advanced-usage-switching-between-thinking-and-non-thinking-modes-via-user-input)를 붙이고 temperature 0.7·top_p 0.8을 사용한다. 기본값은 OFF이다. 이는 테스트 입력의 명시적 모델 운용 조건이며 SDK가 사용자의 프롬프트를 수정하거나 모델의 기본 추론 모드를 변경하는 기능이 아니다. 기본 모드에서 관측한 출력 한도 실패와 이 별도 조건의 결과를 구분한다. 요구하는 실제 도구 호출·임의 값·영속 상태 검사는 두 조건에서 같다.

llama.cpp 모델 로딩의 `options.tool_grammar`는 boolean이고 기본값은 true이다. false는 구조화 대화의 생성 시 스키마 문법 제약만 끈다. 모델에 제공하는 원래 스키마, 도구 응답 파서, 불완전 출력 거절 및 실행 전 JSON Schema·권한 검사는 유지한다. 일반 텍스트 생성에는 영향을 주지 않는다. 호스트는 모델을 unload한 뒤 바뀐 옵션으로 다시 로드해야 한다. 현재 포함한 [upstream JSON Schema 변환기](https://github.com/ggml-org/llama.cpp/blob/5202104b59ada9005db079eea43882a2b7bf5802/common/json-schema-to-grammar.cpp#L712)는 선택 필드의 출력 순서를 제한하므로 여러 필드를 자연스러운 순서로 출력하는 모델과의 대조에 사용할 수 있다. 이 옵션은 잘못된 의미의 인수를 자동 수정하지 않는다.

`IILOCALLLM_TEST_AGENT_TOOL_GRAMMAR=OFF`는 카탈로그 수락 검사에서 위 옵션을 끈다(기본 ON). 생성과 수정 프롬프트는 요청 문자열을 따옴표로 구분하고, 수정 후 담당자·상태뿐 아니라 기존 제목과 설명 보존까지 검사한다. 모델·샘플링·프롬프트·생성 문법 조건이 다른 실행은 별도 결과로 보고한다.
