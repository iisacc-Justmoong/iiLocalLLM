# 사용자 입력과 세션 시작 생명주기

0.24.0은 [SessionClear.md](SessionClear.md)의 실제 초기화와 즉시 SessionStart(clear)를 추가한다. 현재 ABI는 0.24이다. 아래 0.22/0.23 설명은 당시 구현 범위이다.

0.23.0에서 [SessionEnd.md](SessionEnd.md)의 실제 세션 종료가 추가되었다. endSession 뒤 같은 ID를 실행하면 resume하며, 아래의 0.22 구현 시점 설명과 구분한다.

0.22.0은 C++ UserPromptSubmit·SessionStart 콜백과 호스트 명령 훅을 기존 Engine·API·CLI·MCP 실행 경로에 연결한다. HookResult::initialUserMessage, RunRequest::userPrompt, EngineOptions::sessionStartHooks가 추가되며 0.22에서 ABI가 변경되었다. 소비자는 새 헤더와 라이브러리로 함께 다시 빌드한다.

## 사용자 제출

직접 프롬프트, 사용자 스킬 호출, 큐의 kind:prompt를 모델 실행 전에 검사한다. 스킬 훅에는 확장 본문 대신 `/이름 인자`와 선택적 추가 요청을 전달한다. 알림, 모델이 호출한 Skill, 자식에게 위임한 내부 명령에는 사용자 제출 훅을 새로 호출하지 않는다. RunRequest::userPrompt=false는 신뢰하는 C++ 호스트 전용이며 API·IPC·MCP 입력에는 노출하지 않는다.

| 훅 결과 | 현재 실행 | 저장과 후속 모델 문맥 |
|---|---|---|
| 허용·추가 문맥 | 계속 실행 | 원래 메시지를 보존하고 모델에 전달하는 복사본에 문맥을 추가 |
| decision:block 또는 명령 종료 2 | failed, invalid_argument | 차단 기록을 저장하고 일반 모델 문맥·압축 요약·분기 문맥·지침 대상 경로에서 제외 |
| continue:false | cancelled | 원래 입력을 저장하고 후속 대화에서는 유지 |

차단과 중단이 함께 있으면 차단을 우선한다. 추가 문맥은 허용한 입력에만 적용한다. 명령 오류·시간 초과의 비차단 계약은 [CommandHooks.md](CommandHooks.md)를 따른다. 일반 C++ 콜백이 예외를 던지거나 취소되면 그대로 실패/취소를 전파한다.

원본 JSONL의 metadata["iilocal.user_prompt_hook"]에는 version=1, disposition, reason, context가 있다. 스킬처럼 저장 본문과 제출 문자열이 다르면 submitted_prompt도 보관한다. 허용된 문맥은 원본 ID를 유지한 모델용 복사본에 한 번만 추가하므로 압축 경계 ID를 바꾸지 않는다. 원문 조회 API와 iiLocalLLM.session.read는 차단 기록도 반환할 수 있다. 입력 훅은 비밀 자료 접근 제어 기능이 아니다.

## 큐와 취소

새 InputQueue::deliver(prepare, persist)는 delivery.lock으로 소비자를 직렬화하고 queue.lock으로 게시·확인을 보호한다. 기존 단일 persist 오버로드도 같은 소비자 잠금을 사용한다. prepare는 queue.lock 밖에서 실행되어 같은 큐의 조회·추가·철회가 가능하다. persist는 queue.lock 안에서 수행하며 같은 큐에 재진입하거나 외부 관찰자를 호출하지 않는다. prepare에서 deliver를 다시 호출하는 것도 허용하지 않는다.

선택한 스냅샷의 동일 종류 입력만 한 배치에서 최대 limit개 준비한다. 준비 중 새로 게시한 입력은 다음 배치로 넘긴다. 준비 후 입력이 철회되었으면 저장하지 않는다. 입력이 바뀌었으면 protocol_error, 준비 예외·취소이면 큐를 보존한다. persist가 false를 반환하면 현재 항목까지 확인한 뒤 배치를 끝낸다.

Engine은 차단·중단된 입력도 원본과 판정을 저장한 뒤 큐에서 확인하므로 재개 때 같은 입력이 무한히 다시 차단되지 않는다. 뒤의 미처리 입력은 남는다. 같은 배치에서 먼저 수락한 입력은 이미 저장·확인되지만 뒤의 입력이 차단되면 그 실행은 모델을 호출하지 않고 끝난다. 이후 재개에서 수락한 이력을 사용한다.

저장 후 확인 전에 프로세스가 중단되면 입력 ID·원문·종류·메타데이터를 대조하여 원래 판정을 재사용한다. 이 경우 사용자 제출 훅과 transcript append를 반복하지 않는다. prepare의 외부 효과와 저장·확인은 하나의 트랜잭션이 아니므로 저장 전에 중단되면 훅 외부 효과가 반복될 수 있다. now가 준비를 취소하면 미확인 입력은 남고 새 연산에서 다시 준비한다. 준비 중 철회해도 이미 실행한 외부 효과를 되돌리지 않는다.

