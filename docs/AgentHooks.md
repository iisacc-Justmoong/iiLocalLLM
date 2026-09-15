# C++ 에이전트 훅 (0.31.0)

`agent::CommandHooks`의 `type: "agent"`는 별도의 짧은 대화를 시작하고 실제 도구 결과를 확인한 뒤 `StructuredOutput`으로 조건의 충족 여부를 반환한다. 기존 C++ Engine·도구 실행기·권한·스킬·지연 도구 검색을 재사용한다. 생산 경로에 Python 프로세스나 새 의존성을 추가하지 않는다. 단일 모델 판단은 [PromptHooks.md](PromptHooks.md)의 `type: "prompt"`이다.

```json
{
  "hooks": {
    "Stop": [{
      "hooks": [{
        "type": "agent",
        "prompt": "Read verification.txt and check that the requested work is complete. Inspect actual tool results before calling StructuredOutput. Hook input: $ARGUMENTS",
        "timeout": 60,
        "statusMessage": "Checking the completed work"
      }]
    }]
  }
}
```

daemon의 `--agent-hooks FILE`, MCP의 `--hooks FILE`, C++의 `EngineOptions.hooks`에 같은 설정을 연결한다. 설정은 workspace 밖의 비공개 호스트 파일이며 API·MCP의 모델 입력에서 변경할 수 없다. MCP는 `--model model://id --models CATALOG` 없이 agent 훅을 설정하면 초기화 전에 거부한다. 모델 별칭을 임의의 온라인 공급자로 해석하지 않는다.

## 입력과 실행

공통 matcher·`if`·`once`·`statusMessage`·자리표시자 문법은 [CommandHooks.md](CommandHooks.md), [PromptHooks.md](PromptHooks.md)를 따른다. 기본 기한은 60초이며 `timeout`은 0 초과 3,600 이하의 초 단위이다. 같은 이벤트에서 일치한 `type + prompt + if`가 같으면 마지막 설정을 선택한다. prompt와 agent는 별도로 중복 제거한다. `once`는 실제 실행기 호출 시작 후 소비되며 프로세스 재시작을 넘겨 보존하지 않는다. 모델·실행기 부재 등 시작 전 오류에서는 소비하지 않는다.

모델은 명시한 `model`, 부모 대화 모델, 독립 실행기의 `hookModelName` 순으로 선택한다. 이력을 복사하지 않는 새 대화에 확장된 조건을 넣는다. 시스템 지시는 중첩된 훅 입력·원래 사용자 요청·예정된 도구 호출을 수행 명령이 아닌 검증 자료로 구분한다. 이는 모델 지시이며 호스트의 실제 권한 검사를 대체하지 않는다. 자체 시스템 지시와 지원되는 스킬 카탈로그는 Engine이 구성하지만 부모 프로젝트 지시를 자동으로 재탐색하지 않는다. `enableThinking=false`, temperature 0, 기본 `maxModelTokens=1024`를 사용한다. 부모의 모델 로딩 설정은 변경하지 않는다. 모델은 일반 텍스트 대신 실제 도구 호출을 반환할 수 있어야 한다.

`StructuredOutput`은 호스트가 새로 등록하는 예약 도구이다. 입력은 `{"ok":true}` 또는 `{"ok":false,"reason":"설명"}`이며 `ok`는 필수 boolean, `reason`은 선택 string이다. 알 수 없는 키·잘못된 타입은 일반 도구 입력 검증에서 거부되어 모델이 수정할 수 있다. 일반 텍스트만 반환하면 내부 Stop 훅이 결과 도구 호출을 요구한다. 성공한 결과 도구는 추가 모델 질의 없이 대화를 끝내며 같은 응답의 뒤쪽 도구는 실행하지 않는다. 앞서 완료한 도구의 효과는 유지한다.

최대 assistant 메시지 수는 50이다. 참조의 경계 순서를 따라 49번째의 유효한 결과는 수락하고 50번째 응답은 도구를 실행하기 전에 중단한다. 압축용 모델 질의는 이 메시지 수에 포함하지 않으며 사용량·출력 상한에는 포함한다. 기존 호스트의 자동 압축 옵션과 모델 컨텍스트 한도도 적용된다.

## 도구와 권한

검증기는 해당 호출의 registry 스냅샷을 사용한다. 부모 `StructuredOutput`은 제거하고 자신의 결과 도구를 등록한다. 부모의 terminal 플래그도 제거한다. 스킬·ToolSearch·대화 읽기 도구는 새 대화에 맞춰 구성한다. 호스트에서 허용한 인라인 스킬과 지연 MCP·앱 도구 검색/실행은 사용할 수 있다. fork 스킬 실행기는 연결하지 않아 재귀 위임을 시작할 수 없다.

