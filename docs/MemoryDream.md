# 프로젝트 메모리 정리 — 0.41

`agent::MemoryDream`은 완료된 부모 대화를 바탕으로 기존 주제 메모의 중복·모순·오래된 정보를 정리하는 C++ 작업이다. 추출과 정리는 `MemoryContext` 및 내부 `MemoryWorker`의 모델 실행·권한·읽기 캐시 코드를 공유한다. 기존 Qt Core의 QLockFile/QSaveFile과 기존 네이티브 도구를 사용하며 새 생산 의존성이나 Python 실행 계층은 추가하지 않는다.

## 실행 조건과 저장

자동 실행은 기본 OFF이다. 프로젝트 메모리와 SessionHistory가 모두 있어야 수동 요청도 사용할 수 있다. 활성화한 Engine은 도구 호출 없는 주 assistant 응답을 저장한 뒤 Stop 훅보다 먼저 후보를 제출한다. 자식·훅 검증 에이전트는 자동 정리를 상속하지 않는다. 추출 실패와 정리 실패는 각각 주 답변의 성공을 바꾸지 않는다.

자동 후보는 마지막 성공 시작 시각부터 기본 24시간, 최근 목록 검사부터 10분, 마지막 성공 시각 이후 수정된 다른 대화 5개를 순서대로 확인한다. 생성 시각이 아니라 transcript 수정 시각을 사용하며 현재 대화는 제외한다. `SessionHistory::recent`가 기존 목록·헤더 읽기 예산과 작업공간 소유권 검사를 적용한다. 대화 내용은 목록에 포함하지 않는다. 별도 작업 스레드에서 검사하므로 부모 응답은 목록 검사를 기다리지 않는다.

프로젝트 메모리 디렉터리의 부모에 `.dream.lock`과 `.dream-state.json`을 둔다. 일반 메모리 도구가 이 제어 파일을 수정할 수 없다. QLockFile을 얻은 뒤 시간·세션 조건을 다시 검사한다. 살아 있는 보유자의 잠금을 경과 시간만으로 제거하지 않으며 죽은 PID의 잠금은 QLockFile의 회수 규칙을 따른다. 성공한 루프에 도구 오류가 없을 때만 시작 시각을 QSaveFile로 저장한다. 파일은 소유자 읽기·쓰기 권한으로 저장하며 링크·잘못된 경로·오염된 타임스탬프를 거절한다.

실패·취소·시간 초과·턴 한도·프로세스 중단은 마지막 성공 시각을 전진시키지 않는다. 이미 성공한 메모리 쓰기는 유지한다. 여러 파일과 추출/정리 사이에 전체 트랜잭션을 제공하지 않는다. 쓰기 전 전체 읽기 및 현재 SHA-256 확인은 기존 파일 도구가 수행한다. 정상 종료한 무변경 작업도 성공 시각을 갱신한다.

수동 `request`는 최근 완료 부모 문맥을 사용하며 시간·목록 주기·최소 세션 수 조건을 건너뛴다. 프로세스 잠금·경로·권한·한도는 그대로 적용한다. 문맥이 없으면 `no_context`이며, 요청자가 임의 모델·대화·작업공간을 주입하는 원격 경로는 없다. 호스트 프로세스 재시작 후 첫 완료 대화가 있어야 수동 정리를 실행할 수 있다.

## 모델과 도구 계약

부모가 사용한 model, system prompt, 메시지 접두부, 전체 도구 정의, 생성 설정과 context ID를 유지하고 별도의 정리 지시를 덧붙인다. 모델 서비스의 직렬화와 실제 접두부에 따라 KV 캐시가 재사용될 수 있으며 재사용량은 실행 결과로 기록한다.

지시는 현재 인덱스·주제 확인, 필요한 과거 기록과 코드의 좁은 확인, 모순·중복 정리, 상대 링크 인덱스 축약을 요청한다. 주제의 name/description/type, 절대 날짜, 200줄·25,000 UTF-8 바이트 이내의 인덱스를 요청한다. 사실 선택과 문서 품질은 모델의 판단이고, 도구 경계·해시·실행 한도는 호스트가 강제한다. 비밀·추측·임시 진행 상태는 저장하지 않도록 지시한다.

