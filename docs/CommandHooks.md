# 외부 명령 훅

0.29.0부터 같은 CommandHooks에 `type:"http"`도 설정할 수 있다. HTTP/HTTPS 전송·URL/환경 정책·DNS/TLS·프록시·응답 계약은 [HTTPHooks.md](HTTPHooks.md)를 따른다. 아래의 stdin·stdout·종료 코드·프로세스 설명은 명령 훅에 해당한다.

0.21.0의 `agent::CommandHooks`는 호스트가 지정한 명령을 C++ 생명주기 콜백에 연결한다. 명령은 JSON을 표준 입력으로 받고 도구 입력 변경·권한 판단·차단·추가 문맥·실행 중단을 반환한다. daemon, IPC/API, MCP와 임베디드 Engine이 같은 실행기를 사용한다. 명령 실행·대기·JSON 처리·취소는 C++이며 Python은 검증 클라이언트에만 사용한다.

Qt Core의 QProcess·JSON·스레드 풀과 기존 프로세스 그룹 정리를 재사용한다. 별도 셸 라이브러리나 서버를 도입할 필요가 없어 새 생산 의존성은 없다. Qt의 기존 라이선스·배포 조건을 유지한다. 명령은 신뢰하는 호스트 코드이며 일반 도구 권한이나 OS 샌드박스로 명령 자체를 격리하지 않는다.

## 호스트 설정

```json
{"hooks":{"PreToolUse":[{"matcher":"Write|Edit","hooks":[{
  "type":"command","command":"/absolute/private/check-edit.sh",
  "timeout":10,"statusMessage":"Check edit policy"
}]}]}}
```

daemon의 `--agent-hooks FILE`, MCP 서버의 `--hooks FILE`에 지정한다. 파일은 workspace 밖의 소유자 전용 일반 파일이어야 하며 최대 128 KiB이다. 잘못된 JSON·이벤트·명령 종류는 서비스/모델 초기화 전에 거부한다. 추가 작업 디렉터리가 이를 포함해도 파일 도구에서 보호한다. 시작 시 설정 내용을 고정하며 재적용하려면 호스트를 다시 시작한다. 명령이 별도로 읽는 스크립트나 정책 파일까지 고정하는 기능은 아니다.

`.claude/settings.json`이나 스킬·에이전트 파일의 hooks를 자동 실행하지 않는다. API·MCP 인수로 명령을 등록하는 기능도 없다. 계층형 권한 설정은 별도 [PermissionSettings.md](PermissionSettings.md)를 따른다.

```cpp
#include <agent/CommandHooks.h>
#include <agent/Engine.h>
iiLocalLLM::agent::CommandHookOptions hookOptions;
hookOptions.workingDirectory = workspace;
iiLocalLLM::agent::CommandHooks commands(hostJson, hookOptions);
iiLocalLLM::agent::EngineOptions options;
options.sessionsDirectory = privateSessions;
options.hooks.append(commands.callback());
```

callback은 공유 구현을 보유하므로 원래 CommandHooks 객체가 없어져도 사용할 수 있다. `describe()`는 이벤트·매처·조건·시간 제한·once·명령 SHA-256을 제공하며 명령 본문과 환경은 반환하지 않는다. 임베디드 호출자는 JSON·환경·cwd를 직접 제공하므로 CLI 파일의 소유권 검사도 별도로 적용해야 한다. ToolRunnerOptions.hooks와 EngineOptions.hooks는 각각 직접 도구 호출과 에이전트 실행의 설정이며 기본 CLI는 둘 다 연결한다.

## 이벤트와 입력

stdin은 UTF-8 JSON 객체 한 개와 마지막 줄바꿈이며 채널을 닫아 EOF를 보낸다. 모델·도구 입력을 명령 문자열에 삽입하지 않는다. 공통 필드는 hook_event_name, session_id, run_id, cwd, transcript_path, permission_mode이다. 직접 ToolRunner만 사용하면 transcript 경로는 빈 문자열이고 Engine 경로는 실제 세션 JSONL이다. 생명주기에서 정책 정보가 제공되지 않으면 모드는 unknown이다.

