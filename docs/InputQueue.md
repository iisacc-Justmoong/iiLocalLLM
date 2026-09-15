# 대화별 입력 큐 (0.13.0)

0.24.0의 transferNotifications는 선택된 완료 알림의 ID·내용을 유지하고 목적지 순번을 새로 부여한다. 목적지 기록 후 원본을 확인 처리하며 예외·재시도·세션 초기화 계약은 [SessionClear.md](SessionClear.md)에 있다.

대화 중 입력을 영속화하고 기존 실행에 전달하는 C++ SDK 계약이다. 참조는 고정 Claude Code 분석본 c8cd253554319f32ff64ff7000636199f720c9bc의 `types/textInputTypes.ts`, `utils/messageQueueManager.ts`, `utils/queueProcessor.ts`이다. C++과 Qt의 QLockFile·QSaveFile을 재사용하며 새 런타임 의존성은 없다.

`InputQueue`는 대화 ID별 입력을 별도 개인 저장소에 원자적으로 보관한다. 입력은 `text`, `kind`(prompt/notification), `priority`(now/next/later), `context_paths`이다. prompt의 기본 우선순위는 next, notification은 later이다. 같은 종류·우선순위 안에서는 저장 순서대로 처리한다. 입력 ID·순번은 저장소가 생성하며 호출자가 지정할 수 없다. JSON·타입·용량·경로 오류와 손상된 상태를 거절한다.

`Engine::enqueueInput`은 실행 중에도 입력을 받는다. now는 현재 모델·도구 연산에 협력 취소를 전달하고, 미완료 도구 결과를 중단 기록으로 짝지은 뒤 새 입력으로 이어간다. 실행 자체의 취소 토큰은 유지하며 사용자 취소·종료는 연결된 모든 연산에 전달한다. now는 강제적인 네이티브 스레드 종료가 아니다. 이미 별도 실행으로 분리된 백그라운드 셸은 이 취소에 포함되지 않으며 TaskStop으로 제어한다. next는 현재 도구 호출 묶음을 마친 뒤 다음 모델 요청 전에 들어간다. later는 현재 답변·Stop 훅이 끝난 경계에서 들어간다. 우선순위로 기존 실행의 턴·컨텍스트·권한 제한을 우회하지 않는다.

입력은 한 경계에서 가장 높은 우선순위 입력과 같은 종류만 최대 16개 전달한다. prompt와 notification을 같은 묶음에 섞지 않는다. 전달은 원본 대화에 기록한 뒤 큐에서 확인 처리한다. 기록과 큐 확인 사이에 프로세스가 중단되어도 동일 입력 ID·종류·본문·메타데이터와 user 역할을 원본에서 확인해 중복 삽입을 막는다. 같은 ID의 다른 기록은 protocol_error로 거절하고 큐에 남긴다. 저장 실패를 성공으로 표시하지 않는다. 큐의 저수준 `deliver` 콜백은 영속 기록만 수행하며 같은 큐 재진입이나 사용자 관찰자 호출을 금한다. Engine의 Message·InputDelivered 이벤트는 큐 잠금을 해제한 뒤 전달한다.

`queuedInputs`는 우선순위 순으로 페이지 조회하고, `removeInput`은 아직 큐에 남은 입력을 제거한다. 이미 전달된 입력이나 now가 이미 일으킨 취소를 되돌리는 기능은 아니다. 원본 대화는 재개·압축 후에도 보존하며 fork는 대화 기록만 복사하고 부모의 미전달 입력은 가져가지 않는다. 알림 텍스트는 외부 데이터라는 표시와 함께 모델에 전달한다.

유휴 상태의 큐는 `runQueued`로 시작한다. `RunRequest.prompt`는 비우고 모델·워크스페이스는 저장된 대화에서 사용한다. 생성 옵션·최대 턴은 기존 run 계약을 따른다. 입력 없는 호출은 거절한다. 실행 종료와 경합해 남은 입력 또는 턴 한도로 남은 입력은 큐 조회 후 다시 시작할 수 있다. 자동 유휴 기동, 셸 완료 알림 생산, 첨부·slash/bash 모드·서브에이전트 수신자와 Sleep 깨우기는 별도 미구현 항목이다. 0.22부터 입력별 UserPromptSubmit 훅을 제공하며 [InputLifecycle.md](InputLifecycle.md)의 차단·중단·추가 문맥 계약을 적용한다. 기존 BeforeModel 훅의 text는 RunRequest.prompt이며 runQueued에서는 빈 값이다. 개별 큐 입력은 InputDelivered 이벤트와 세션 메시지에 보존된다. 전체 하네스 대응 상태는 partial이다.

기본 상한은 대화당 256개, 입력당 65,536 UTF-16 코드 단위, 상태 8 MiB, 잠금 대기 5초이다. 호스트는 상태 경로를 도구가 조작할 워크스페이스와 분리해야 한다. 인증 API는 이 분리를 강제하지만 일반 Engine과 MCP stdio의 기본 경로는 호스트가 구성한다. 대화의 지침·경로 규칙은 전달 시 다시 확인한다. 큐의 존재·입력 전달은 모델이 작업을 성공적으로 완료했다는 증거가 아니다.

