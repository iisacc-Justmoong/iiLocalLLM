# 저장된 대화 검색 — 0.40

`agent::SessionHistory`와 기본 도구 `SessionSearch`는 호스트가 소유한 SessionStore의 과거 대화를 검색한다. 외부 서비스나 새 의존성 없이 기존 Qt Core의 JSON·파일·문자열 기능을 사용한다. C++ Engine, 인증 API/IPC, MCP, 얇은 CLI에서 같은 검색기를 호출한다.

## 소유 범위와 호출

임베디드 호스트는 `EngineOptions.sessionHistoryEnabled=true`로 활성화한다. 경로는 호스트의 `sessionsDirectory`로 고정하며 요청에서 경로·작업공간을 받지 않는다. 요청 소유 세션의 불변 헤더와 실제 작업공간을 검증한다. 검색 대상도 같은 루트와 작업공간에 속해야 한다. 자식 에이전트와 훅 검증 에이전트에는 이 기능을 상속하지 않는다.

데몬은 앱 인증 토큰마다 별도 Engine/세션 디렉터리를 할당하므로 Society의 검색은 Dreamscapes의 기록에 접근하지 못한다. 독립 `iillm-mcp` HTTP 서버는 기존 계약대로 지정한 작업공간·Engine을 모든 인증 클라이언트가 공유한다. 따라서 그 서버의 과거 대화 검색 범위도 공유된다. MCP 연결마다 현재 대화와 커서는 분리되지만 과거 대화 자체를 클라이언트별로 격리하는 서버는 아니다. 클라이언트별 기록 분리가 필요하면 데몬 API 또는 별도의 MCP 호스트 상태 디렉터리를 사용한다.

데몬과 agent 모드 MCP에서는 기본 활성화한다. 각각 `--agent-no-session-history`, `--no-session-history`로 끈다. API의 `agent.info.session_history_enabled`와 MCP의 `iisacc/sessionHistory` 기능 정보가 실제 활성 상태를 알린다.

```cpp
auto result = engine.runSessionSearch(ownerSessionId,
    {{"query", "release alias"}, {"limit", 10}});
// result.isError를 확인한 뒤 result.data의 matches, next_cursor, complete를 읽는다.
```

API는 `agent.sessions.search`이며 `session_id`로 소유 대화를 선택한다. 나머지 인자는 도구와 동일하다. 반환 형식은 `{text,result,is_error}`이며 `result`에 검색 페이지가 들어간다. MCP 도구 이름은 `SessionSearch`이고 소유 대화는 연결에 고정하므로 `session_id`를 받지 않는다. 네이티브 도구와 직접 API/MCP 호출은 기존 권한 규칙·도구 훅을 거치며 명시적 deny/ask를 우회하지 않는다.

```json
{"query":"release alias","limit":10}
```

```sh
iillm --socket SOCKET --auth-file TOKEN_FILE agent sessions search SESSION_ID PARAMS_JSON_FILE
```

`query`는 비어 있지 않은 최대 512 UTF-16 단위의 대소문자 무시 리터럴이다. 정규식·의미 검색·모델 재순위화는 아니다. `session_ids`로 최대 64개의 중복 없는 UUID를 선택할 수 있다. 기본 검색은 현재 대화를 제외한다. 현재 대화를 포함하려면 명시적으로 선택한다. 이 기본값은 모델의 다음 도구 호출이 자신의 transcript를 갱신하면서 이전 검색 커서를 무효화하는 일을 피한다.

## 읽기와 페이지 계약

세션은 transcript 수정 시간 내림차순, 동률이면 ID순으로 검색하고 각 세션의 메시지는 기록순으로 읽는다. 메시지당 최초 일치 필드 하나를 반환한다. 필드 순서는 `text`, `tool_calls`, `data`, `content`이며 구조화된 값은 compact JSON 문자열로 검색한다. 시스템 프롬프트·호스트 `metadata`·압축 요약 레코드는 검색하지 않는다. 결과 텍스트는 과거의 근거이며 현재 지시로 실행하지 않는다.

