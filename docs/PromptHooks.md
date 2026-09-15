# C++ 프롬프트 훅 (0.30.0)

`agent::CommandHooks`의 `type: "prompt"`는 호스트의 조건을 로컬 모델에 한 번 질의하고 JSON 판단을 검증한다. 기존 명령·HTTP 훅과 같은 matcher, `if`, `once`, 진단·동시 실행 상한을 사용한다. 생산 실행 경로는 C++이며 Python은 전송 교차 검증에만 사용한다. 도구를 여러 번 실행하는 `type: "agent"` 검증 루프는 별도 미구현 항목이다.

```json
{
  "hooks": {
    "PreToolUse": [{
      "matcher": "Write|Edit",
      "hooks": [{
        "type": "prompt",
        "prompt": "Approve only when the proposed file change follows the user's request. Hook input: $ARGUMENTS",
        "timeout": 30,
        "statusMessage": "Checking the proposed file change"
      }]
    }]
  }
}
```

daemon에는 `--agent-hooks FILE`, MCP에는 `--hooks FILE`을 지정한다. 파일은 기존 호스트 설정처럼 workspace 밖의 비공개 JSON이어야 한다. MCP 프롬프트 훅은 `--model model://id --models CATALOG`가 필요하며, 모델 없이 설정하면 서비스 초기화 전에 거부한다. API·CLI·MCP 요청 데이터로 훅 설정을 추가하거나 덮어쓸 수 없다. C++ 호스트는 `CommandHooks(settings, options).callback()`을 `EngineOptions.hooks`에 연결한다.

| 설정 | 계약 |
|---|---|
| `type` | `prompt` |
| `prompt` | 비어 있지 않은 조건 문자열, NUL 제외·최대 65,536 문자 |
| `model` | 선택한 호스트 모델 식별자. 생략하면 현재 대화 모델, 독립 ToolRunner에서는 `hookModelName` |
| `timeout` | 초 단위, 0 초과 3,600 이하. 기본 30초 |
| `if` | 기존 단일 도구 권한 규칙. 호출 이름/입력에 대한 조건이며 권한 부여가 아님 |
| `once` | 프로세스 내 세션·설정 엔트리별 한 번. 모델 실행 시작 후 실패해도 소비됨 |
| `statusMessage` | 진단에 포함하는 호스트 표시 문자열 |

지원 이벤트는 [CommandHooks.md](CommandHooks.md)의 16개 이벤트이다. 같은 이벤트에서 매칭된 `prompt + if`가 같으면 마지막 설정의 모델·기한·once·표시 문자열을 사용한다. 서로 다른 조건은 별도로 실행한다. 기본 `maxConcurrentProcesses=4`는 명령·HTTP·프롬프트에 공유되는 슬롯이며, 모델 훅만 별도 무제한 풀을 만들지 않는다. 호스트의 `Model` 구현은 기존 Engine 계약처럼 동시 호출과 협력 취소를 지원해야 한다.

## 입력과 대화 경계

`$ARGUMENTS`는 해당 이벤트의 JSON 입력 전체이다. 자리표시자가 없으면 `ARGUMENTS:`와 JSON을 끝에 붙인다. `$ARGUMENTS[n]`과 `$n`도 기존 스킬과 공유하는 따옴표·역슬래시 인자 분리기를 사용해 치환한다. 인덱스는 0부터 시작하며 없는 인덱스는 빈 문자열이다. JSON 입력은 보통 인자 하나로 분리되므로 전체 `$ARGUMENTS` 사용을 권장한다. 셸·환경 변수·명령 치환은 실행하지 않으며, 삽입된 입력 안의 `$ARGUMENTS` 등을 재귀적으로 치환하지 않는다.

`HookInput.modelContext`는 C++ 호스트 전용이다. 모델 포인터, 선택 모델, 불변 `Session` 스냅샷과 도구 정의를 담으며 명령 stdin·HTTP JSON에는 직렬화하지 않는다. Engine은 현재 소유한 세션에서 스냅샷을 만든다. 같은 세션의 저장소 잠금을 다시 획득하지 않는다. 압축된 모델 이력과 차단된 사용자 입력의 기존 제외 규칙을 적용한다. 훅 질의·응답은 원래 transcript에 쓰지 않는다.