Agent·AgentOutput·AgentStop·AgentList·AgentProfiles, TaskOutput·TaskStop, EnterPlanMode·ExitPlanMode·ExitPlanModeV2, AskUserQuestion, Workflow, `iiLocalLLM.agent.*`, `builtin.subagent` 계열 도구를 검증 모델에서 제외한다. 부모의 추가 도구 필터·서브에이전트 프로필 범위도 유지한다. 예약 결과 도구만 프로필의 일반 이름 필터를 통과시키며 명시적인 호스트 거부 규칙은 계속 적용한다. 도구 이름은 호스트가 통제하는 계약이며 임의의 커스텀 도구 동작을 판별하는 OS 샌드박스는 아니다.

권한 판단은 실제 부모 세션 ID와 현재 명시적 규칙·세션 허용을 사용하되 호출 모드는 `dontAsk`로 설정한다. 부모의 bypass/acceptEdits에 따른 암묵적 쓰기 허용을 상속하지 않는다. Deny가 먼저, Ask가 Allow보다 먼저이며 Ask는 거부된다. 허용한 스킬의 일시적 권한은 검증 호출 안에서만 유효하다. 부모의 질문·권한 갱신 콜백/채널은 연결하지 않는다. 서브에이전트의 Plan 제한은 그대로 적용된다. 도구 훅은 현재 ToolContext의 호출별 허용도 전달한다. Engine 생명주기·Task 콜백의 스냅샷은 세션 정책을 기준으로 하므로 부모 run/인라인 스킬의 일시적 허용 전체를 복제하지는 않는다. 커스텀 `PermissionPolicy`도 C++ `ToolContext.permissionMode` 계약을 구현해야 한다.

부모 transcript는 자동으로 입력 이력에 포함하지 않는다. 기본 workspace `Read`에 한해 호스트가 전달한 **정확한 파일 한 개**를 읽을 수 있다. 준비·실행 시 canonical 경로가 바뀌지 않았는지 확인하고 POSIX no-follow 읽기로 symlink를 거부한다. 읽는 파일은 최대 `min(maxInputBytes, 1 MiB)`의 UTF-8 일반 파일이며 Read의 offset/limit 규칙을 따른다. 파일 옆의 다른 상태, 링크를 통한 별칭, 쓰기 권한은 부여하지 않는다. 명시적 Read Deny·Ask는 이 임시 허용보다 우선한다. 커스텀 Read 구현이나 Read가 없는 registry에는 이 예외를 주입하지 않는다. 허용된 Bash 및 호스트의 외부 도구는 자체 기능을 실행하므로 파일 도구의 경로 제약이 셸 전체를 격리한다는 뜻은 아니다.

MCP는 외부 연결 ID와 Engine 대화 ID를 구분한다. task/control wrapper는 연결 ID를 그대로 사용하고, 검증기의 작업 목록·세션 권한은 실제 Engine 소유 대화에 연결한다. 활성 대화나 즉시 제어 경로에서는 이력을 잠금 대기 없이 읽기 위해 시도하지 않고 메타데이터만 사용한다. 검증기가 원래 Engine의 세션 잠금을 다시 획득하지 않는다.

Task 훅의 검증기는 같은 게시된 작업 목록에 접근한다. 원래 훅은 다시 실행하지 않는 기본 task handler를 사용한다. TaskCreated/Completed의 게시 전 검증 중에는 보드 잠금을 놓고, 검증 후 잠금을 다시 얻어 원래 전체 보드와 동일한지 비교한다. 변경이 있으면 `already_exists` 충돌로 원래 게시를 거부하며 검증 자체가 수행한 변경을 덮어쓰거나 자동 반복하지 않는다. 후보 작업은 게시 전까지 TaskList에 보이지 않는다. [Tasks.md](Tasks.md)에 이 동시성 계약을 기록한다.

## 판단과 종료

`ok:true`는 훅을 통과하며 도구 권한을 별도로 부여하지 않는다. `ok:false`는 `block=true`, `stop=false`이다. PreToolUse는 현재 도구를 거부하고 Stop은 피드백을 넣어 부모의 다음 턴을 요청한다. PostToolUse의 거부는 이미 실행한 효과를 되돌리지 않는다. UserPromptSubmit은 입력을 차단하고 TaskCreated/Completed는 게시를 거부한다. SessionStart·SessionEnd의 false는 진단에만 남긴다. 각 이벤트의 기존 실행 계약을 바꾸지 않는다.

시간 초과·내부 취소·50개 메시지 한도·결과 없이 끝난 제한 실행은 `cancelled` 진단이다. 모델 부재·잘못된 실행 결과·입출력 상한·모델 실행 오류는 `non_blocking_error` 진단이다. 공통 이벤트 JSON 자체의 크기 초과는 개별 훅 실행 이전의 오류이다. 호출자 취소와 호스트 소비자 실패는 부모에 전파한다. 훅은 접근 제어 정책의 대체 수단이 아니며 추론의 오류는 권한 허용을 의미하지 않는다.