Engine의 훅은 세션 transcript lease를 보유한다. 큐 조회·게시·철회와 sessionMetadata는 가능하지만 같은 세션 전체 기록을 다시 열거나 run을 동기 대기하지 않는다. Message·InputDelivered 관찰자는 큐 잠금을 해제한 뒤 호출한다. 추가 큐 계약은 [InputQueue.md](InputQueue.md)를 따른다.

## 세션 활성화

SessionStart는 주 대화에서 최초 run/runQueued/compact를 시작할 때 실행한다. createSession·조회·직접 도구 호출만으로는 실행하지 않는다. 같은 Engine이 생성하거나 fork한 세션은 source=startup, 기존 세션을 처음 여는 새 Engine은 resume이다. 같은 Engine에서 다음 턴을 실행할 때는 반복하지 않는다. 압축 체크포인트를 저장한 뒤에는 source=compact를 보낸다. 최초 활성화가 수동 압축이면 활성화와 compact 사건이 모두 발생할 수 있다.

입력은 source·model과 공통 세션·실행·작업 디렉터리·원문 경로 정보이다. 명령 매처는 source를 검사한다. 성공한 텍스트 stdout 또는 JSON additionalContext를 별도 세션 문맥으로 저장한다. 명령 종료 2의 stderr와 JSON 차단 reason은 진단으로만 남기며 대화를 막거나 모델 문맥에 추가하지 않는다. block·continue:false는 SessionStart의 거부권으로 사용하지 않는다. 일반 콜백의 예외, 호스트 취소, 저장 실패, 자원 상한은 여전히 실행 실패/취소를 일으킨다.

initialUserMessage는 비어 있지 않으면 일반 next 우선순위 prompt로 게시한다. Engine의 maxInputCharacters와 일반 큐의 크기 제한을 모두 통과하고 소비 시 UserPromptSubmit을 받는다. 시작 훅이 현재 실행의 직접 입력을 대체하지 않는다. 압축 후 게시한 초기 입력은 다음 입력 소비 경계에서 처리하므로 같은 모델 요청에 즉시 들어간다는 보장은 없다. 자식 Engine은 sessionStartHooks=false이며 기존 SubagentStart/Stop을 사용한다.

활성화 기록은 Engine 수명 동안 유지한다. 명령 실행·문맥 저장·초기 입력 게시가 원자적이지 않으므로 실패 뒤 재시도에서 효과가 반복될 수 있다. 프로세스 간 exactly-once, 새 MCP 연결의 자동 세션 복원은 제공하지 않는다. 기존 비동기 실행·연결별 소유권과 취소 계약은 유지한다.

## 설정과 검증 범위

```json
{"hooks":{
  "SessionStart":[{"matcher":"startup|resume|compact","hooks":[{"type":"command","command":"/private/session-context.sh"}]}],
  "UserPromptSubmit":[{"hooks":[{"type":"command","command":"/private/check-prompt.sh"}]}]
}}
```

daemon의 --agent-hooks FILE, MCP의 --hooks FILE은 기존의 workspace 밖 소유자 전용 설정 파일 규칙을 따른다. 원격 호출로 훅이나 호스트 출처를 바꾸는 기능은 없다. 추가 생산 의존성 없이 기존 Qt JSON·QLockFile·C++ 명령 실행기를 재사용한다. Python은 검증용 클라이언트와 테스트 훅에만 사용한다.

고정 참조 c8cd253554319f32ff64ff7000636199f720c9bc의 processUserInput.ts, sessionStart.ts, hooks.ts, coreSchemas.ts를 확인했다. 0.23/0.24에서 SessionEnd와 SessionStart(clear)를 추가했다. watchPaths, PermissionRequest/Denied, 다른 생명주기와 HTTP/prompt/agent/async 훅은 남아 있다. 특히 참조의 PermissionDenied는 조건부 자동 분류 경로와 연결되므로 모든 도구 거부를 그 사건으로 대신하지 않는다. SessionEnd도 매 턴의 Stop으로 대신하지 않는다.

tests/input_lifecycle_tests.cpp는 차단/중단·재개·압축·분기·스킬·자식 범위·큐 재진입·취소·확인 실패를 검사한다. tests/command_hooks_wire.py는 실제 명령 프로세스와 인증 API·CLI·MCP의 같은 동작을 검증하며 선택적으로 실제 로컬 모델을 사용한다. 소스·메모리 검사·설치 consumer·네이티브 실행의 결과는 [Verification.md](Verification.md)에 구분한다. 전체 하네스 목표는 [HarnessParity.md](HarnessParity.md)의 partial 상태로 유지한다.