도구 호출 후 결과가 아직 저장되지 않은 경계에서는 미실행임을 명시한 임시 Tool 결과를 짝지어 네이티브 대화 템플릿에 전달한다. `PostToolUse`·실패 훅에서는 해당 호출의 실제 결과를 사용한다. 이 결과는 모델용 복사본에만 존재하며 도구를 실제로 실행하거나 원래 이력을 변경하지 않는다. UserPromptSubmit은 현재 입력을 저장하기 전 이력과 이벤트 입력을 받는다. Stop은 방금 저장한 답변까지 받는다.

일반 Engine 도구 훅은 해당 턴의 도구 목록과 실행 직전 세션 스냅샷을 받는다. 그 외 Engine 생명주기 훅은 호스트 기본 registry의 정의를 받는다. Task 훅은 도구 목록을 구성한 시점의 스냅샷을 사용하며, 독립 `runTaskTool`·셸 제어는 모델 식별자와 이벤트 데이터만으로 판단한다. SubagentStart는 자식의 초기 이력과 범위 제한된 모델·도구를 받는다. 이후 자식 훅은 자식 Engine의 스냅샷을 사용한다.

MCP 직접 파일/셸 호출은 기본 모델을 공유한다. 일반 호출에서는 해당 MCP 대화가 유휴 상태이면 이력을 읽으며, 활성 세션이나 즉시 제어 요청에서는 이력 없이 이벤트 데이터와 모델 식별자를 사용한다. 다른 API가 동시에 점유한 세션을 강제로 읽지 않는다. MCP `agent.run` 내부에서는 일반 Engine 경로가 동일하게 적용된다. 독립 C++ ToolRunner에는 `ToolRunnerOptions.hookModel`·`hookModelName`과 필요시 `ToolContext.sessionSnapshot`을 호스트가 제공한다.

## 판단·권한·실패

응답은 `{"ok":true}` 또는 `{"ok":false,"reason":"설명"}`이다. `ok`는 필수 boolean, `reason`은 선택 string이며 다른 필드는 거부한다. Markdown 코드 블록, 일반 문자열, 잘못된 JSON, 도구 호출을 포함한 응답은 유효한 판단이 아니다. 입력·권한 수정은 프롬프트 훅 응답으로 수행할 수 없다.

`ok:true`는 훅을 통과한다는 뜻이다. 기존 호스트 Deny·Ask·경로 검증·스키마 검증은 계속 적용된다. `ok:false`는 `block`과 `stop`을 함께 반환한다. 도구·Stop·권한 요청에서는 현재 실행이 취소되며, UserPromptSubmit은 차단 disposition으로 기록되고 실행에 들어가지 않는다. TaskCreated/Completed의 false는 게시 전 트랜잭션을 중단한다. SessionStart·SessionEnd는 기존 비거부 생명주기 계약에 따라 판단을 진단에만 남긴다.

유효하지 않은 응답, 모델 미가용, 자원 상한 또는 모델 오류는 `non_blocking_error` 진단이다. 프롬프트 훅은 접근 제어 정책을 대체하지 않는다. 호스트 소비자 오류(`consumer_failure`)는 기존 콜백 계약대로 전파한다. 공통 이벤트 JSON 자체의 입력 상한 초과는 개별 판단 이전의 실행 오류이며, 훅별 문맥·출력 상한 진단과 구분한다. 시간 초과와 모델 내부 취소는 `cancelled` 진단으로 처리한다. 호출자 취소는 원래 요청에 전파한다. 판단용 토큰은 부모에 연결된 별도 자식이므로 자체 기한이 부모 토큰을 취소하지 않는다. 실행 중인 훅을 OS 수준에서 강제로 선점하지는 않는다.

기한은 슬롯 획득·입력 구성 후 모델 호출 직전부터 시작한다. 직렬화한 이력·도구·스키마·조건은 `maxInputBytes`(기본 1 MiB), 스트림 누계와 반환 문자열은 각각 `maxOutputBytes`(기본 1 MiB) 이하여야 한다. `maxModelTokens`는 기본 1,024, 허용 범위 1~16,384이며 sampling temperature는 0이다. 모델의 실제 컨텍스트 한도는 추가로 적용하며 자동 요약이나 잘림으로 훅 판단 자료를 바꾸지 않는다. SessionEnd에는 Engine의 전체 종료 예산(기본 1,500 ms)도 적용되어 긴 모델 판단이 취소될 수 있다.

