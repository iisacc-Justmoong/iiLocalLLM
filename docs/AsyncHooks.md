# 비동기 명령 훅

0.33.0은 명령의 설정 기반 async/asyncRewake와 첫 stdout 행의 async 선언을 C++ 프로세스 실행기, 세션 입력 큐, 인증 API 및 MCP에 연결한다. 새 런타임 의존성은 없다. 기존 Qt QProcess와 C++ 스레드, InputQueue를 사용한다. 전체 하네스 목표는 partial이다.

명령 설정 `async: true` 또는 `asyncRewake: true`는 시작 확인, JSON stdin 쓰기 접수와 EOF 요청 뒤 호출을 반환한다. 실제 stdin 소비 완료를 기다리지는 않는다. 작업자는 프로세스 그룹, stdout/stderr, 취소와 기한을 계속 소유한다. 보통 명령도 첫 출력 행에 `{"async":true,"asyncTimeout":15000}`을 쓰면 실행 중 배경으로 전환한다. stdout이 여러 조각으로 도착해도 첫 행만 검사한다. HTTP의 async 응답은 외부 접수 확인이며 이 명령 실행기와 별개다.

```json
{"hooks":{"Stop":[{"hooks":[{"type":"command","command":"./check.sh","asyncRewake":true,"timeout":120}]}]}}
```

일반 비동기 완료는 stdout에서 async 선언 이후의 JSON 결과를 읽는다. 전체 JSON 객체를 우선하고, 여러 행이면 async 속성이 없는 첫 JSON 객체를 사용한다. systemMessage와 해당 이벤트의 hookSpecificOutput.additionalContext만 합쳐 소유 세션의 notification/next 입력으로 저장한다. 이미 끝난 호출에 대한 권한 변경·차단·중단·입력 교체·MCP 출력 교체는 적용하지 않는다. JSON 결과가 없으면 문맥도 없다. 일반 완료는 유휴 모델을 자동 실행하지 않으며 다음 실행/모델 경계에서 사용한다. 전달 실패는 훅 기록의 delivery_error에 남는다.

asyncRewake는 종료 코드 2에서만 재실행을 요청한다. stderr가 있으면 stderr, 없으면 stdout을 문맥으로 사용한다. 종료 코드 0/1, 비정상 종료, 취소는 재실행을 요청하지 않는다. 실행 중인 세션에는 큐로 전달하고, 유휴 세션은 Engine이 자동 runQueued로 이어간다. 자동 실행은 기존 호출의 임시 이벤트 콜백을 재사용하지 않는다. 완료 답변은 transcript와 hookStatus의 wake_run에 남는다. wake_run은 마지막 자동 실행만 표시하며 이전 RPC 스트림을 다시 열지 않는다. 클라이언트는 상태와 세션 이력을 조회한다.

EngineOptions.maxAsyncHookWakeRuns 기본값은 명시적 실행당 8이며 0이면 자동 실행을 끈다. 상한에 도달하면 wake_error와 대기 입력을 보존한다. 모델마다 maxTurns도 적용된다. 마지막 명시적 실행의 generation/maxTurns/contextPaths만 재사용한다. allowedTools, skill의 임시 권한, promptMetadata, 요청별 permissionRequests는 물려받지 않는다. API의 엔진 소유 권한 채널은 유지되며 MCP 요청에만 묶인 채널을 자동 실행에 다시 쓰지는 않는다. 독립 실행 전에 별도로 정책을 완화하지 않는다.

C++ 호스트는 Engine::hookStatus(sessionId, offset=0, limit=32)와 cancelHooks(sessionId, hookId={})로 제어한다. 훅 하나를 지정하면 해당 작업만 취소한다. hookId를 생략하면 세션의 현재 명령들과 진행 중 자동 실행을 취소하고 추가 자동 기동을 억제한다. 대기 문맥은 보존하고, 다음 명시적 실행에서 새 기동 예산을 시작한다. 기존 run handle의 취소도 그 실행에서 시작한 명령으로 전파한다. now 입력의 연산 중단은 배경 훅을 취소하지 않는다.

| 전송 | 조회 | 취소 | 소유권 |
| --- | --- | --- | --- |
| 인증 HTTP/native IPC 및 iillm rpc | agent.hooks.status {session_id, offset?, limit?} | agent.hooks.cancel {session_id, hook_id?} | API 인증 클라이언트의 세션 |
| MCP | iisacc/hooks/status {offset?, limit?} | iisacc/hooks/cancel {hook_id?} | 현재 MCP 연결만 |