| 이벤트 | 추가 입력과 효과 |
|---|---|
| PermissionRequest | 0.25: Ask 도구의 승인/거부·입력 변경·호스트 권한 갱신. [PermissionRequest.md](PermissionRequest.md) |
| PreToolUse | tool_name, tool_input, tool_use_id; 실행 전 입력 변경·권한 판단 |
| PostToolUse | 도구 정보와 tool_response(text/data/content); 이미 실행한 효과를 되돌리지 않음 |
| PostToolUseFailure | error, is_interrupt:false; 검증·정책·도구 실패 뒤 호출. 취소 예외는 전파하며 취소 전용 실패 이벤트는 없음 |
| Stop | last_assistant_message, stop_hook_active; block이면 다음 모델 턴 요청 |
| PreCompact / PostCompact | trigger(manual/auto), custom_instructions / compact_summary; 기존 압축 콜백 계약 |
| TaskCreated / TaskCompleted | task_id, task_subject, task_description와 SDK 전체 task 객체; 게시 전 거부면 트랜잭션을 저장하지 않음 |
| SubagentStart / SubagentStop | agent_id, agent_type, parent_session_id, 자식 transcript 경로. Stop에는 마지막 응답·stop_hook_active 포함 |
| BeforeModel / AfterModel | SDK 확장 이벤트. 각각 prompt / response와 기존 모델 콜백의 피드백·차단 |
| UserPromptSubmit | 0.22: 원래 prompt, input_source(direct/queue), 큐이면 input_id. 모델 실행 전 허용·차단·중단 |
| SessionEnd | 0.23: reason 매처, 실제 세션 종료의 비차단 정리. [SessionEnd.md](SessionEnd.md) |
| SessionStart | 0.22: source(startup/resume/compact/clear), model. 활성화/압축 문맥과 initialUserMessage |

SubagentStart는 접수 뒤 Engine.run 전에 호출하므로 run_id가 비어 있다. 자식 Stop은 SubagentStop으로 전달한다. 0.31.0부터 Task 게시 전 콜백은 저장소 잠금 밖에서 실행하며, 다시 잠근 뒤 원래 보드와 비교하여 동시 변경을 거부한다. 같은 보드 읽기가 가능하며 콜백에서 발생한 변경을 덮어쓰지 않는다. 훅이 외부에 낸 효과는 작업 저장 취소로 되돌리지 않는다.

매처는 빈 문자열 또는 `*`이면 전부 일치한다. ASCII 영문·숫자·밑줄·`|`만 있으면 정확한 이름 또는 이름 목록이고 나머지는 Qt QRegularExpression이다. 도구 이름, 자식 agent_type, 압축 trigger, SessionStart source, SessionEnd reason을 검사하며 그 외 이벤트는 매처를 생략한다. JavaScript 정규식의 모든 문법이나 도구 별칭을 지원하지 않는다. 명령의 선택 `if`는 `Write(allowed.txt)`와 같은 SDK 네이티브 권한 규칙 한 개다. 도구 호출이 있는 이벤트에서만 일치하며 계층형 설정의 출처별 파일 패턴과 다르다. [Permissions.md](Permissions.md)를 참조한다.

## 출력·권한·진단

```json
{"hookSpecificOutput":{
  "hookEventName":"PreToolUse","permissionDecision":"allow",
  "permissionDecisionReason":"Checked by host policy",
  "updatedInput":{"path":"allowed.txt","content":"Checked content"},
  "additionalContext":"Use the checked path in the response."
}}
```

stdout 앞뒤 공백을 제거한 첫 문자가 `{`이면 JSON 파싱을 시도한다. PermissionRequest를 제외하면 유효한 객체의 제어 결과는 종료 코드보다 먼저 처리한다. PermissionRequest는 정상 종료 0의 응답만 처리하며 종료 2를 거부로 우선한다. JSON 문법이 깨졌으면 일반 출력으로 처리하고, JSON은 맞지만 지원 스키마와 다르면 비차단 오류 진단을 남긴다. 알 수 없는 필드도 거부하므로 참조의 일부 unknown 필드 제거 동작과 다르다.

일반 출력의 종료 코드 0은 성공, 2는 stderr를 이유로 차단, 그 외는 비차단 오류이다. SessionStart·SessionEnd의 종료 2는 비차단 진단이다. 일반 stdout은 BeforeModel·PreCompact·UserPromptSubmit·SessionStart에서 피드백으로 사용하고 다른 이벤트에서는 진단이다. 잘못된 UTF-8, 시간 초과, 출력 한도, 시작 실패는 비차단 진단이며 취소는 전파한다. 거부권이 있는 이벤트에서 반드시 차단해야 하는 정책은 명시적 거부 또는 종료 코드 2를 사용해야 한다.