## C++와 앱 전송

```cpp
auto receipt = engine.enqueueInput(sessionId, {{"text", "다음 파일을 확인하라"}, {"priority", "next"}});
auto pending = engine.queuedInputs(sessionId);
iiLocalLLM::agent::RunRequest request{sessionId, {}};
request.maxTurns = 8;
auto run = engine.runQueued(request, observe); // 현재 실행이 없는 대화에서 호출한다.
```

응답의 `input`에는 정규화한 입력과 `id`, `sequence`가 있고 `revision`은 큐 변경 번호다. 수락된 실행이 있으면 `active_run_id`도 반환한다. 이 ID는 실행 완료나 입력 소비를 보장하지 않는다. 목록은 전체 `count`, 페이지 `inputs`, 필요 시 `next_offset`과 `revision`을 반환한다. 페이지 사이에 쓰기가 있으면 동일 스냅샷을 보장하지 않으므로 revision으로 변경 여부를 확인한다.

인증된 `agent.inputs.enqueue/list/remove/run`은 [AgentAPI.md](AgentAPI.md)에 정의한다. 등록·조회·삭제에는 별도의 제한된 작업자 풀이 있어 에이전트 실행 풀이 포화되어도 호출을 처리한다. 전송 자체의 연결·요청 한도는 여전히 적용된다. `run`은 기존 실행 슬롯·기한·연결 취소 계약을 따른다.

`iillm --auth-file TOKEN agent inputs ACTION SESSION [PARAMS_JSON_FILE]`에서 ACTION은 enqueue/list/remove/run이다. enqueue 파일은 `{"text":"새 지시","priority":"now"}`, remove 파일은 `{"input_id":"큐가 반환한 ID"}`이다. run 파일에는 선택적 options/max_turns/context_paths를 넣고 prompt는 넣지 않는다. 실패한 실행은 종료 코드 1이다.

Engine을 설정한 MCP 서버는 같은 네 동작을 `iiLocalLLM.agent.inputs.*` 도구로 제공한다. 대화는 현재 연결에 묶이며 임의 session_id를 받지 않는다. list는 읽기 전용이며 enqueue/remove/run은 호스트의 도구 정책을 적용한다. 실행 파일에서 필요하면 `--allow 'iiLocalLLM.agent.inputs.*'`를 지정한다. 진행 중 입력 제어 세 도구는 실행 잠금 밖에서 처리하지만 스키마·권한·대화 소유권은 그대로 검증한다. 연결 종료 후 큐 파일은 남지만 새 MCP 연결이 이전 대화에 자동 연결되지는 않는다.

## 검증 범위

`tests/input_queue_tests.cpp`는 우선순위와 종류 분리, 스레드·프로세스 간 게시, transcript 게시 후 확인 실패 복구, 중복 ID 충돌, 실행 중 next/now, later 경계, 명시적 취소·턴 한도 뒤 보존, 이벤트 콜백 재입력과 손상·symlink 저장 거절을 검사한다. API·MCP 단위 검사는 실행 중 별도 요청의 긴급 입력과 앱·연결 격리를 확인한다. `tests/input_queue_runtime_smoke.cpp`는 고정 Qwen3 8B에서 실제 Read 후 next 입력, 실행 중인 셸의 PID·준비 파일 확인 뒤 now 입력, 셸 종료와 후속 Read의 미리 알 수 없는 파일 값을 검사한다. 공식 MCP Python SDK는 전송 검사에만 사용한다. 전체·단독·설치본 결과는 [Verification.md](Verification.md)에서 구분한다.

0.22의 prepare/persist 오버로드는 외부 준비 콜백을 queue.lock 밖에서 실행하고 별도 delivery.lock으로 소비자를 직렬화한다. 기존 persist 오버로드도 같은 소비자 잠금을 사용한다. 철회된 입력은 준비 뒤 저장하지 않으며 persist=false는 현재 항목의 확인 후 배치를 끝낸다. 새로 게시한 입력은 다음 배치에서 선택한다. 훅의 외부 효과와 원본/큐 저장은 단일 트랜잭션이 아니다. 입력 판정이 이미 transcript에 저장되어 있으면 재시도에서 훅을 다시 실행하지 않는다.

0.33.0의 비동기 명령 훅은 세션/연결별로 완료 결과를 보관하고 문맥을 notification/next로 전달한다. asyncRewake 종료 코드 2는 횟수를 제한한 유휴 실행을 요청한다. 세션 종료·clear는 훅을 취소·정리하며, 자식 Engine은 실행이 끝난 뒤 자동 기동하지 않는다. 상태/취소 메서드와 기존 요청별 권한·콜백을 재사용하지 않는 경계는 [AsyncHooks.md](AsyncHooks.md)를 따른다.
