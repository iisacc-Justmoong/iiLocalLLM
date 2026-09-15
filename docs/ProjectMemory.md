# 프로젝트 메모리

`agent/ProjectMemory.h`는 프로젝트별 Markdown 메모리의 저장 위치, 자동 문맥, 목록·검색과 기존 파일 도구 연결을 제공한다. C++ 및 기존 Qt Core/Network를 사용한다. YAML 메타데이터는 이미 사용 중인 libyaml 기반 Frontmatter 파서를 재사용한다. 외부 서비스와 새 런타임 의존성은 추가하지 않는다.

참조는 고정된 분석본 `c8cd253554319f32ff64ff7000636199f720c9bc`의 `source/src/memdir/paths.ts`, `memdir.ts`, `memoryScan.ts`, `memoryTypes.ts`, `findRelevantMemories.ts`이다. 참조의 저장·검색 계약을 분석했으며 소스나 프롬프트를 복사하지 않는다. 참조에 있는 조건부 추출·dream 정리 기능을 기본 파일 저장 기능과 구분한다.

## 저장과 문맥

호스트가 지정한 base 아래 `projects/<canonical workspace SHA-256>/memory/`를 사용한다. 같은 base와 workspace의 새 세션·재실행은 같은 메모리를 본다. 다른 workspace는 다른 디렉터리를 사용한다. `MEMORY.md`는 자동으로 읽는 인덱스이며 상세 내용은 하위 Markdown 파일에 보관한다. 인덱스 기본 한도는 200줄·UTF-8 25,000바이트이다. 초과 시 문자 경계를 지켜 자르고 잘림을 명시한다. 원본은 수정하지 않는다.

메모리는 모델의 user 문맥에 과거 자료로 전달하며 권한이나 system 지침으로 승격하지 않는다. 현재 관측이 메모리와 다르면 현재 관측을 따른다. 저장은 사용자 지시와 호스트 정책을 따른다. 이 모듈을 활성화하는 것만으로 기존 파일 권한을 우회하지 않는다.

상세 메모는 선택적으로 YAML `name`, `description`, `type`을 가진다. type은 `user`, `feedback`, `project`, `reference`이다. 목록은 최근 수정 순서이며 동률은 경로 순서이다. snapshot의 query는 경로·본문·헤더에 대한 대소문자 무시 리터럴 검색이다. 실패한 파일과 생략된 링크는 diagnostics에 남긴다. 디렉터리 진입 수·파일 크기·전체 스캔 바이트·결과 수에 각각 상한이 있다. 한도에 걸리면 catalog_truncated를 반환한다.

## 파일 도구

호스트의 기존 Read·Write·Edit·Glob·Grep이 정확한 메모리 디렉터리 안의 절대 경로를 처리한다. 상대 경로는 원래 workspace를 기준으로 한다. Bash나 임의 MCP 도구에 메모리 영역 접근을 추가하지 않는다. 인접 세션 기록·다른 프로젝트 메모리는 이 경로 확장에 포함하지 않는다.

base를 workspace 안에 둔 경우에도 일반 파일 도구의 재귀 검색과 링크 별칭은 base를 비공개 경로로 처리한다. 현재 프로젝트의 정확한 메모리 경로만 별도로 연결한다. 이 제외 목록은 호스트 ToolContext의 protectedPaths로 전달하며 모델 인자나 MCP 메타데이터로 지정할 수 없다.

메모리 변경은 `.md` 파일에 한정하며 경로의 심볼릭 링크를 거부한다. 기존 파일은 완전히 읽은 뒤 변경해야 하며 읽은 후 해시가 달라지면 다시 읽어야 한다. Read·Write·Edit은 처리한 파일의 SHA-256도 반환한다. 도구 호출의 준비·권한·훅·실행 순서와 host deny 우선순위를 유지한다.

MemoryForget은 현재 SHA-256과 일치하는 메모만 삭제한다. 삭제 전 프로젝트의 비공개 forgotten 디렉터리에 백업하고 삭제 직전 내용을 다시 검사한다. 인덱스의 링크는 자동으로 고치지 않으므로 필요하면 Edit으로 함께 정리해야 한다. 프로젝트별 파일 잠금으로 이 모듈을 사용하는 실행끼리 충돌을 직렬화한다. 일반 편집기·Bash 등 외부 작성자와 원자적인 다중 파일 트랜잭션이나 OS 샌드박스를 제공한다는 의미는 아니다.

## 남은 범위

Git worktree 사이의 공통 저장소 식별, 사용자/관리/원격 메모리 설정 계층, 전문 에이전트·팀의 별도 메모리, 모델 기반 관련도 선택, 세션 기록 검색, 턴 종료 자동 추출, 예약 dream 정리와 원격 동기화는 별도 미완료 범위이다. 기본 저장 기능을 통과해도 전체 memory 영역은 부분 구현 상태를 유지한다.

검증 기록은 기능 구현과 설치 소비자·API·MCP·실제 모델 실행을 구분해 `Verification.md`에 추가한다.

## Engine·API·MCP·CLI

내장 호스트는 `EngineOptions.projectMemoryEnabled=true`로 활성화한다. `projectMemory.directory`가 비어 있으면 해당 Engine의 sessionsDirectory/memory가 base이다. Engine은 모델을 호출할 때마다 새 인덱스를 읽으며 이 자료는 자동 압축의 예산 측정에도 포함한다. 인덱스를 transcript 메시지로 중복 저장하지 않는다. 같은 workspace의 fork·clear·새 세션은 같은 메모리를 사용하고, 기본 자식 에이전트도 부모의 메모리 base를 상속한다.

인증 API는 각 client ID의 비공개 상태 안에 별도 메모리를 할당한다. `ApiOptions.engine.projectMemory.directory`를 임의로 공유 설정하는 것은 거부한다. `agent.memory.get(session_id, query?)`은 인덱스·목록·해시·진단을 반환한다. `agent.memory.read/write/edit/glob/grep/forget`은 기존 파일 도구 인자에 session_id를 추가한다. 모든 변경은 기존 권한과 훅을 통과한다. 훅이나 승인 응답이 인자를 바꾸면 메모리 영역을 다시 검사한다. get의 query는 리터럴이고 grep의 pattern은 기존 정규식이다.

MCP는 `iiLocalLLM.agent.memory.get`과 `MemoryForget`을 내보내며 기존 Read·Write·Edit·Glob·Grep을 연결 소유자의 프로젝트 영역에 연결한다. MCP 입력으로 session_id나 메모리 root를 지정할 수 없다. 같은 호스트 엔진·workspace에 인증된 MCP 연결은 프로젝트 메모리를 공유한다. 이는 client ID별로 구분하는 API 저장 영역과 다른 계약이다.

daemon의 에이전트 API와 모델이 활성화된 iillm-mcp는 기본 활성화한다. 각각 `--agent-no-memory`, `--no-memory`로 끌 수 있다. 끄면 자동 문맥과 파일 접근 확장·삭제 도구도 사라지며 기존 메모리 파일은 보존한다. CLI는 `iillm --auth-file TOKEN_FILE agent memory get SESSION` 또는 `agent memory read/write/edit/glob/grep/forget SESSION PARAMS_JSON_FILE`을 사용한다. RPC·HTTP와 같은 인증 경로를 따른다.
