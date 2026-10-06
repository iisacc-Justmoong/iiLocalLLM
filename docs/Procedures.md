# 절차 반환 채널과 중복 자식 실행 방지

0.53은 `agent::Procedures`를 추가한다. Engine의 실제 실행 경계를 구조화된 상태로 전달하고, 호스트가 절차 반환값을 다음 단계에 반영하기 전에 처리할 수 있다. 관측은 기본 활성화이며 호스트 응답 대기는 명시적 설정이다. C++23을 사용하고 기존 JSON·전송 ABI를 유지하며 새 생산 의존성은 추가하지 않는다. 공개 구조체 변경에 따라 0.53 헤더와 라이브러리로 소비자를 다시 빌드한다.

0.54는 decision_input/decision 절차와 RunStatus::Deferred를 추가한다. 호스트의 정량 입력·확률 추론·기대가치·실행 보류 계약은 [DecisionGate.md](DecisionGate.md)를 따른다.

## 절차와 상태

`EventKind::Procedure`는 JSON에서 `event: "procedure"`이다. `data`는 `iisacc.procedure/1` 객체이며 다음 정보를 포함한다.

| 필드 | 의미 |
|---|---|
| `procedure_id` | 한 번의 절차 실행에 부여한 UUID |
| `parent_procedure_id` | 부모 실행 또는 도구 절차 ID |
| `owner_session_id` | 호스트 제어 권한이 속한 부모 대화 |
| `session_id`, `run_id` | 실제 수행 중인 대화와 실행 |
| `agent_id` | 별도 자식 에이전트일 때 해당 ID |
| `turn`, `tool_call_id` | 모델 턴과 도구 호출 연결 |
| `kind` | 실제 절차 종류 |
| `phase` | started, returned, waiting, completed, failed, cancelled, expired |
| `input` | 해당 절차 입력 또는 입력 요약 |
| `output` | 절차가 실제로 반환한 원래 값 |
| `effective_output` | 호스트 응답을 적용한 값 |
| `replaceable` | 해당 반환값의 교체 가능 여부 |
| `controllable` | 진행·교체·취소 응답 수락 여부; run은 false |
| `action`, `reason` | continue/replace/cancel 및 호스트의 이유 |
| `sequence` | 상태가 갱신될 때 증가하는 채널 순번 |
| `created_at`, `updated_at`, `duration_ms` | UTC 시각과 완료된 절차의 경과 시간 |
| `error`, `error_code` | 확정된 오류 또는 시간 초과 |

`run`은 실행 전체, `input`은 최초 입력 준비, `context`는 모델 문맥 조합, `compaction`은 압축 후보 반환, `model`은 모델 응답, `tool`은 도구 호출 전체, `tool_validation`은 입력 검증, `tool_preparation`은 대상 준비, `permission`은 실행 허용 판단, `tool_execution`은 실제 실행과 출력 검증, `completion`은 일반 최종 답변 반환이다. 시작 이벤트는 실제 함수 실행 전에 발생하며 반환 이벤트는 실행 이후, 다음 단계 반영 전에 발생한다. 기존 입력 큐·메모리·훅·Task 전용 상태 이벤트도 계속 제공한다. 이번 절차 채널이 모든 유지 관리 작업을 새 절차로 재구현하는 것은 아니다.

| 절차 | 반환 `output` |
|---|---|
| run | RunResult: status, text, turns, usage, error_code, error_message 및 실행/세션 ID |
| input | 준비된 text |
| decision_input | 호스트 value_unit/estimates/evidence; 실제 후보·정책은 input에 고정 |
| decision | iiDecision 계산 후 execute/defer, 선택 ID, 후보별 확률·기대가치·이유; 교체 불가 |
| context | model, context_id, message_count, tools, instruction_fingerprint |
| compaction | 기존 CompactionCheckpoint JSON |
| model | text, tool_calls: id/name/arguments |
| tool_validation | valid, 두 번째 검증에서는 훅 적용 후 arguments도 포함 |
| tool_preparation | 실제 준비한 ToolDefinition JSON |
| permission | allowed, reason, 최종 arguments와 준비된 target |
| tool_execution | 실행·출력 검증 후 text, data, is_error |
| tool | 훅·큰 결과 처리까지 적용한 text, data, is_error |
| completion | 일반 최종 답변 text |

`completed`는 해당 절차의 반환 처리가 완료되었다는 뜻이다. 전체 작업 성공은 `run`의 확정 결과로 판단한다. `payload_omitted:true`는 실패 기록이 용량을 초과하여 입력/출력 본문을 생략한 경우이다.

