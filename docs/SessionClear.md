# 대화 초기화와 백그라운드 작업 보존

0.24.0은 C++ Engine::clearSession, 인증 API agent.sessions.clear, MCP iiLocalLLM.agent.clear 및 agent.run의 new_session을 연결한다. C++ Tool과 Session의 공개 구조가 바뀌므로 소비자는 0.24 헤더·라이브러리를 함께 다시 빌드한다. 기존 Qt·표준 C++로 구현하며 새 생산 의존성이나 Python 실행기는 추가하지 않는다.

| 진입점 | 동작 |
|---|---|
| engine.clearSession(id) | 해당 세션의 접수된 실행과 네이티브 호출을 취소·대기하고 새 세션을 반환 |
| agent.sessions.clear {session_id} | 인증된 클라이언트의 세션만 초기화; 새 세션 상한을 먼저 예약 |
| MCP iiLocalLLM.agent.clear {} | 같은 연결의 진행·대기 호출을 취소·대기; 모델을 실행하지 않음 |
| MCP iiLocalLLM.agent.run {new_session:true,...} | 새 대화로 초기화한 뒤 입력 실행; 기존 실행 잠금 뒤에서 처리되므로 즉시 중단에는 별도 clear 도구 사용 |

0.26의 순서는 기존 실행 정리 → SessionEnd(clear) → 새 세션 게시 → 런타임 권한 상속 → 백그라운드 소유권 전환 → SessionStart(clear)이다. 새 세션은 같은 모델·작업 디렉터리·호스트 system prompt를 사용하고 parent_session_id로 이전 기록을 가리킨다. 새 ID이므로 이전 대화·압축·읽은 파일 상태·선택한 도구·모델 문맥 ID를 재사용하지 않는다. 실제 추론은 다음 run까지 실행하지 않는다. 시작 훅의 additionalContext는 새 기록에, initialUserMessage는 새 입력 큐에 저장한다. 이후 같은 Engine의 run은 시작 훅을 반복하지 않는다.

승인된 세션/cliArg 권한·모드·디렉터리 바인딩은 새 ID에 독립 복사한다. 일반 fork도 같다. 상속 실패 시 clear의 permissions 진단이나 fork 오류의 새 ID로 생성된 기록을 확인할 수 있다. 파일 설정은 공유되며 이후에도 다시 읽는다. [PermissionUpdates.md](PermissionUpdates.md)를 따른다.

이전 transcript·아티팩트·대기 사용자 입력·계획 Task/Todo는 이전 ID에 남는다. 새 세션으로 복사하지 않으며 이전 기록은 명시적으로 조회·재개할 수 있다. parent_session_id는 추적 정보이며 다른 클라이언트의 기록에 접근할 권한을 주지 않는다. 기존 v1/v2 transcript의 부모 필드 생략도 읽는다.

백그라운드 셸과 background=true 자식 에이전트는 실행 중·종료 여부에 관계없이 새 소유자로 전환한다. 프로세스·자식 대화·작업 ID·출력 경로는 유지한다. 이전 ID의 출력/중지 권한은 사라지고 새 ID의 TaskOutput/TaskStop/AgentOutput/AgentStop이 같은 작업을 제어한다. 아직 전달하지 않은 자식 완료 알림만 안정된 입력 ID와 내용 그대로 옮긴다. 같은 자식을 여러 번 재실행한 경우에도 아직 소비하지 않은 각 완료 알림을 함께 옮기며 확인 처리된 알림 참조는 정리한다. 일반 알림과 사용자 입력은 그대로 남는다. foreground 자식은 부모 취소에 연결되어 종료되며 기록은 이전 소유자에게 남는다. 호스트 전체 종료는 살아 있는 백그라운드 작업도 취소한다.

각 작업 저장소의 session-owners.json은 프로세스 잠금 아래 QSaveFile로 원자적으로 교체한다. 원래 작업 파일의 부모/세션은 실행 기원이며 현재 소유권은 별도 표로 판정한다. Tool::transferSession은 신뢰한 C++ 호스트 전용 콜백이고 도구 스키마·모델·원격 호출에는 노출하지 않는다. 같은 저장소의 제어 도구 하나에만 설치한다. 임의 C++ 호스트가 직접 이 콜백을 호출할 때도 이전/새 세션의 접수 작업을 먼저 정리해야 한다.

