# 세션 종료 생명주기

0.23.0은 C++ `HookKind::SessionEnd`, `Engine::endSession`, `Engine::close`와 API·MCP의 실제 종료 경로를 연결한다. `Stop`은 모델 턴의 종료 판단이고 `SessionEnd`는 주 대화의 활성화 수명 종료이다. 일반 응답·실패·취소가 매번 SessionEnd를 발생시키지는 않는다. EngineOptions에 필드가 추가되므로 소비자는 0.23 헤더와 라이브러리로 함께 다시 빌드한다.

## 발생 지점과 기록

| 경로 | 발생 시점과 reason |
|---|---|
| C++ endSession(id, reason) | 해당 세션 정리가 끝난 뒤, 지정한 사유 |
| C++ close(), 소멸자 | 이 Engine이 만진 세션을 정리, 기본 other |
| 인증 API agent.sessions.end | 호출 앱 소유 세션만 종료, 기본 other |
| API close(), daemon 정상 종료 | 클라이언트별 활성 세션 종료, other |
| MCP new_session=true | 교체 전 대화 종료, clear |
| MCP 연결 종료·HTTP DELETE·서버 종료 | 그 연결의 대화 종료, other |
| MCP stdio EOF·파이프 단절·SIGINT·SIGTERM | serveStdio 정리와 연결 종료, other |

허용 사유는 clear, resume, logout, prompt_input_exit, other, bypass_permissions_disabled이다. 사용자에게 새 로그아웃 UI나 권한 모드를 제공한다는 뜻은 아니다. C++ 호스트/API가 실제 동작에 맞는 사유를 선택한다. 잘못된 사유는 취소나 정리 전에 거부한다.

최초 run/runQueued/compact 활성화가 끝난 주 세션에만 훅을 보낸다. 생성·조회만 한 세션, 아직 실행하지 않은 대기 작업, 새 Engine에서 열지 않은 과거 기록에는 보내지 않는다. 직접 실행한 네이티브 도구만 있는 세션도 정리는 하지만 SessionStart/End를 새로 만들지 않는다. 자식 Engine은 sessionStartHooks=false로 기존 SubagentStart/Stop을 사용한다.

동일 활성화에 여러 종료 호출이 겹치면 정리를 직렬화하고 훅은 한 번만 실행한다. 오류·시간 초과·continue:false·block도 그 활성화의 종료를 되돌리지 않는다. 이후 같은 세션 ID로 실행하면 기록과 미소비 입력 큐를 유지한 채 SessionStart(resume)를 보낸다. close는 Engine 전체의 새 실행·생성·분기·네이티브 도구 접수를 막으며 반복 호출은 추가 훅을 실행하지 않는다. endSession은 기록을 삭제하지 않는다.

0.24.0의 MCP 새 대화와 C++/API clear는 백그라운드 작업을 보존하고 새 ID에 SessionStart(clear)를 즉시 실행한다. 이전 기록은 유지한다. 세부 순서·부분 실패·공유 MCP 연결의 경계는 [SessionClear.md](SessionClear.md)를 따른다. endSession(id,"clear") 자체는 기존 종료 API이며 신규 대화를 만들지 않으므로 실제 초기화에는 clearSession을 사용한다.

## 정리와 동시성

