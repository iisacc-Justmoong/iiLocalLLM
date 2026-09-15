# 대화 종료 메모리 추출

0.39의 `agent/MemoryExtraction.h`는 주 에이전트의 답변 뒤에 대화에서 장기적으로 유용한 정보를 골라 프로젝트 메모리로 저장한다. C++·Qt와 기존 Model, ProjectMemory, ToolRunner, tree-sitter Bash 파서를 사용한다. 새 생산 의존성은 없다. 기준은 `c8cd253554319f32ff64ff7000636199f720c9bc`의 `services/extractMemories/extractMemories.ts`, `prompts.ts`, `query/stopHooks.ts`, `utils/forkedAgent.ts`이며 구현과 프롬프트는 독립적으로 작성한다.

## 실행 계약

- Engine은 도구 호출 없는 assistant 응답을 기록한 뒤, Stop 훅보다 먼저 추출을 제출한다. 추출 실패가 주 답변의 성공을 바꾸지 않는다. 추출 대화·도구 결과는 주 transcript나 InputQueue에 추가하지 않는다.
- 부모가 실제 사용한 모델, system prompt, 모델에 보낸 메시지 접두부, 도구 정의, 생성·thinking 설정을 유지하고 마지막 답변과 추출 지시를 덧붙인다. `ServiceModel`은 부모의 context ID를 그대로 사용한다. 서비스의 FIFO 실행과 토큰 접두부 비교에 따라 KV 재사용량이 정해지며 재사용을 보장하거나 사용량을 추정하지 않는다.
- native Read의 SHA-256·완전/부분 읽기 상태를 제출 시점에 복사한다. 작업마다 별도 세션 ID로 설치하고 종료 시 제거한다. 자식의 읽기·쓰기는 부모의 읽기 기록을 갱신하지 않는다. 바뀐 파일을 예전 읽기 기록으로 수정할 수 없다.
- 복사된 읽기 캐시는 작업별로 256개를 유지하며 부모의 캐시 용량을 소모하지 않는다. native 작업 도구 소유자 하나에 동시 캐시 복사본은 최대 64개이다. 겹쳐 바인딩한 동일 메모리 라우터가 캐시를 중복 설치하지 않도록 한다.
- 한 소유자에 작업 스레드는 하나이다. 세션별로 실행 중 요청 하나와 최신 대기 문맥 하나를 유지한다. 새 요청은 기존 대기 문맥을 대체한다. 후속 작업은 먼저 끝난 작업의 커서 이후 메시지 수를 다시 계산하고 주기를 다시 기다리지 않는다.
- 커서는 성공한 작업 뒤에만 전진한다. 압축 때문에 커서가 보이지 않으면 현재 보이는 대화 전체를 새 범위로 사용한다. Tool 결과도 모델에 보이는 메시지 수에 포함한다. 주 에이전트가 해당 범위에서 자기 메모리에 Write/Edit를 시도했으면, 도구 성공 여부와 관계없이 중복 추출을 생략하고 커서를 전진시킨다.
- 저장 지시는 확인된 사용자 선호, 피드백, 프로젝트 제약, 재사용할 외부 참조를 대상으로 한다. 자격 증명·비밀·임시 진행 상태·추측·소스에서 쉽게 얻는 사실은 저장하지 않도록 지시한다. 사실 선택은 모델의 판단이다. 파일 경계·해시·한도 검사는 호스트가 강제한다.
- 주제 파일의 `name`, `description`, `type` frontmatter와 중복 없는 갱신을 요청한다. 기본적으로 주제를 먼저 저장하고 200줄 이내의 MEMORY.md 상대 링크 인덱스를 갱신하도록 지시한다. `manageIndex=false`는 인덱스 갱신 지시만 끈다.

## 도구와 수명