## 반환값 개입

`ProcedureOptions.intercept`에 지정한 절차는 반환 시 `waiting` 상태가 되며 호스트 응답을 기다린다. 빈 목록은 대기 없이 관측한다. C++ `ProcedureCallback`을 연결하면 해당 콜백의 반환을 사용한다. 콜백과 이벤트 수신자에서 동시에 응답하면 먼저 확정된 응답이 유지된다. 콜백은 채널·Engine·세션을 파괴하거나 자신의 실행 완료를 동기 대기하면 안 된다.

| 동작 | 계약 |
|---|---|
| `continue` | 원래 반환값을 다음 단계에 전달 |
| `replace` | 검증된 호스트 반환값을 다음 단계에 전달 |
| `cancel` | 현재 실행을 취소 결과로 종료 |

교체 가능한 반환값은 최초 `input`의 `{text}`, `model`의 `{text, tool_calls}`, 최종 `tool`의 `{text, data, is_error}`, 일반 `completion`의 `{text}`이다. 나머지 절차는 관측·진행·취소를 지원하고 내부 검증/권한 결과를 교체하지 않는다. `run`은 전체 결과를 관측하는 경계이며 응답 대기나 콜백 개입 대상이 아니다.

모델 반환 교체는 도구 ID·이름·JSON 구조·턴당 호출 수·텍스트 한도를 재검사하고 실제 도구는 기존 스키마·권한·훅을 그대로 통과한다. 관측한 모델 사용량은 호스트가 덮어쓰지 않는다. 도구 반환 교체는 구조와 성공한 `data`의 출력 스키마를 검사하며 native content·metadata를 호스트 입력으로 교체하지 않는다. 모델용 결과가 바뀌어도 이미 발생한 외부 효과가 취소되거나 재실행되지는 않는다. 최종 답변 교체 시 적용된 Assistant 기록을 추가하여 최종 결과와 대화의 마지막 답변을 일치시킨다.

모델 스트림은 개입 이전의 관측 출력이다. 앱은 완료 절차의 확정된 값을 최종 결과로 사용해야 한다. `output`과 `effective_output`의 차이로 호스트 변경을 구분한다. 절차 기록은 용량 제한을 가진 메모리 상태이며 재시작 후 영속 감사 기록을 제공하지 않는다. 대화 및 자식 실행 기록은 기존 저장 계약을 따른다.

```cpp
agent::EngineOptions options;
options.sessionsDirectory = privateSessions;
options.procedures = std::make_shared<agent::Procedures>(
    agent::ProcedureOptions{},
    [](const QJsonObject& step, const CancellationToken&) {
        agent::ProcedureResponse response;
        if (step["kind"] == "completion") {
            response.action = agent::ProcedureAction::Replace;
            response.output = {{"text", "호스트가 확정한 답변"}};
        }
        return response;
    });
// Engine 이벤트는 앱이 자신의 UI 스레드로 전달한다.
```

## 앱 API·IPC·HTTP·CLI

인증 API의 `ApiOptions.procedures`로 대기 경계와 한도를 설정한다. 채널은 인증된 client ID별로 분리되며 모델/API 입력으로 설정할 수 없다. 자식은 부모의 채널을 공유하고 부모 owner ID에 연결되므로 부모의 목록에서 자식의 실제 절차를 볼 수 있다.

`agent.info.procedures`는 스키마, 설정된 intercept 목록, 교체 가능한 절차 종류와 응답 시간 제한을 제공한다.

`agent.procedures.list`는 `session_id`, 선택 `after`와 `limit`(1..128)을 받는다. `procedures`, `next_cursor`, `has_more`를 반환한다. 순번은 상태 갱신 순번이며 목록은 보존 중인 절차의 최신 상태 스냅샷이다. 모든 과거 전환을 재생하는 로그가 아니다. 완료 기록은 용량 한도에 따라 퇴거한다.

`agent.procedures.respond`는 `procedure_id`와 `response`를 받는다. 잘못된 교체값은 응답 확정 전에 거부하므로 호스트가 수정하여 다시 제출할 수 있다. 동일 응답 재전송은 보존 중인 기록에서 `replayed: true`로 확인하고 충돌 응답은 거부한다. 수락은 반환값 응답의 확정이며 전체 작업의 완료를 뜻하지 않는다.

```json
{
  "procedure_id": "반환 이벤트의 UUID",
  "response": {
    "action": "replace",
    "output": {"text": "호스트 답변", "tool_calls": []}
  }
}
```