종료 입장 후에는 같은 세션의 새 Engine 실행·입력 게시·직접 네이티브 도구 호출을 거부한다. 실행 중인 작업과 직접 네이티브 호출에는 취소를 전달한다. 아직 시작하지 않은 Engine 작업은 QThreadPool에서 제거하여 취소 결과를 확정하므로 다른 세션의 긴 실행이 끝날 때까지 기다리지 않는다. worker가 실행권을 얻은 작업은 취소를 확인하고 기존 transcript 복구·저장을 끝낸다. 실행 시작 여부와 task 수명을 Engine mutex로 함께 보호하여 이미 삭제한 runnable을 다시 제거하지 않는다. Qt의 큐 제거·소유권·ABA 주의사항은 [QThreadPool::tryTake 공식 문서](https://doc.qt.io/qt-6.8/qthreadpool.html#tryTake)를 확인했다. 다른 세션은 계속 실행할 수 있다.

명시적인 API 종료는 별도의 제어 worker를 사용한다. 동일 클라이언트·세션에 이미 접수된 일반 요청을 취소하고 현재 수행 중인 요청의 Engine 진입·정리가 끝나기를 기다린다. API 큐에서 아직 실행하지 않은 요청은 나중에 worker를 얻어도 취소를 확인하고 실행하지 않는다. 종료 중 새 요청은 model_in_use이다. API 응답 연결 하나를 닫는 것만으로 영속 세션을 종료하지는 않는다.

주 실행의 transcript lease를 해제한 뒤 네이티브 배경 셸의 프로세스 그룹을 중단하고 종료를 기다린다. 소유한 자식 에이전트에는 취소를 요청한다. 기존 AgentStop 계약상 개별 자식의 최종 결과까지 기다린다는 보장은 없다. 전체 Subagents 호스트 소멸/close는 별도로 worker를 합류한다. 이러한 정리는 모델 권한 규칙과 독립적인 호스트 작업이다. 외부 MCP 서버·임의 사용자 자원을 자동 폐기하지 않는다.

훅은 Engine mutex와 transcript lease 밖에서 실행한다. 같은 세션의 session(), sessionMetadata(), queuedInputs() 조회가 가능하다. 자신의 이벤트/훅 안에서 endSession/close/소멸을 호출하거나 동기 대기하지 않는다. 신뢰하는 C++ 콜백이 이런 재진입 금지 계약을 어기면 교착할 수 있다. 참조를 캡처한 훅의 대상 객체는 Engine 종료까지 살아 있어야 한다.

## 시간 예산과 결과

`EngineOptions::sessionEndTimeoutMs`의 기본값은 1500ms, 허용 범위는 1~600000ms이다. 한 세션의 모든 종료 훅이 공유하는 취소 토큰을 watchdog이 취소한다. 명령 훅의 설정 timeout도 유지하며 둘 중 먼저 만나는 제한이 적용된다. 명령 대기열에서 아직 시작하지 않은 작업도 공통 취소를 확인한다. 종료를 요청한 run의 이미 취소된 토큰을 재사용하지 않는다.

호출자 취소는 종료 입장 전에 검사한다. 입장한 종료는 독립적으로 정리를 완료하며 이후 연결 취소로 되돌리지 않는다. 훅 시간 예산은 주 실행/자식/셸 정리 시간, 프로세스 시작·강제 종료 비용을 포함하는 전체 wall-clock 상한이 아니다. C++ 콜백·모델·사용자 도구가 취소를 무시하면 강제로 선점하지 못한다. 여러 활성 세션을 닫는 호스트의 예산은 세션마다 적용된다. SIGKILL, 크래시, 전원 차단에서는 훅 실행을 보장하지 않는다.

```cpp
auto report = engine.endSession(id, "logout");
// {session_id, reason, ended, timed_out, diagnostics}
auto shutdownReports = engine.close();
```

ended는 활성화가 종료되었는지, timed_out은 훅의 공통 예산 소진 여부이다. diagnostics에는 명령 stdout/stderr의 기존 크기 제한, 비차단 오류, 예외와 무시한 제어 결과가 포함된다. JSON block·continue:false·additionalContext·initialUserMessage는 새 메시지나 실행을 만들지 않는다. CLI에서 같은 객체를 받고 C++ close는 세션별 결과 배열을 반환한다. API 호스트 close와 MCP 연결 정리는 반환 진단을 별도 영속 로그/원격 알림으로 보관하지 않는다. 현재 보고서가 영속 감사 기록을 대신하지 않는다.

```json
{"hooks":{"SessionEnd":[{"matcher":"clear|other","hooks":[{"type":"command","command":"/private/save-session.sh","timeout":1}]}]}}
```

명령 stdin에는 공통 session_id·cwd·transcript_path·permission_mode와 reason, SDK 확장 model·빈 run_id가 있다. matcher는 reason을 검사한다. 일반 종료 2도 비차단 진단이다. 설정은 기존 --agent-hooks/--hooks의 호스트 전용 파일 규칙을 따른다. 원격 클라이언트가 명령·시간 예산을 바꾸지 못한다. 프로세스 로컬 once는 계속 세션 ID 기준이며 end/resume이 once 기록을 초기화하지 않는다.

## 근거와 검증

고정 참조 c8cd253554319f32ff64ff7000636199f720c9bc의 utils/hooks.ts(executeSessionEndHooks, executeHooksOutsideREPL, getSessionEndHookTimeoutMs), utils/gracefulShutdown.ts, commands/clear/conversation.ts, entrypoints/sdk/coreSchemas.ts의 실제 호출·출력·사유를 읽었다. 참조의 기본 1.5초 종료 예산과 비차단 정리 의미를 사용한다. JS 실행기나 새 생산 의존성을 도입하지 않고 기존 Qt JSON·QThreadPool·명령 프로세스 실행기와 표준 C++ 스레드를 재사용한다.

tests/session_end_tests.cpp는 활성화별 중복 방지·기록 보존·재개·직접/대기 실행 취소·네이티브 도구 취소·API 소유권·접수 대기 요청 취소·예외·실제 명령의 공통 예산을 검사한다. tests/mcp_server_tests.cpp는 교체/연결별 정리와 중복 종료를 검사한다. tests/command_hooks_wire.py는 인증 HTTP·native IPC CLI·MCP HTTP DELETE·공식 SDK stdio 종료·SIGINT/SIGTERM 및 선택적 실제 Qwen 추론을 검사한다. 실행 결과는 [Verification.md](Verification.md)에 기록한다. 전체 훅 및 하네스 호환은 여전히 [HarnessParity.md](HarnessParity.md)의 partial이다.