MCP는 experimental.iisacc/asyncHooks에 iisacc.async-hooks/1과 두 메서드를 공개한다. HTTP RPC의 정상 응답 정리는 수락된 백그라운드 훅을 취소하지 않는다. 아직 끝나지 않은 요청의 연결 중단·기한 만료는 기존 취소 경로를 따른다. 제어 메서드는 모델용 도구가 아니며 등록된 별도 제어 용량을 사용한다. 외부 session_id나 설정/명령 입력은 받지 않는다. Engine 없는 MCP 내보내기도 연결별 수명·조회·취소를 제공하지만 문맥 큐와 자동 모델 실행은 없다. 조회는 count/hooks/next_offset을 반환하고 Engine의 활성 훅 상태가 있으면 wake_runs/pending_wake_inputs/wake_disabled/wake_error를, 자동 실행이 있으면 wake_run을 추가 제공한다. max_wake_runs는 엔진의 설정값이다. 기동 상태는 조회 시점의 관측이며 페이지 사이 원자적 스냅샷은 아니다.

AsyncHookScope는 C++ 호스트가 제공하는 수명과 완료 처리기이다. Engine은 세션별 scope를 제공하고 MCP의 직접 도구 호출도 같은 소유 세션에 연결한다. 독립 CommandHooks는 자체 scope와 asyncResults(sessionId, consume)로 결과를 조회한다. 작업자의 완료 처리기는 잠금 밖에서 호출되며 자신의 scope를 닫거나 CommandHooks 소유자를 파괴해서는 안 된다. CommandHooks::close는 공유 설정의 모든 명령을 취소·합류한다. 마지막 소유자 파괴도 같은 정리를 수행한다.

scope는 기본 128개 기록을 보관한다(CommandHookOptions.maxAsyncRecords 또는 EngineOptions.maxAsyncHookRecords, 1~4096). 용량이 차면 가장 오래된 완료 기록부터 제거하고, 모두 실행 중이면 새 명령을 거부한다. 명시적 배경 명령은 프로세스 슬롯이 가득 차면 queue_full 진단으로 즉시 반환한다. 프로세스 출력은 기존 maxOutputBytes로 제한하고 기록의 stdout/stderr는 각각 4096자, text는 65536자로 제한한다. Engine/input queue의 더 작은 입력 한도도 적용한다. 완료 기록은 재시작 뒤 복구되지 않으며 수락한 notification은 기존 영속 큐 계약을 따른다.

endSession, clearSession, API 종료와 MCP 연결 종료는 해당 scope를 취소·정리한다. 명령 훅은 clear 이후 새 세션으로 옮기지 않는다. 기존 셸 작업·자식 작업의 이관 계약은 별도다. SessionEnd와 명시적 clear의 SessionStart는 forceSynchronousHooks로 처리한다. SessionEnd에는 기존 종료 예산, clear 시작 훅에는 각 훅의 기한을 적용한다. 호스트가 직접 forceSynchronousHooks를 설정한 호출도 배경 전환하지 않고 async 선언 뒤 최종 동기 JSON을 처리한다.

SubagentStart는 자식 Engine의 scope로 문맥을 전달한다. 자식의 도구/Stop 훅도 같은 수명을 따른다. 수명이 한 번의 위임 실행에 한정된 자식 Engine은 자동 유휴 기동을 끄고 종료 시 남은 훅을 정리한다. 자식 실행 종료 후 재개, Sleep/팀 mailbox, durable wake scheduler, 참조의 환경 캐시 무효화와 전체 설정/스킬/플러그인 병합은 구현한 것으로 간주하지 않는다.

참조는 미러 c8cd253554319f32ff64ff7000636199f720c9bc의 schemas/hooks.ts, utils/hooks.ts의 executeInBackground/첫 행 감지, utils/hooks/AsyncHookRegistry.ts, utils/attachments.ts 및 utils/messages.ts의 async_hook_response이다. 미러의 출처를 독립 인증한 것은 아니다. 참조의 asyncTimeout은 registry에 저장되지만 조사한 완료 검사에서는 실행 기한으로 사용되지 않았다. SDK도 이를 메타데이터로 기록하며 실제 기한은 명령 timeout이다. 참조의 전역 레지스트리·UI 알림 대신 소유 세션과 영속 입력 큐를 사용하고, 재실행 횟수·보관 한도를 명시한다.

대상 플랫폼은 기존 명령 훅과 같은 데스크톱 POSIX이며 검증 환경은 macOS/Qt 6.8.3이다. 다른 OS나 실제 앱의 최신 패키지 적합성은 별도다. ToolContext와 EngineOptions/CommandHookOptions가 바뀌었으므로 소비자는 0.33 헤더와 라이브러리를 함께 다시 빌드한다. 결정적 회귀는 tests/async_hooks_tests.cpp, 실제 API/CLI/MCP 전송은 tests/async_hooks_wire.py, 실제 모델은 tests/async_hook_runtime_smoke.cpp이다. 최종 빌드·설치·검사 증거는 [Verification.md](Verification.md)에 구분해 기록한다.