현재 사용자의 명시적 정정은 오래된 선호 메모보다 우선한다. 현재 대화를 기본 제외하는 SessionSearch에서 같은 내용을 찾지 못해도 이미 부모 문맥에 있는 근거를 무효화하지 않도록 지시한다. 호스트의 메모리 정리 권한을 분명히 하고 주제 내용과 중복 링크를 모두 확인한 뒤 무변경으로 종료하도록 요청한다.

모델이 도구 없이 종료하려 하면 호스트가 ProjectMemory::index로 인덱스만 잠금 아래에서 확인한다. 앞뒤 공백을 trim한 ProjectMemory 인덱스 표현을 기준으로 200줄·25,000바이트 또는 호스트가 설정한 더 작은 한도와 문서화된 `- [제목](상대경로)` 목록의 중복 대상을 검사한다. `./`는 정규화하며 서로 다른 제목도 같은 대상이면 중복이다. 일반 Markdown 전체를 해석하거나 사실의 의미를 검증하는 기능은 아니다. 문제가 남으면 구체적인 호스트 피드백을 같은 작업 대화에 추가한다. 모델·도구·생성 설정·총 턴/시간 예산은 그대로이며 추가 도구 작업도 기존 권한을 거친다. 검증 횟수에 따른 재시도는 validation_retries, 통과 여부는 completion_validated로 기록한다. 끝까지 고치지 못하면 성공 시각을 전진시키지 않는다. 이는 참조의 단순 루프 종료보다 엄격한 iiLocalLLM의 구조 검증이며 다른 메모리 writer와의 전체 트랜잭션은 아니다.

실제 모델 검사에서 기존 Glob의 `**/*.md`가 루트와 두 단계 이상 깊이의 파일을 놓치는 문제를 발견했다. 0.41은 Qt의 단일 경로 구성요소 와일드카드 변환을 재사용하고 `**` 구성요소에 0개 이상의 디렉터리 의미를 적용한다. `*`·`?`는 한 구성요소 안에서 일치하며 대소문자를 구분한다. 문자 클래스는 기존 Qt 변환을 따른다. 메모리와 일반 작업공간에 같은 수정이 적용되고 기존 비공개 경로·링크·목록 한도는 유지한다.

허용 도구는 네이티브 Read/Grep/Glob, 해당 소유자에 묶인 SessionSearch, 보수적으로 분류된 읽기 전용 Unix Bash, 해당 메모리 안의 Write/Edit이다. 외부 MCP·위임·삭제와 알 수 없는 도구는 검증/준비 콜백부터 실행하지 않는다. 기존 명시적 deny/ask와 도구 훅을 유지하고 비대화형 작업은 권한 질문을 띄우지 않는다. 모델에 부모 도구 정의를 유지하는 것과 도구 실행 허용은 별개이다. 읽기 복사와 셸 제약은 [MemoryExtraction.md](MemoryExtraction.md)를 따른다.

작업 스레드는 소유자당 하나이다. 실행 중 작업 외에 세션별 최신 대기 요청 하나를 유지하며 대체된 기록은 `superseded`가 된다. 기본 부모 문맥 16개, 완료 기록 128개이며 유휴 문맥부터 비운다. 실행·대기 중 기록은 조기 제거하지 않아 작은 maxRecords보다 일시적으로 많을 수 있다. 문맥·작업 기록·목록 검사 시각은 메모리 상태이고 성공 시각과 주제 파일은 재시작 후 유지된다.

기본 한도는 30 모델 턴, 협력적 300초 실행 기한, 입력 4 MiB, 누적 모델 출력 1 MiB, 턴당 도구 호출 64개이다. `phase`는 starting에서 첫 Write/Edit **시도**에 updating으로 바뀐다. 성공 경로는 written_paths/saved_topics와 실제 수를 별도로 기록한다. 최근 assistant 요약은 최대 30개·32 KiB, 텍스트는 1024 UTF-16 단위, 상태의 경로 배열은 각각 8 KiB이다. Unicode surrogate pair를 분리하지 않고 생략 플래그를 제공한다. 상태 페이지는 최대 8개 기록이며 offset은 현재 보관 목록 기준이다.