Read/Grep/Glob은 부모 작업 디렉터리와 기존 비공개 경로 정책을 따른다. Write/Edit는 준비된 `canonical_path`와 `memory_directory`를 확인하여 해당 프로젝트 메모리 안에서만 실행한다. 기존 deny/ask 규칙은 유지하며 추출 작업은 사용자에게 권한 질문을 띄우지 않는다. native 도구의 입력 변경 훅도 적용한 뒤 경계를 재검사한다. 저장용 AcceptEdits 범위는 이 작업의 메모리에만 적용한다.

MCP·Agent·Skill 등 허용하지 않은 도구는 검증/준비/실행 콜백을 호출하기 전에 거절한다. 모델에 광고된 전체 도구 목록을 유지하는 것과 실행 허용은 별개이다. 위임 에이전트와 훅 검증 에이전트에는 자동 추출을 켜지 않는다. 부모의 lifecycle/Stop/model 훅을 추출 루프에서 재실행하지 않으며, 도구 훅은 유지한다.

Unix Bash는 기존 AST 파서로 리터럴 명령·인수·파이프를 검사한다. 현재 분류 범위는 pwd, true/false, echo, 제한된 printf 형식, cat/wc/head/tail/ls의 읽기 옵션이다. 파일 인수는 작업 경계와 비공개 경로를 검사하며, 출력 리다이렉션·치환·변수 대입·백그라운드·재귀 검색·알 수 없는 명령은 거절한다. 실행 시 빈 시작 환경과 고정 `/usr/bin:/bin` PATH를 사용하고 native 실행 직전 다시 검사한다. 이는 OS 샌드박스가 아니다. 더 넓은 Bash 명령 분류, Windows 셸 판정과 경쟁적 파일 교체의 OS 격리는 남은 범위이다.

기본 한도는 5 모델 턴, 턴당 64 도구 호출, 60초 협력적 실행 기한, 요청 4 MiB, 누적 모델 출력 1 MiB이다. 대기/재시도용 부모 문맥은 최대 16 세션이며 새 세션이 필요하면 가장 오래 사용하지 않은 유휴 문맥을 비운다. 완료 기록은 기본 128개이다. 기록당 경로 목록은 64 KiB로 제한하며 생략 시 `paths_truncated`, 실제 `written_count`와 `saved_topic_count`를 제공한다. 상태 조회는 최대 32개씩 페이지로 반환한다. 한도 도달로 기록이 삭제되면 offset은 현재 보관 목록 기준이다.

`written_paths`는 성공한 native Write/Edit 결과만 포함하고 `saved_topics`에서는 MEMORY.md를 제외한다. 실패한 작업도 이미 완료한 쓰기와 사용량을 보고한다. 여러 파일을 한 번에 롤백하는 트랜잭션은 없다. 부분 저장 뒤 취소할 수도 있다. `completed`는 루프가 종료되었다는 뜻이며 저장 여부는 성공 경로와 오류 수를 함께 확인한다.

부모 RunHandle의 종료/취소 토큰과 추출 토큰은 분리한다. `cancel`은 실행·대기 작업을 취소하고 `drain`은 후속 대기 작업 및 완료 콜백까지 기다린다. `endSession`/`clearSession`은 해당 세션 작업을 취소·합류하고 문맥/커서를 해제한다. Engine 종료는 기본 60초 soft drain 후 남은 작업을 취소·합류한다. Model/Hook은 취소에 협력해야 한다. 협력하지 않는 호스트 콜백의 강제 종료는 제공하지 않는다. 완료 콜백은 작업 스레드에서 잠금 없이 호출한다. UI 호스트는 자기 스레드로 전달해야 하며 그 콜백에서 소유자를 종료하거나 drain하면 안 된다.

## C++·API·MCP·CLI

임베디드 C++은 `EngineOptions.projectMemoryEnabled=true`와 `memoryExtraction.enabled=true`를 설정한다. daemon 및 agent MCP CLI는 프로젝트 메모리가 켜져 있으면 기본 활성화한다. `--agent-no-memory-extraction` 또는 MCP의 `--no-memory-extraction`으로 끈다. 프로젝트 메모리 전체를 끄면 추출도 비활성이다.