기한은 공통 훅 슬롯 획득과 조건 확장 후 실행기 호출 직전부터 적용한다. Engine 초기화·모델·도구·정리에 걸린 시간도 기한 판단에 포함하며 모델·도구에는 연결된 자식 토큰을 사용한다. 정리는 Engine의 자체 종료 계약을 따른다. 호스트/모델/도구는 협력 취소를 지원해야 한다. SessionEnd의 전체 예산도 별도로 적용된다. 매 모델 입력의 직렬화된 system/messages/tools는 `maxInputBytes`(기본 1 MiB), 스트림 누계와 직렬화한 반환 assistant 메시지 누계는 각각 `maxOutputBytes`(기본 1 MiB)로 제한한다. 도구 결과에는 기존 Engine의 크기·artifact 규칙과 다음 모델 입력 상한이 적용된다.

임시 상태는 호스트의 비공개 `sessionsDirectory/hook-agents/hook-*`에 생성한다. 자식 실행을 취소·join하고 Engine을 닫은 뒤 임시 대화를 제거한다. 검증기가 시작한 native background Bash는 자식 세션 소유로 정리한다. 작업 파일·공유 TaskStore 변경·커스텀 도구의 외부 효과는 자동 rollback하지 않는다. 네이티브 셸의 종료된 작업 기록은 호스트 ShellTasks에 남는다. 모델 residency와 KV 캐시는 기존 Service의 한도·퇴거 정책을 따르며 임시 대화 디렉터리 삭제와 별개이다. Python·Qt 테스트의 임시 경로도 `build/tmp` 아래에서 실행하여 검증한다.

진단은 기존 해시·이벤트·상태·시간에 `agent_id`, `assistant_messages`, `tool_calls`, `tools_used`, `history_messages:0`, 모델 식별자와 prompt/generated/cached 토큰을 더한다. `tools_used`는 실제 시작한 도구 이름의 중복 없는 목록이며 인자·관측 내용은 포함하지 않는다. 훅 토큰을 부모 RunResult의 사용량에 합산하지 않는다. 예외로 종료한 생성의 부분 사용량·영속 훅 trace·비용 예산은 미구현이다.

## C++ 연결과 참조 범위

Engine 경로는 모델·registry·policy·스냅샷과 실행기를 자동 연결한다. 직접 ToolRunner를 사용하는 C++ 호스트는 `hookModel`, `hookModelName`, `hookAgent=engine.hookAgent()`를 설정하고 실제 `ToolContext.sessionSnapshot`·`transcriptPath`를 제공한다. `Engine::hookAgent()`는 Engine 객체의 raw pointer 대신 필요한 호스트 설정을 보유한다. 캡처된 모델·도구·콜백의 외부 자원 수명은 여전히 호스트 책임이다. 공개 C++ 구조체가 변경되어 0.31 헤더와 라이브러리로 consumer를 다시 빌드해야 한다.

동작 분석 기준은 미러 커밋 `c8cd253554319f32ff64ff7000636199f720c9bc`의 `utils/hooks/execAgentHook.ts`, `hookHelpers.ts`, `constants/tools.ts`이다. 새 대화·기본 60초·추론 비활성·dontAsk·정확한 transcript Read·결과 도구·Stop 강제·50번째 경계·false의 비중단 차단을 재현한다. 원본의 Haiku 기본값은 로컬 선택 모델로 대체하며 내부 `USER_TYPE=ant` 위임 예외는 구현하지 않는다. 원본의 정상 루프 후 `clearSessionHooks`와 달리 로컬 임시 상태는 예외 경로에서도 정리한다. 전체 설정/플러그인/스킬별 훅 병합·남은 생명주기·MLX 도구·실제 제품 앱/다른 플랫폼 검증은 완료되지 않았다. 전체 하네스는 [HarnessParity.md](HarnessParity.md)의 partial 상태이다.

`tests/agent_hooks_tests.cpp`는 실제 C++ 도구·파일·권한·작업 목록·MCP bridge·서브에이전트·스킬·셸을 제어 가능한 모델과 함께 검증한다. `tests/model_hooks_wire.py --agent-hooks --catalog ... --official-stdio`는 실제 로컬 모델을 API·IPC CLI·HTTP MCP·공식 Python MCP stdio client로 호출한다. 정상 판단·차단·Stop 후 수정·Task 게시 거부에서 실제 Read/TaskList와 StructuredOutput, 두 번 이상의 assistant 메시지를 확인한다. 도구 훅의 최종 native fixture는 호출마다 파일에 기록한 JSON 판단을 실제 Read로 읽고 반환하는 조건이다. 경로 이름의 의미를 판단하는 초기 조건에서 Qwen3가 잘못 허용한 사례도 Verification.md에 기록한다. 이는 실행 계층의 모델별 사례 검증이며 의미 기반 정책 판단의 정확성을 검증한 것은 아니다.
