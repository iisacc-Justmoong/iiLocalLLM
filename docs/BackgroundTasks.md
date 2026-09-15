# 백그라운드 셸 작업

0.24.0의 SessionClear는 실행 ID·프로세스·출력 파일을 유지한 채 소유권을 새 세션으로 넘긴다. 이전 ID의 조회/중지는 거부하고 새 ID로 제어한다. [SessionClear.md](SessionClear.md)를 따른다.

구현 계약과 검증을 이 문서에 기록한다. `TaskStore`의 계획 상태와 실제 실행 상태는 별개이다. 실행은 C++ `ShellTasks`, 기존 Qt 6.8.3의 QProcess·QLockFile·QSaveFile 및 표준 C++ 스레드를 사용한다. 새 런타임 의존성이나 Python 실행기를 도입하지 않는다. [QProcess](https://doc.qt.io/qt-6.8/qprocess.html)와 [QSaveFile](https://doc.qt.io/qt-6.8/qsavefile.html)의 공개 계약을 확인했다.

참조는 분석한 저장소의 `c8cd253554319f32ff64ff7000636199f720c9bc`이다. Bash의 `run_in_background`, TaskOutput의 대기/조회와 TaskStop의 종료를 독립적으로 구현한다. 출력 대기를 취소해도 원래 실행은 계속된다. 프로세스 종료는 명시적 TaskStop, 실행 시간/출력 상한 또는 호스트의 정상 종료로 수행한다. 호스트 비정상 종료 후 남은 실행은 결과 미확인 상태로 복구하며 명령을 재실행하거나 저장된 PID를 종료하지 않는다.

## C++와 도구

```cpp
auto shells = std::make_shared<iiLocalLLM::agent::ShellTasks>(workspace, privateState);
auto tools = std::make_shared<iiLocalLLM::agent::ToolRegistry>();
iiLocalLLM::agent::registerWorkspaceTools(*tools, workspace, shells);
// tools를 Engine/API/MCP에 전달한다. ShellTasks의 마지막 소유자 소멸 또는
// close()는 실행을 종료하고 모든 작업 스레드의 반환을 기다린다.
```

기존 인수 두 개인 `registerWorkspaceTools`는 동기 셸 계약을 유지한다. `ShellTasks`를 전달하면 Bash에 `run_in_background`와 `description`을 추가하고 제어 도구 세 개를 등록한다. 제어 도구는 기본적으로 지연 공개된다. 호스트는 ToolRegistry에서 각 정의의 `deferred`를 false로 바꾸어 처음부터 제공할 수 있다. 배경 작업 기능을 제공하는 실제 네이티브 도구인지 확인한 뒤에만 Engine의 직접 제어와 MCP 실행 잠금 예외를 적용한다.

| 도구 | 입력과 결과 |
|---|---|
| `Bash` | `command`, 선택 `timeout_ms`, `description`, `run_in_background`. 배경 실행은 QProcess의 시작 확인 또는 시작 실패의 기록을 기다린 뒤 `backgroundTaskId`, 현재 상태와 `output_file`을 반환한다. 시작 요청의 수락은 명령의 성공을 뜻하지 않는다. 일반 실행은 기존 text·exit_code·crashed 결과를 반환한다. |
| `TaskOutput` | `task_id`, `block` 기본 true, `timeout` 기본 30,000ms·범위 0..600,000. `retrieval_status`는 success/timeout/not_ready이며 실행의 성공·실패와는 별개이다. `task.status`, `exitCode`, `error_code`를 함께 검사한다. |
| `TaskStop` | `task_id` 또는 이전 입력명 `shell_id` 중 하나. 실행 중인 소유 작업에 종료를 요청하고 실제 실행 스레드의 종료 상태를 기다린다. 이미 종료된 작업은 거절한다. |
| `ShellTaskList` | iisacc 확장 도구. 같은 세션의 실행을 진행 중인 것부터, 이후 최근 생성 순으로 반환한다. `offset`, `limit` 기본 100·최대 100. `next_offset`이 있으면 다음 페이지를 조회한다. 마지막 페이지가 정확히 limit개이면 다음 페이지가 비어 있을 수 있다. |

TaskOutput은 `offset`과 `limit_bytes`(기본 24,576, 최대 65,536)로 원시 바이트를 페이지화한다. `next_offset`, `has_more` 및 UTF-8 표시용 `output`을 반환한다. 페이지가 다중 바이트 문자를 가르거나 출력이 바이너리일 때도 `output_base64`로 정확한 바이트를 복원할 수 있다. `output.log`에는 stdout·stderr의 실제 수신 바이트를 저장한다. 두 독립 파이프 사이의 정확한 발생 순서는 보장하지 않는다. 자기 세션의 출력 파일은 Read로 읽을 수 있으나 파일 크기 1MiB와 UTF-8 텍스트라는 Read의 기존 상한은 그대로 적용된다. 큰 출력과 바이너리는 TaskOutput 페이지를 사용한다.

`exitCode`는 프로세스가 정상적인 exit로 종료된 경우에만 숫자이며 0 또는 명령의 오류 코드이다. 신호에 의한 비정상 종료·취소·시간 초과·결과 미확인은 null로 반환한다. 실행의 최종 판단에는 status와 error_code도 사용한다.

`Bash` 권한은 실행 전에 기존 ToolRunner에서 확인한다. TaskStop은 이미 허용된 자기 세션 실행의 중단이므로 기본·Plan 모드에서 허용하지만 명시적 deny/ask 규칙이 우선한다. 출력·목록 조회 역시 명시 규칙을 따른다. 작업 ID는 `sh-UUID`이며 TaskStore의 숫자 계획 ID와 섞지 않는다. ID를 알아도 다른 세션의 실행을 읽거나 중단할 수 없다.

## 수명·저장·상한

새 세션/분기는 실행 소유권을 이어받지 않는다. 일반 에이전트 턴이나 API 시작 요청이 끝나면 이미 인수된 배경 실행은 계속된다. TaskOutput 조회의 취소·대기 시간 만료도 실행을 중단하지 않는다. API 출력 대기는 남은 API 요청 기한으로 제한된다. Engine은 각 모델 턴에 최근 실행 상태 미리보기를 넣고 토큰 예산에 포함한다. 미리보기는 원본 transcript에 추가하지 않는다.

상태 폴더는 프로세스 하나가 QLockFile로 소유한다. 각 작업은 별도의 `state.json`과 `output.log`를 사용한다. 시작/종료 기록은 QSaveFile로 교체하며 소유자 전용 권한을 설정한다. 세션·작업 경로, 기록 스키마, 심볼릭 링크와 출력 크기를 검사하고, 종료 기록과 파일 크기가 다르면 실패시킨다. 상태 폴더가 workspace 안에 있어도 Read/Write/Edit와 검색을 통한 내부 기록 노출을 막는다. 이것은 협력적인 같은 사용자 환경의 파일·도구 경계이며 OS 샌드박스가 아니다. 허용된 Bash 자체는 사용자 권한으로 실행한다.

| 설정 | 기본값·상한 |
|---|---|
| 동시 실행 | 기본 8, 호스트 설정 1..64 |
| 보존 기록 | 기본 1,000, 호스트 설정 1..10,000. 자동 삭제하지 않으며 가득 차면 신규 실행을 거절한다. 유지보수는 호스트를 닫은 뒤 수행한다. |
| 작업별 출력 | 기본 8MiB, 호스트 설정 1KiB..64MiB. 초과 부분을 저장하지 않고 실행을 종료해 resource_limit으로 기록한다. |
| 배경 실행 시간 | 기본 요청 1시간, 호스트 최대 기본 24시간. 요청은 호스트 상한 이내여야 한다. 동기 Bash는 기본 30초·최대 10분이다. |
| 명령·설명 | 명령 UTF-8 64KiB, 설명 4KiB. 도구의 명시 description은 1,024 Unicode 문자 이내이다. |

각 실행은 별도 POSIX 세션에서 시작한다. 취소 시 프로세스 그룹에 TERM을 보내고 실행 중인 셸을 최대 200ms 기다린 후 KILL을 전달해 종료를 회수한다. 정리 중 수신한 마지막 출력도 남은 바이트 상한 내에서 보존한다. 정상 셸 종료 뒤에도 남은 같은 그룹의 자식을 정리한다. 자식이 직접 새 세션으로 이탈하는 경우까지 포함한 OS 격리는 제공하지 않는다. 입력은 EOF로 닫고 셸 초기화 파일은 읽지 않는다. 부모 환경을 상속하지만 명령 간 환경·cwd 변경은 유지하지 않는다.

호스트의 정상 종료는 작업을 killed 또는 이미 관측된 completed/failed로 보존한다. 호스트의 비정상 종료 뒤 pending/running 기록은 `interrupted`, `host_interrupted`, 종료 코드 null로 전환한다. 이는 프로세스 결과 미확인을 뜻한다. 오래된 PID를 종료하거나 명령을 재실행하지 않으며, 예전 자식 프로세스가 이미 사라졌다고 주장하지 않는다. 종료 기록 저장 실패는 메모리 결과에 storage_failure와 persisted=false로 드러난다. 실행 상태와 출력은 제품 작업이 올바르게 끝났다는 검증을 대체하지 않는다.

## API·CLI·MCP

`agent.shell.start/output/stop/list`는 인증된 `session_id`를 필수로 받으며 같은 Engine/ToolRunner 정책과 저장소를 사용한다. start는 `run_in_background=true`를 호스트가 지정하므로 입력에서 이 필드를 받지 않는다. 결과는 `{text,result,is_error}`이다. 각 메서드는 실행 중인 에이전트 턴과 독립적으로 호출할 수 있다. 재시도 중복 방지를 위한 요청 재생/idempotency 계약은 아직 제공하지 않는다.

```sh
iillm --auth-file TOKEN agent shell start SESSION command.json
iillm --auth-file TOKEN agent shell output SESSION output.json
iillm --auth-file TOKEN agent shell stop SESSION stop.json
iillm --auth-file TOKEN agent shell list SESSION
```

`command.json`은 `{"command":"cmake --build build","description":"Build the application"}`, `output.json`은 `{"task_id":"sh-UUID","block":true,"timeout":30000}`, `stop.json`은 `{"task_id":"sh-UUID"}` 형태이다. CLI는 추론 라이브러리를 링크하지 않고 native IPC를 호출하며 도구 오류는 종료 코드 1로 반환한다.

데스크톱 POSIX daemon은 기본으로 API 상태 폴더의 `shells`를 사용한다. `--agent-no-background`로 비활성화한다. MCP CLI는 HTTP에서 비공개 `--state/shells`, stdio에서 `workspace/.iilocal-llm/shells`를 사용하며 `--no-background`로 비활성화한다. 이 옵션은 계획 Task/Todo 비활성화 옵션과 독립적이다.

MCP에서 셸·파일 도구와 로컬 에이전트가 같은 대화 소유권을 공유한다. 제어 도구는 모델/일반 도구의 실행 잠금 밖에서 동작하지만 권한·스키마·소유권 검사는 동일하다. 같은 연결에서 agent.run 또는 TaskOutput이 기다리는 동안 TaskStop을 처리할 수 있다. MCP 연결 종료와 `new_session=true`는 해당 이전 소유자의 실행을 정리한다. 재연결은 새 소유권이며 이전 ID로 다른 연결의 기록에 접근할 수 없다. 이는 일반 `tools/call` 기능이며 MCP 비동기 tasks 프로토콜 구현을 뜻하지 않는다.

## 검증 범위

C++ 회귀는 실제 셸과 자식 프로세스, 출력 크기/시간 제한, 조회 취소, 소유권, 기록 복구·손상 거절 및 중단 중 API/MCP 접근을 검증한다. 공식 Python MCP 클라이언트와 실제 daemon·CLI를 통한 검증도 수행한다. `tests/shell_runtime_smoke.cpp`는 고정 모델 카탈로그를 로드한 후 모델이 Bash→TaskOutput을 직접 호출하고 프롬프트에 없는 임의 파일 값을 답하는지 검사한다. 도구는 이 수락 검사에서 처음부터 제공한다. 모델 조건과 실제 통과/실패는 [Verification.md](Verification.md)에 별도로 기록한다.

자동 배경 전환, Ctrl+B, 지속 셸 환경, Windows/모바일 실행, 백그라운드 하위 에이전트·원격 실행, 완료 알림의 외부 push 및 이전 도구 이름 별칭은 아직 미완료이다. TaskOutput의 이전 입력형과 별칭을 모두 지원한다고 주장하지 않는다. 전체 요구사항은 [HarnessParity.md](HarnessParity.md)를 따른다.

0.13.0의 [입력 큐](InputQueue.md)는 진행 중인 에이전트에 prompt/notification을 전달한다. 백그라운드 셸이 끝날 때 알림을 자동 생산해 큐에 넣는 연결은 아직 제공하지 않는다. now 입력은 현재 에이전트 연산을 취소하며 이미 분리된 셸 실행은 TaskStop으로 중단한다.