두 메서드는 추론·일반 요청 worker와 transcript lease를 기다리지 않는 제어 경로이다. HTTP의 `/v1/rpc`, SSE 이벤트와 native IPC에서 같은 계약을 사용한다. 다른 인증 클라이언트의 세션·절차는 접근할 수 없다.

daemon은 workspace 밖의 비공개 호스트 파일을 `--agent-procedures FILE`로 읽는다. 예시는 `{"intercept":["model","tool"],"timeout_ms":120000}`이다. CLI는 `iillm --auth-file TOKEN_FILE agent procedures list PARAMS_JSON_FILE` 및 `respond PARAMS_JSON_FILE`을 사용한다.

## MCP

에이전트 Engine을 연결한 MCP 서버는 `iisacc/procedures` experimental capability와 `iisacc/procedures/list`, `iisacc/procedures/respond` 제어 메서드를 제공한다. 목록은 현재 연결의 대화를 사용하고 호출자가 세션 ID를 지정하지 못한다. 응답도 동일 연결의 owner에 속한 절차만 허용한다. 제어 메서드는 모델용 도구로 노출하지 않는다. `iillm-mcp --model ... --procedures FILE`로 호스트 대기 경계를 설정한다.

## 동일 자식 작업의 중복 방지

`SubagentOptions.deduplicateRequests` 기본값은 true이다. 같은 부모 실행 ID에서 같은 새 `Agent` 요청 인자가 반복되면 새 자식 실행 대신 기존 ID와 상태를 반환한다. 부모의 직접 도구 반복, 일반 Read/Bash의 반복과 다른 부모 실행은 이 자동 중복 제거 범위가 아니다.

`Agent`에 `idempotency_key`(1..128자)를 지정하면 같은 부모 세션에서 API 재시도와 호스트 재시작 이후에도 같은 새 자식 요청을 식별한다. 키는 최초 실행 기록과 함께 게시된다. 같은 키에 다른 인자를 제출하면 충돌을 반환한다. 실행 중 중복은 `async_launched`, `finished:false`, `reused:true`와 기존 ID를 반환하고, 종료된 중복은 최초 실행의 결과와 `finished:true`를 반환한다. 이미 재개한 자식이라도 원래 키의 재시도는 최초 결과를 사용한다. 재시작 시 미확인 작업은 interrupted로 복구하며 자동으로 실행하지 않는다.

`resume`는 별도 의도적 호출이므로 `idempotency_key`와 함께 지정할 수 없다. fork 스킬과 이름 있는 지속 팀원의 호출은 이 새 자식 중복 제거 범위에 포함하지 않는다. 키 보존은 자식 기록의 수명과 동일하며 일반 외부 부작용 전체의 exactly-once 보증은 아니다.

재사용 응답의 `finished`와 `result`는 최초 요청의 결과이며 `execution_phase`는 현재 자식의 상태이다. 최초 요청이 완료된 뒤 의도적으로 재개 중인 자식은 `finished:true`, 최초 result와 `execution_phase:running`을 함께 반환할 수 있다.

## 한도·검증

기본 대기는 120초, 보존 기록은 1,024개, 기록 한도는 8MiB, 채널 총 한도는 32MiB이다. API와 CLI MCP의 레코드 크기는 전송 한도에 맞춰 더 작게 제한한다. 만료·사용자 취소·연결 종료는 협력적 취소를 사용한다. C++ 사용자 콜백의 강제 선점은 제공하지 않는다.

`procedure_tests.cpp`는 모델/도구/최종 답변 교체, 검증 실패 후 재응답, 포화 중 제어, 다른 앱 격리, 시간 초과·취소와 자식 재시작 중복 제거를 검사한다. `agent_transport_tests.cpp`는 실제 loopback HTTP·SSE·IPC를 교차하여 반환값을 제어한다. `mcp_server_tests.cpp`는 MCP 연결 격리와 실행 중 제어 응답을 검사한다. 설치 소비자는 같은 공개 계약을 별도 설치 헤더·라이브러리로 검증한다. 실제 모델의 작업 분해 품질과 제품 앱 UI 연결은 별도 검증 범위이다.

`procedure_runtime_smoke.cpp`는 기존 로컬 Qwen2.5 GGUF를 실제 ServiceModel로 호출한다. 호스트가 부모 모델 반환에서 동일한 Agent 요청 두 개를 제출하고, 실제 자식 모델 반환과 최종 답변을 교체한다. 모델 반환 세 번(부모 두 번·자식 한 번), 자식 하나, `reused:true` 및 실제 생성 토큰을 검사한다. 모델이 스스로 작업을 분해하거나 중복을 인지한다는 증거는 아니다.