공통 JSON 필드는 continue, stopReason, suppressOutput, decision, reason, systemMessage이다. decision은 approve/block이며 PreToolUse의 permissionDecision이 있으면 이를 우선한다. `continue:false`는 SessionStart·SessionEnd를 제외하고 stopReason으로 현재 실행을 취소한다. Stop의 block은 다음 턴을 요청하는 별도 동작이다. C++ HookResult의 stop·stopReason·permission·diagnostics도 같은 역할이다.

PreToolUse의 permissionDecision은 allow/ask/deny/passthrough이다. allow는 현재 호출의 도구 허용 후보에만 추가한다. 명시적 Deny/Ask, Plan, 자식 읽기 전용 범위, workspace와 비공개 경로 검사는 유지한다. Ask는 0.25의 PermissionRequest 훅과 C++ 구조화 응답, 기존 permission callback 순으로 처리하며 결정이 없으면 실행하지 않는다. 다음 호출·resume에 허용을 저장하지 않는다. 변경된 입력은 원래 스키마와 도메인 검사를 다시 통과해야 한다. 커스텀 PermissionPolicy는 허용 후보를 해석하지 않을 수 있으며 최종 판단은 해당 정책을 따른다.

PermissionRequest 이외의 지원 이벤트의 hookSpecificOutput은 이름 일치와 additionalContext를 받는다. PermissionRequest는 allow/deny decision 객체를 받으며 세부 계약은 [PermissionRequest.md](PermissionRequest.md)를 따른다. PreToolUse는 permissionDecision·permissionDecisionReason·updatedInput, SessionStart는 initialUserMessage를 추가 지원한다. 참조에 정의되지 않은 이벤트의 additionalContext는 SDK 콜백 확장이며 모든 출력 스키마와 동일하지 않다. PostToolUse는 0.32.0부터 updatedMCPToolOutput을 추가 지원한다. watchPaths, 설정 갱신, 권한 재시도 출력은 미지원이다.

0.22의 UserPromptSubmit은 허용된 stdout/추가 문맥을 입력에 연결한다. SessionStart의 성공한 stdout도 문맥에 추가하지만 종료 2·차단 reason·continue:false를 거부권으로 적용하지 않는다. 초기 입력 게시, 원문 보존, 차단 입력 제외와 재개·압축 계약은 [InputLifecycle.md](InputLifecycle.md)를 따른다.

명령들은 같은 원본 입력을 받아 제한된 동시성으로 실행한다. 결과는 완료 순서로 합친다. PermissionRequest는 첫 완료 결정의 behavior·입력·갱신 목록을 함께 보존하며 나머지는 진단만 수집한다. 다음 병합 규칙은 다른 이벤트에 적용된다. 차단·중단은 하나라도 있으면 유지하고 권한은 deny > ask > allow이다. updatedInput은 마지막 완료 결과를 사용한다. 변경 입력을 다음 명령에 직렬 전달하지 않는다. 하나가 차단해도 다른 명령의 외부 효과는 되돌리지 않는다.

명령 완료 진단에는 이벤트·설정 인덱스·명령 SHA-256·종료 코드·시간·결과와 stdout/stderr 각각 최대 4096자가 들어간다. systemMessage는 별도 진단으로 전달한다. suppressOutput은 stdout 진단만 숨기고 제어 결과·stderr·systemMessage를 삭제하지 않는다. 스크립트 출력은 호스트 이벤트에 전달된다. API `agent.info.hooks_enabled`와 MCP tools/list `_meta["iisacc/hooksEnabled"]`로 활성 여부를 확인한다. API 스트림과 progressToken이 있는 MCP 요청은 기존 hook 이벤트로 진단을 전달한다. 활성 여부는 콜백 등록 여부이며 비어 있는 명령 설정도 등록된 콜백으로 표시될 수 있다. 전체 생명주기 지원 목록이나 설정 내용을 공개하는 API는 아니다.

## 한도와 남은 범위