자식 종료와 소유권 커밋은 같은 잠금으로 직렬화한다. 알림 큐 대기 중에는 자식 잠금을 잡지 않는다. 알림은 목적지 기록 후 원본을 확인 처리한다. 대상이 가득 차거나 저장에 실패하면 원본 알림을 유지하고 오류를 반환한다. Subagents::transferSession(from,to) 재호출 또는 저장소 재개는 notification_refs에 보존한 각 실행의 알림 ID·소유자(구 기록은 notification_owner)와 현재 소유자를 비교해 남은 이동을 재시도한다. 목적지 큐의 순번은 바뀔 수 있으므로 notification 재전달은 순번만 제외하고 안정 ID·종류·내용·우선순위·context_paths와 기록된 메시지를 모두 비교한다. 사용자 프롬프트의 순번 충돌 검사는 유지한다.

clear 결과는 previous_session_id, session_id, complete, end, background, diagnostics, 선택적 start_diagnostics이다. end에는 기존 비차단 종료 훅 결과가 들어간다. 신규 세션 생성 실패는 빈 session_id, 소유권 이동/시작 훅 실패는 게시된 새 ID와 complete=false를 반환한다. MCP new_session은 이 경우 모델을 실행하지 않고 clear 결과를 포함한 오류를 반환한다. 새 세션을 얻었다면 그 ID와 진단을 보존해야 한다. 같은 원본으로 clear를 다시 호출하면 또 다른 세션을 만들므로 요청 전체를 멱등 재실행으로 간주하지 않는다.

여러 작업 저장소·두 입력 큐·transcript는 하나의 원자적 트랜잭션이 아니다. 임의 시점 프로세스 중단의 전체 초기화 복구나 정확히 한 번의 외부 훅 실행을 보장하지 않는다. 부분 이동 시 이전·신규 기록을 모두 보존한다. C++ 시작 훅 실패는 현재 Engine에서 다음 run 시 clear 소스로 재시도하며, 새 호스트가 과거 세션을 여는 경우에는 resume이다. 콜백의 외부 효과는 재시도될 수 있다. API는 접수 뒤 정리를 독립 토큰으로 마치며 예산을 넘겨도 새 ID를 숨기지 않고 deadline_exceeded를 반환한다. 전송 연결이 사라지면 클라이언트가 응답을 받지 못할 수 있다.

clear/close/endSession을 자신의 실행·도구·이벤트·훅에서 동기 호출하지 않는다. clear의 시작 훅은 새 transcript lease를 사용한다. close는 진행 중 clear의 게시·전환·시작 처리를 기다린 뒤 새 활성화도 종료한다. 협조하지 않는 C++ 콜백을 강제 선점하지 않으며 SessionEnd의 독립 예산과 비차단 제어 의미는 [SessionEnd.md](SessionEnd.md)를 따른다.

참조는 c8cd253554319f32ff64ff7000636199f720c9bc의 commands/clear/conversation.ts, commands/clear/caches.ts이다. 백그라운드 보존·SessionEnd(clear)·새 ID·즉시 SessionStart(clear)는 실제 호출을 확인했다. 참조의 UI 상태·팀·LSP·git 캐시·worktree·플러그인 재초기화 전체는 아직 구현되지 않았다. SDK의 MCP 연결은 여러 소비자가 공유하므로 세션 초기화가 다른 소비자의 서버 연결을 끊지는 않는다. 가중치/유휴 KV의 실제 메모리 해제는 기존 Service의 TTL/LRU 정책이며 clear가 강제 GPU 메모리 반환을 뜻하지 않는다. 전체 하네스 대응 상태는 계속 partial이다.

tests/session_clear_tests.cpp, input_queue_tests.cpp, shell_tasks_tests.cpp, subagent_tests.cpp, mcp_server_tests.cpp가 문맥 분리·실제 프로세스 보존·연속 전환·완료 경쟁·큐 오류/재시도·호스트 재개·진행 호출 취소·종료 경합·인증/상한을 검사한다. tests/command_hooks_wire.py는 API·CLI·MCP HTTP/공식 stdio와 선택적 실제 모델의 clear 시작 문맥을 검사한다. 설치 소비자는 동일 공개 헤더로 다시 빌드하며 실제 결과는 [Verification.md](Verification.md)에 기록한다.

0.33.0의 비동기 명령 훅은 세션/연결별로 완료 결과를 보관하고 문맥을 notification/next로 전달한다. asyncRewake 종료 코드 2는 횟수를 제한한 유휴 실행을 요청한다. 세션 종료·clear는 훅을 취소·정리하며, 자식 Engine은 실행이 끝난 뒤 자동 기동하지 않는다. 상태/취소 메서드와 기존 요청별 권한·콜백을 재사용하지 않는 경계는 [AsyncHooks.md](AsyncHooks.md)를 따른다.