각 결과에는 세션/메시지 ID, 역할, 필드, 발췌, 1부터 시작하는 JSONL 줄 번호, 0부터 시작하는 바이트 위치, 파일 크기·수정 시각·헤더 SHA-256을 담는다. 전체 transcript의 내용 해시는 아니다. 발췌는 기본 1024 UTF-16 단위로 제한하며 surrogate pair를 분리하지 않는다.

기본 한도는 디렉터리 항목 1024개, 파일당 64 MiB, 레코드당 4 MiB, 페이지당 읽기 16 MiB·레코드 20,000개·일치 20개이다. 호출자는 일치 한도를 1–50으로 지정할 수 있다. 메타데이터 헤더 읽기도 바이트 예산에 포함한다. 초기 목록/헤더 예산이 부족하면 명시적 제한 오류를 반환하므로 `session_ids`로 좁혀야 한다. 내용 검색 예산은 `next_cursor`로 이어 읽는다. 빈 `matches`만으로 검색 완료를 판단하면 안 된다.

커서는 호스트 메모리에 보관하는 무작위 ID로, 해당 검색기 인스턴스·소유 세션·작업공간에 묶인다. 다음 요청은 `cursor`와 선택적인 `limit`만 받는다. 쿼리/대상 목록은 변경할 수 없다. 커서는 1회 사용, 검색 시작부터 기본 5분, 검색기당 최대 16개이다. 접수된 이어 읽기는 실패·취소되어도 기존 커서를 소비한다. 만료·프로세스 재시작·한도 초과 후에는 새 검색을 시작한다.

검색은 writer lease 없이 파일 크기/변경 표식을 고정하고 페이지 전후에 대상 파일들을 재검증한다. POSIX에서는 열린 파일과 경로의 device/inode, 크기, 나노초 mtime/ctime을 비교한다. 다른 플랫폼은 Qt 파일 크기·수정/생성 시간을 사용한다. 대상 기록이 변경되면 새 검색을 요구한다. 이는 OS 샌드박스나 모든 파일의 원자적 스냅샷을 제공하지 않는다.

파일/세션 디렉터리/루트의 심볼릭 링크·경로 이탈을 거부한다. 기본 목록의 링크/누락 항목은 건너뛰고 명시적으로 선택하면 실패한다. 완전한 JSONL 줄만 읽으며 마지막의 미완성 줄은 수정 없이 제외하고 `incomplete_tails`에 표시한다. 불완전한 헤더·잘못된 JSON·메시지 연결·역할은 실패한다. 전체 SessionStore의 도구 짝/중복 ID 검증을 대체하는 무결성 감사기는 아니다.

## 참조와 남은 범위

참조 커밋 `c8cd253554319f32ff64ff7000636199f720c9bc`의 `source/src/services/autoDream/consolidationPrompt.ts`는 큰 과거 JSONL 기록을 좁게 검색하도록 요구한다. 이 모듈은 호스트 비공개 transcript를 일반 파일 도구에 개방하지 않고 그 기능에 필요한 검색 경로를 제공한다. 참조 `source/src/utils/agenticSessionSearch.ts`의 모델 기반 의미 검색·태그/제목/브랜치 우선순위 기능은 별도이며 아직 구현하지 않았다. 0.41의 시간/세션 조건·프로세스 잠금·메모리 정리는 [MemoryDream.md](MemoryDream.md)를 따른다. 호스트 전용 recent(owner,workspace,sinceMs)는 같은 목록·헤더 한도와 소유권 검사를 적용해 현재 대화를 제외한 수정 시각 > sinceMs인 세션 ID·시각·크기를 반환한다. 원문을 읽지 않으며 모델/원격 입력에 파일 경로를 노출하는 별도 목록 API는 추가하지 않는다.

`session_history_tests.cpp`는 큰 기록·페이지·소유권·취소·링크·미완성/오염된 줄을, `session_history_engine_tests.cpp`는 모델 호출·활성 세션·권한·API·MCP 연결을 검증한다. 실제 모델과 설치 소비자 결과는 [Verification.md](Verification.md)에 구분해서 기록한다. 기존 앱 설치와 홈 SDK의 0.36 조합은 별도이며, 새 stage 설치를 제품 갱신으로 보고하지 않는다.
