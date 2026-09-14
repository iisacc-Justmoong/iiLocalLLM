# 대화 압축과 재개 (0.6.0)

iiLocalLLM의 에이전트는 로컬 모델의 실제 입력 토큰을 측정하고, 한도에 가까워지면 오래된 도구 결과를 줄이거나 대화를 요약한다. 원본 JSONL 메시지는 보존한다. 요약이 필요한 경우 같은 로컬 모델을 사용하며, 원본 도구를 다시 실행하지 않는다.

## 의존성과 기준 구현

기존 Qt 6.8.3, Service 스케줄러, llama.cpp 템플릿·토크나이저·추론을 재사용한다. 새로운 라이브러리나 Python 프로세스는 추가하지 않는다. 압축 체크포인트와 세션 모델 조합은 iiLocalLLM 고유 계약이므로 C++로 구현한다. 기존 의존성의 고정 버전·라이선스는 THIRD_PARTY_NOTICES.md를 따른다.

분석 기준은 Exhen 저장소의 `c8cd253554319f32ff64ff7000636199f720c9bc`이다. [autoCompact.ts](https://github.com/Exhen/claude-code-2.1.88/blob/c8cd253554319f32ff64ff7000636199f720c9bc/source/src/services/compact/autoCompact.ts)는 모델 창에서 요약 출력 예약량과 자동 압축 여유를 뺀 임계값을 계산하며, 연속 실패 3회 후 자동 시도를 멈춘다. [microCompact.ts](https://github.com/Exhen/claude-code-2.1.88/blob/c8cd253554319f32ff64ff7000636199f720c9bc/source/src/services/compact/microCompact.ts)는 오래된 도구 결과를 줄이면서 최근 결과와 호출 관계를 유지한다. [compact.ts](https://github.com/Exhen/claude-code-2.1.88/blob/c8cd253554319f32ff64ff7000636199f720c9bc/source/src/services/compact/compact.ts)는 요약·경계·보존 메시지를 만들고 파일 읽기 상태를 비운다. [grouping.ts](https://github.com/Exhen/claude-code-2.1.88/blob/c8cd253554319f32ff64ff7000636199f720c9bc/source/src/services/compact/grouping.ts)는 API 응답 단위로 그룹을 나눈다.

iiLocalLLM은 이 동작을 자체 세션 프로토콜에 맞춰 구현했다. 클라우드용 13,000/20,000 토큰 상수를 작은 로컬 컨텍스트에 적용하지 않는다. 요약 프롬프트·구현 코드를 복사하지 않으며 비공개 추론 과정도 요청하지 않는다.

## 실제 토큰 측정

`Service::measureConversation(request, cancellation)`은 `converse()`와 같은 입력 검증, 모델 선택·상주 정책, `RuntimeModel::prepareConversation()`을 사용한다. `ContextBudget.inputTokens`는 템플릿·시스템 프롬프트·도구 스키마를 포함한 실제 토큰 수이며 `contextTokens`는 현재 로드된 모델의 창 크기다. 한도를 넘는 입력도 수치로 반환한다. KV 컨텍스트를 만들거나 토큰을 생성하지 않는다. 호출자 취소·큐 거절·서비스 종료는 기존 스케줄러를 따른다. 측정 자체로 모델이 로드될 수 있다. 준비한 프롬프트 상태를 먼저 소멸한 뒤 모델 lease를 풀어, keep_alive=0의 즉시 해제에서도 모델을 참조하는 임시 상태가 먼저 종료된다.

`agent::ServiceModel`은 측정과 생성에 동일한 변환 함수를 사용한다. 사용자 정의 `Model`은 `measure()`에서 자신의 네이티브 수치를 제공해야 한다. 기본값 `nullopt`이면 자동 압축은 건너뛰며 수동 압축은 `RuntimeUnavailable`로 실패한다. 문자 수를 토큰 수로 추정하여 네이티브 예산처럼 사용하지 않는다. 입력 문자/메시지 수 등 Service의 기존 상한은 계속 적용된다.

## 자동 압축 순서

각 모델 호출 직전에 프로젝트 지침과 현재 대화 뷰를 조합하고 예산을 측정한다. 기본 임계값은 `(contextTokens - RunRequest.generation.maxTokens) × 0.85`이다.

1. 임계값을 넘으면 현재 세션 원문 조회 도구를 스냅샷에 추가하고, 그 스키마 비용까지 포함해 다시 측정한다.
2. 최근 그룹을 제외한 큰 도구 결과를 짧은 원문 참조로 바꾼다. 호출 ID·결과 ID·오류 상태는 유지한다. 충분히 줄면 micro 체크포인트를 저장한다.
3. 여전히 크면 오래된 완전한 그룹들을 요약한다. 한 요청에 들어가지 않으면 이전 요약과 다음 그룹을 함께 넣는 방식으로 이어서 요약한다. 각 후보를 네이티브 토크나이저로 검증하며 기본 최대 16회로 제한한다.
4. 요약·보존 메시지·프로젝트 지침·원문 조회 스키마를 합친 최종 입력을 재측정한다. 토큰 수가 실제로 감소하고 자동 임계값 안에 들어오는 경우에만 체크포인트를 저장한다.

그룹은 일반 메시지 하나 또는 Assistant의 도구 호출과 그 결과 전부다. 하나의 병렬 호출 묶음을 중간에서 자르지 않는다. 기본 최근 2개 그룹은 원문으로 유지한다. 최신 User 메시지가 요약 범위 안에 들어가더라도 그 메시지를 요약 다음에 원문 그대로 다시 배치한다. 긴 단일 사용자 작업에서도 최근 도구 수행을 보존하며 초기 요청을 유지할 수 있다.

각 요약 단계에 최신 사용자 요청 원문도 다시 제공하여 앞 단계 요약에서 요구가 빠져도 다음 단계가 원문을 참조할 수 있게 한다. 요약 입력은 메시지 ID를 포함하는 JSON 기록이며 실행 가능한 도구 목록은 비운다. `ModelRequest.summarizing`을 설정하고 `ServiceModel`은 `tool_choice=none`을 사용한다. 요약은 새 Assistant 작업 결과로 기록하지 않으며 일반 `model_delta` 스트림에도 섞지 않는다. 빈 요약이나 도구 호출을 담은 요약은 거절한다.

오래된 도구 결과가 먼저 제거되었다면 요약에도 참조가 들어간다. 모델이 그 결과의 정확한 내용이 필요할 때 원문 조회 도구를 사용해야 한다. 요약 품질은 모델에 의존하며, 수치·프로토콜 검증이 사실 충실성까지 보장하지 않는다.

## C++ 사용

```cpp
iiLocalLLM::agent::EngineOptions options;
options.sessionsDirectory = sessionsDirectory;
options.compaction.automatic = true;
options.compaction.keepRecentGroups = 2;
options.compaction.summaryMaxTokens = 512;
// Engine을 구성한 뒤 기존 run()은 자동 압축을 사용한다.
iiLocalLLM::agent::CompactRequest request;
request.sessionId = sessionId;
request.instructions = "Preserve exact release identifiers and unfinished work.";
auto handle = engine.compact(request, onEvent);
// handle.cancel(); 결과는 UI 스레드 밖에서 기다린다.
```

`CompactionOptions`는 호스트 설정이다. `automatic`, `clearOldToolResults`, `triggerFraction`(0.1 이상 1 미만), `keepRecentGroups`(1..128), `summaryMaxTokens`(16..8192), `maxSummaryPasses`(1..64)를 제공한다. 요약 출력 예약은 설정값과 실제 모델 창의 1/4 중 작은 값이다. 요약은 독립된 기본 샘플링 옵션에서 temperature=0, top_p=1, top_k=0으로 실행하며 원래 실행의 stop/logit bias를 가져오지 않는다.

수동 압축은 새 사용자 메시지나 후속 답변을 생성하지 않는다. 예산 측정에는 짧은 후속 입력을 임시로 붙인다. `CompactRequest.generation.maxTokens`는 후속 응답 예약량이다. 실제 다음 요청이 크면 다음 실행에서 다시 예산을 판단한다. 수동 성공 결과의 `turns`는 0이고 `text`는 요약이다. 비어 있거나 너무 짧아 줄일 수 없는 세션은 명시적으로 실패한다. 기존 요약 앞에 새 원본 기록이 없어 압축 경계를 전진시킬 수 없으면 추가 요약 추론 전에 종료한다.

## 저장·복원·분기

새 JSONL은 version 2다. 기존 `message` 레코드와 `parent_id` 체인은 그대로 유지하고 `type: compaction` 레코드를 별도로 추가한다. 체크포인트에는 고유 ID, 이전 체크포인트 ID, 작성 당시 원본 tail ID, 요약한 prefix 끝 ID, 요약, 원문으로 보존할 최신 사용자 ID, 누적 도구 결과 참조 ID, 압축 전후 토큰 수를 저장한다.

`Session.messages`는 항상 원본이다. `Session.compactions`는 완료된 경계 목록이고 `modelMessages(session)`은 가장 최근 경계로 모델 입력만 복원한다. JSONL 독자는 version 1도 읽는다. 최초 압축 때 기존 메시지 바이트를 보존한 채 헤더를 version 2로 원자적으로 교체한 다음 체크포인트를 추가한다. 구버전 SDK는 version 2를 읽지 못하므로 소비자를 함께 갱신해야 한다.

체크포인트는 세션 전체 실행 잠금 안에서 검증·쓰기·flush한다. 원본에 없는 참조, 도구 호출 중간 경계, 역행 경계, 중복 ID, 잘못된 이전 체크포인트, 최신 입력 누락, 비감소 수치는 거절한다. 완성되지 않은 마지막 JSONL 레코드는 재개 시 잘라내고 마지막 완료 경계로 복원한다. 줄바꿈까지 기록된 손상 레코드는 조용히 버리지 않는다. flush는 전원 장애에 대한 fsync 보장과 다르다.

분기는 선택한 메시지까지의 원문과 그 시점까지 완료된 체크포인트를 새 세션에 원자적으로 게시한다. 아직 결과가 없는 도구 호출에서 분기할 수 없다. 원문 조회는 별도 artifact 파일을 만들지 않는다. 기존 artifact가 있는 세션의 복제 제한 및 파일 rewind 미지원은 유지된다.

## 원문 조회와 파일 읽기 상태

체크포인트가 있거나 압축을 시작할 때 내부 스냅샷에 예약 도구 `iiLocalLLM.session.read`를 추가한다. 호스트가 같은 이름을 등록하면 충돌로 실패한다. 이 도구는 현재 세션의 기록만 조회한다. 입력은 `message_id`, 선택적 `offset`, `limit`(기본 512, 최대 1024)이며 JSON 레코드 문자열의 UTF-16 문자 위치를 사용한다. `slice`, `total_characters`, 선택적 `next_offset`으로 페이지를 반환한다. 이는 내용 조각이며 매 조각이 독립된 JSON 문서라는 뜻은 아니다.

일반 ToolRunner의 스키마·정책·훅을 통과한다. 다른 세션 ID를 받거나 원본 작업을 재실행하지 않는다. `Read`와 마찬가지로 정책에서 금지할 수 있다. 모델 생성 중 registry가 바뀌어도 해당 턴의 원문 스냅샷은 고정된다.

압축이 성공하면 `ToolContext.contextRevision`이 증가한다. Workspace의 기존 파일 편집 확인은 세션·revision·경로별로 분리하므로 압축 전에 읽은 파일은 다시 읽어야 편집할 수 있다. 과거 Read 결과를 원문 조회로 가져오는 것은 현재 파일의 완전 읽기 확인을 대신하지 않는다. 프로젝트 지침은 원본 경로 메타데이터를 바탕으로 매 턴 다시 조합한다.

## 이벤트·훅·사용량

`compaction_started` → 0개 이상의 `compaction_progress` → 저장 후 `compacted` 이벤트를 제공한다. 마지막 이벤트의 data에는 체크포인트가 들어간다. micro 압축은 모델 요약이 없으므로 progress가 없을 수 있다.

`BeforeCompact` 훅은 차단 및 추가 요약 요구를 제공한다. `AfterCompact` 훅은 저장 전에 요약을 심사·차단한다. 이 훅의 feedback은 이벤트로 전달하며 새로운 모델 입력으로 자동 추가하지 않는다. 취소·차단·모델 오류·용량 초과는 체크포인트를 남기지 않는다. 저장 후 `compacted` 이벤트 소비자가 예외를 던지면 실행은 실패할 수 있지만 이미 완료된 체크포인트는 유지된다.

`usage.prompt_tokens/generated_tokens/cached_tokens`에는 성공적으로 반환된 요약 호출 사용량도 합산한다. `summary_prompt_tokens`, `summary_generated_tokens`는 그 부분집합이며 `compactions`는 이번 요청에서 저장한 경계 수다. 요약 처리에 토큰을 쓴 뒤 후속 검증이 실패할 수도 있으므로 실패 결과에도 해당 사용량이 남을 수 있다. 원본 메시지를 삭제하지 않으므로 이 기능 자체는 `dropped_messages`를 증가시키지 않는다.

## API·MCP·CLI

인증된 HTTP/native IPC에서 `agent.sessions.compact`에 `session_id`, 선택적 `instructions`, `options`를 보낸다. 기존 `agent.run`과 같은 큐·세션 독점·순차 이벤트·취소·요청 기한을 사용한다. `agent.status`와 `agent.cancel`은 RPC request_id를 사용한다. `agent.sessions.get`은 원본 메시지 페이지, `compaction_count`, 최근 `compaction`을 반환한다.

```sh
iillm --socket private/llm.sock --auth-file private/app-token --json rpc agent.sessions.compact compact.json
```

`compact.json`은 `{"session_id":"SESSION_UUID","instructions":"Keep exact identifiers"}` 형태의 파일이다. CLI의 endpoint·인증 지정은 AgentAPI.md의 기존 형식을 사용한다. 데몬 호스트는 `--agent-no-auto-compact`로 자동 압축을 끌 수 있고, 명시적인 수동 압축은 계속 제공한다. RPC 클라이언트가 호스트 임계값이나 저장 루트를 바꾸지는 못한다.

MCP `iiLocalLLM.agent.compact`는 현재 연결의 기존 대화만 요약한다. 입력은 선택적 `instructions`뿐이며 임의 session_id나 new_session을 받지 않는다. `iiLocalLLM.agent.session`은 해당 연결의 압축 수와 최근 경계를 보여준다. 진행 이벤트는 기존 `iisacc/agentEvent` metadata로 전달한다. 외부 도구 정책은 `agent.run`과 동일하게 적용한다.

## 한계와 남은 대응

- 요약에 넣을 단일 완전한 그룹 자체가 모델 창보다 크거나 최신 보존 입력·도구 스키마·프로젝트 지침만으로 한도를 넘으면 `ContextOverflow`로 실패한다. 몰래 앞부분을 버리지 않는다.
- 한 번의 준비 과정은 최대 요약 호출 수로 제한한다. 압축 실패 시 해당 실행을 종료하며 반복 자동 재시도는 하지 않는다. 기준 구현의 세션 메모리 요약·별도 실패 회로·reactive overflow 재시도·서버 prompt-cache 편집은 남아 있다.
- 측정과 생성 사이 다른 Service 요청이 모델 설정을 바꾸면 생성 시 재검증에서 실패할 수 있다. 네이티브 생성 경로의 최종 한도 검사는 유지한다.
- 첨부 재주입·계획/스킬 복원·세션 메모리·캐시 통계·멀티모달 요약·실제 iisacc 앱 UI 연결은 이 단계의 완료 범위에 포함되지 않는다.
- 실제 모델의 정확한 요약·응답 여부는 별도 추론 테스트로 검증한다. 전체 하네스 호환 완료를 의미하지 않는다.

이 기능을 도입한 버전은 공개 구조체와 Model 가상 인터페이스 변경을 반영한 SDK/SOVERSION 0.6.0/0.6이다. 현재 버전은 README를 참조하고 새 헤더와 라이브러리로 함께 다시 빌드한다. 테스트 및 설치 검증 결과는 Verification.md에 기록한다.