주 transcript·InputQueue에 정리 대화나 완료 메시지를 추가하지 않는다. `memory_dream` 이벤트는 접수만 알리며 실행 결과는 progress/completed 콜백 또는 상태 조회로 받는다. 콜백은 작업 스레드에서 잠금 없이 호출하므로 UI는 호스트 스레드로 전달해야 한다. 콜백에서 소유자 종료·파괴·drain은 금지한다. `cancel`은 실행·대기를 취소하고 `drain`은 완료 콜백까지 기다린다. clear/end는 취소·합류 후 문맥을 지운다. close는 기본 60초 soft drain 후 취소·합류한다. 취소에 협력하지 않는 모델/훅을 강제 종료하지 않는다.

## C++·API·MCP·CLI

```cpp
options.projectMemoryEnabled = true;
options.sessionHistoryEnabled = true;
options.memoryDream.automatic = true;
// The main run returns independently of consolidation.
auto receipt = engine.consolidateMemory(sessionId);
auto status = engine.memoryDreamStatus(sessionId, 0, 8);
engine.cancelMemoryDream(sessionId);
engine.drainMemoryDreams(60000, sessionId);
```

| 인증 API 메서드 | 입력 | 결과 |
|---|---|---|
| agent.memory.dream | session_id | queued 또는 unavailable/no_context 접수 |
| agent.memory.dream.status | session_id, 선택 offset/limit | active/pending/has_context, count/records/next_offset |
| agent.memory.dream.cancel | session_id | 취소 요청 뒤 현재 상태 |

데몬은 `--agent-auto-dream`, agent MCP는 `--auto-dream`으로 켠다. 필요한 메모리·대화 검색을 끄거나 MCP 모델을 지정하지 않으면 자동 활성화 요청을 거절한다. API의 `agent.info.memory_dream_available`과 `auto_dream_enabled`를 구분한다. 데몬의 앱별 상태는 격리되며 MCP는 [SessionHistory.md](SessionHistory.md)에 명시한 호스트 작업공간을 공유한다.

MCP 도구는 API 이름 앞에 `iiLocalLLM.`을 붙이며 연결 소유 대화를 사용하므로 session_id를 받지 않는다. `iisacc/memoryDream`은 `iisacc.memory-dream/1`, automatic, requestTool/statusTool/cancelTool 및 예약 제어 메서드 `iisacc/memory/dream/status`, `iisacc/memory/dream/cancel`을 알린다. 상태·취소는 주 작업 용량과 분리된 제어 경로를 사용한다. CLI는 `iillm --auth-file TOKEN_FILE agent memory dream SESSION`, `dream.status SESSION [PARAMS_FILE]`, `dream.cancel SESSION`이다.

## 참조와 남은 범위

참조 커밋은 `c8cd253554319f32ff64ff7000636199f720c9bc`이다. `source/src/services/autoDream/autoDream.ts`, `config.ts`, `consolidationLock.ts`, `consolidationPrompt.ts`와 `source/src/tasks/DreamTask/DreamTask.ts`를 기준으로 비교한다. 독립적으로 작성한 C++ 구현과 지시이며 TS 코드를 제품 런타임에 포함하지 않는다.

참조는 사용자 설정을 캐시된 기능 플래그보다 우선하고, 원격/KAIROS/메모리 비활성 조건 등을 검사한다. 참조 잠금은 메모리 안에 PID와 mtime을 함께 두고 획득 시 성공 시각을 낙관적으로 기록하며 실패 때 되돌린다. iiLocalLLM은 잠금과 마지막 성공 시각을 분리하고 완료 후 시각을 기록한다. 참조의 PID 쓰기/다시 읽기 경쟁 및 1시간 경과 잠금 회수 동작을 그대로 복제하지 않는다. 참조 호출부에 명시적 maxTurns가 없는 것과 iiLocalLLM의 30턴 기본값은 동일한 계약이라고 주장하지 않는다.

원격 기능 플래그·전체 설정 우선순위, Git worktree 공유, 전문 에이전트/팀 메모리, 모델 기반 의미 세션 검색, 전체 앱 작업 UI, 다른 플랫폼과 원격 동기화는 남은 범위이다. 최신 30개 작업 요약을 API로 제공하는 것과 참조의 전체 UI를 구현한 것은 구분한다. 실제 검사 결과는 [Verification.md](Verification.md)를 따른다. 전체 memory 및 하네스 영역은 partial이다.