진단에는 이벤트·인덱스·유형·프롬프트 SHA-256·표시 문자열·상태·시간을 남긴다. 성공/차단 판단에는 모델 식별자·사용한 이력 메시지 수·prompt/generated/cached 토큰을 추가한다. 프롬프트 원문이나 원래 대화 본문은 진단에 넣지 않는다. 훅 사용량은 개별 진단이며 메인 RunResult의 생성 사용량에 합산하지 않는다. 실패한 생성의 부분 사용량·비용 집계·전용 영속 훅 trace는 남은 작업이다.

## 네이티브 모델 연결과 참조 차이

`ModelRequest.responseSchema`, `enableThinking`, `systemPromptOnly`, `toolChoice`를 `ServiceModel`의 측정·생성 경로에 동일하게 전달한다. 프롬프트 훅은 별도 시스템 지시, `toolChoice=none`, `enableThinking=false`와 고정 판단 스키마를 사용한다. 훅에서 Engine을 재귀 호출하지 않으므로 UserPromptSubmit 등의 훅이 다시 실행되지 않는다. 도구 정의는 문맥에 포함할 수 있지만 실제 호출을 실행하는 루프는 없다.

`ConversationRequest.responseSchema`는 고정 llama.cpp의 `common_chat_templates_inputs.json_schema`로 전달된다. 네이티브 문법은 구조화 출력이 명시되면 `tool_grammar=false`에서도 유지한다. `enableThinking`은 요청 한 번의 선택이며, 생략하면 모델 로딩 옵션을 사용한다. [NativeThinking.md](NativeThinking.md)의 일반 모델 설정을 변경하지 않는다. 템플릿 지원 여부와 모델 판단의 정확성은 별도이다. 이 C++ 요청 필드를 HTTP Chat Completions의 일반 `response_format`·Ollama·MLX 계약 전체에 노출한 것은 아니다. 커스텀 Model은 같은 필드를 구현해야 하며 최종 판단 JSON은 훅 실행기에서 다시 검증한다.

분석 기준은 고정 미러 `c8cd253554319f32ff64ff7000636199f720c9bc`의 `utils/hooks/execPromptHook.ts`, `utils/argumentSubstitution.ts`, `schemas/hooks.ts`, `utils/hooks.ts`이다. 원본의 기본 소형 Haiku 선택을 로컬 대화 모델 선택으로 바꾸며 온라인 모델을 자동 호출하지 않는다. 원본은 도구 정의를 전달하지만 별도 실행 루프가 없고, 이 구현은 `toolChoice=none`을 명시한다. 원본 인자 분리기의 shell-quote 연산자 처리·파싱 실패 fallback·순차 재치환을 그대로 복제하지 않는다. 알 수 없는 응답 키를 무시하지 않고 거부한다. 기존 명령/HTTP와 공유한 세션·스케줄링 정책은 원본 전체 훅 설정 계층과 동일하다는 뜻이 아니다.

기존 Qt와 고정 llama.cpp를 재사용한다. 새 라이브러리·온라인 의존성·Python 생산 실행기를 추가하지 않는다. 원본 `execAgentHook.ts`의 StructuredOutput 도구, 최대 50개 assistant 메시지 루프, 제한된 도구 실행과 dontAsk 권한 문맥, Stop 강제 검증은 아직 별도 구현이 필요하다. 전체 생명주기·플러그인/스킬별 훅 병합과 앱·플랫폼 검증도 [HarnessParity.md](HarnessParity.md)의 partial 상태를 유지한다.

`tests/native_schema_smoke.cpp`는 실제 llama.cpp 모델에 JSON을 출력하지 말라는 입력을 보내고, `tool_grammar=false`에서도 고정 `ok:true` 스키마의 JSON이 생성되는지 별도로 검사한다. 훅 판단 중 발생한 작업 취소가 연결된 SSE 클라이언트에 온전한 오류/종료 프레임으로 전달되는지도 `tests/http_tests.cpp`에서 검증한다. `tests/model_hooks_wire.py`는 MCP의 재개 커서용 빈 SSE 시작 이벤트를 JSON 판단과 구분하며, 비어 있지 않은 잘못된 JSON은 검증 실패로 처리한다.