| 옵션 | 기본 / 허용 범위 |
|---|---|
| timeoutMs | 600000 ms / 1..3600000. 명령 JSON timeout은 초 단위 |
| maxHooks | 128 / 1..1024 |
| maxConcurrentProcesses | 공유 실행기당 4 / 1..32 |
| maxInputBytes / maxOutputBytes | 각각 1 MiB / 1..4 MiB. 출력은 명령별 stdout+stderr 합계 |
| maxOnceEntries | 32768 / 1..1048576 |

기존 환경을 캡처하고 CLAUDE_PROJECT_DIR·IILOCALLLM_PROJECT_DIR를 workspace로 지정한다. cwd는 canonical workspace, 명령은 `/bin/sh -c`이다. `shell:"bash"`도 참조 POSIX 실행처럼 이 경로를 쓰며 Bash 전용 문법을 보장하지 않는다. 필요한 해석기는 명령에 직접 지정한다. 프로세스 그룹으로 TERM/KILL 정리를 수행하며 별도 세션으로 이탈한 자손까지 격리하는 기능은 아니다.

timeout은 시작 확인 뒤 적용하며 슬롯 대기와 최대 5초 시작 대기는 별도다. 큐 대기에서도 취소를 확인한다. 매칭 없는 이벤트는 큰 결과를 직렬화하지 않는다. 매칭된 입력이 입력 한도를 넘으면 명령을 시작하지 않고 오류를 전파한다. 명령별 출력 한도는 전체 콜백 피드백 합계 한도가 아니다.

once:true는 동일 프로세스의 공유 실행기에서 세션·설정 항목별 한 번이다. 동시 호출도 먼저 예약한 하나만 실행한다. 프로세스를 시작하면 실패해도 소비하고 시작 전 실패는 예약을 반환한다. 표가 가득 차면 비차단 오류를 남기며 기존 기록을 버리지 않는다. 호스트 재시작 뒤에는 복구하지 않는다.

macOS에서 검증한 데스크톱 POSIX 명령 실행기이다. Windows·iOS·Android·WASM은 명령 설정을 거부하며 Linux 실기기 검증은 별도다. HTTP 훅은 0.29에 추가했다. prompt 훅은 0.30에 추가했다([PromptHooks.md](PromptHooks.md)). agent 훅, 명령 async/asyncRewake, powershell, 명령 중복 제거, PermissionDenied·Notification·Setup·ConfigChange·Worktree·파일 감시·팀/elicitation 이벤트와 스킬·에이전트·플러그인 hooks 병합은 남아 있다. 지원하지 않는 설정은 명시적으로 거부한다.

분석 기준은 고정 미러 `c8cd253554319f32ff64ff7000636199f720c9bc`의 schemas/hooks.ts, types/hooks.ts, entrypoints/sdk/coreSchemas.ts, utils/hooks.ts, services/tools/toolHooks.ts이다. 설정·stdin·JSON/종료 코드 순서·매처·권한 우선순위를 관찰해 C++로 구현했다. 미러 출처 주장의 독립 인증이나 전체 Claude Code 호환 인증은 아니다. 실행 증거는 [Verification.md](Verification.md), 남은 전체 목표는 [HarnessParity.md](HarnessParity.md)에 구분한다.

0.31.0부터 같은 호스트 설정에 `type: "agent"`를 사용할 수 있다. 실제 도구 실행·StructuredOutput·dontAsk·기한과 정리 계약은 [AgentHooks.md](AgentHooks.md)를 따른다.

0.32.0부터 성공한 MCP 호출의 PostToolUse는 `hookSpecificOutput.updatedMCPToolOutput`을 지원한다. 문자열·MCP 콘텐츠 배열, 원본 구조화 결과 제거, 병합·오류·MCP 재전달 스키마의 계약은 [McpOutputHooks.md](McpOutputHooks.md)를 따른다. 명령 종료 코드가 0이 아니거나 비정상 종료하면 결과 변경을 적용하지 않는다.

macOS 회귀 검사에서는 QProcess 명령 인자에 직접 넣은 한글이 분해형 유니코드로 전달되는 현상을 관찰했다. 이 변경에서 명령 실행기의 인자 인코딩은 수정하지 않았다. 정확한 stdout 코드 포인트를 비교하는 테스트는 JSON Unicode escape를 출력하며, HTTP와 C++ 콜백은 원래 한글 문자열을 직접 검증한다. 고정 스크립트·UTF-8 파일 출력과 명령 인자 문자열은 서로 다른 경로이다.