```cpp
options.projectMemoryEnabled = true;
options.memoryExtraction.enabled = true;
options.memoryExtraction.completed = [](const QJsonObject& result) {
    // Host-owned UI/event dispatch. result contains no extraction transcript.
};
// engine.run(...) returns before background extraction completes.
auto state = engine.memoryExtractionStatus(sessionId, 0, 32);
auto retry = engine.extractMemory(sessionId); // Latest retained completed parent context.
engine.drainMemoryExtractions(60000, sessionId);
```

| 인증 API 메서드 | 입력 | 결과 |
|---|---|---|
| `agent.memory.extract` | `session_id` | queued job ID 또는 disabled/no_context/up_to_date 등의 접수 상태 |
| `agent.memory.extraction.status` | `session_id`, 선택 `offset`, `limit` | active/pending/cursor, count, records, next_offset |
| `agent.memory.extraction.cancel` | `session_id` | 취소 요청 뒤 현재 상태 |

API 세션과 메모리는 인증 앱 소유자별로 분리한다. status/cancel에는 기존 제어 요청용 예약 용량을 사용한다. 원격 입력으로 대화 스냅샷·모델·도구 권한을 주입할 수 없다. `agent.info.memory_extraction_enabled`로 기능을 확인한다.

MCP 도구는 같은 이름 앞에 `iiLocalLLM.`을 붙인다. MCP 연결의 대화 소유자를 사용하므로 `session_id` 입력을 받지 않는다. status의 offset/limit만 선택 입력이다. `iisacc/projectMemory.extractionTools`로 노출 여부를 알린다. 포화 상태에서도 상태/취소를 전달하는 예약 제어 메서드는 `iisacc/memory/extraction/status`, `iisacc/memory/extraction/cancel`이며 같은 capability에 알린다. CLI는 `iillm --auth-file FILE agent memory extract SESSION`, `extraction.status SESSION [PARAMS_FILE]`, `extraction.cancel SESSION`이다.

`memory_extraction` 이벤트는 주 실행에서의 제출 결과만 전달한다. 완료 내용은 옵션의 callback 또는 상태 API로 받는다. prompt/generated/cached 토큰, 소요 시간, 모델 턴, 도구 호출·오류 수는 추출 기록에 별도로 보고한다. cursor/문맥/작업 기록은 프로세스 메모리이며 저장된 주제 파일만 재시작 후 유지한다.

## 검증과 남은 범위

단위 검사는 접두부 동일성, native 쓰기·읽기 복사, 경계와 훅 재검사, 중복 대체·커서·재시도·주기·한도·취소·종료를 검증한다. Engine/API/MCP 검사는 주 답변/기록 분리, Stop 훅 순서, clear/close, 앱 소유권과 제어 기능을 검증한다. 실제 모델 검사는 가공하지 않은 ServiceModel 요청으로 부모 대화→자동 저장→새 세션 조회와 비활성 대조군을 실행한다. 구체적인 실행 결과는 Verification.md를 따른다.

전문 에이전트/팀 메모리, Git worktree 공유, 사용자/관리/원격 설정 우선순위, 원격 동기화, 전체 앱 UI 및 플랫폼 실행 검증은 계속 미완료이다. 이 변경으로 전체 하네스나 memory 영역을 완료 처리하지 않는다.

0.40의 과거 대화 검색은 [SessionHistory.md](SessionHistory.md), 0.41의 정리는 [MemoryDream.md](MemoryDream.md)를 따른다. 추출과 정리는 내부 MemoryWorker에서 모델 문맥·도구 권한·읽기 캐시·취소 코드를 공유한다. 추출 작업에는 SessionSearch를 허용하지 않으며 정리 작업에만 허용한다.
