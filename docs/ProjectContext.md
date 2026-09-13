# 프로젝트 지침과 모델 입력

iiLocalLLM 0.5.0은 지정한 workspace의 프로젝트 지침을 C++에서 읽고 실제 `Engine` → `ServiceModel` → 로컬 모델 입력에 조합한다. HTTP·native IPC·얇은 CLI에서도 동일한 스냅샷을 조회한다. MCP의 `iiLocalLLM.agent.run`도 선택적 `context_paths`를 전달하며 연결별 세션에 범위를 보존한다. 이 단계는 전체 하네스 컨텍스트 기능 중 프로젝트 지침 부분이다. 일반 첨부·자동 요약·microcompact 구현을 뜻하지 않는다.

## 적용 흐름

1. 기본 탐색 경계는 세션의 `workingDirectory`다. C++ 호스트는 `EngineOptions.projectContext.rootDirectory`에 허용할 상위 루트를 명시할 수 있다. `/`는 허용하지 않는다. 인증 API에서는 경계가 고정 workspace와 같아야 한다.
2. 경계에서 작업 폴더까지 상위 → 하위 순으로 읽는다. 각 폴더에서 `CLAUDE.md`, `.claude/CLAUDE.md`, `.claude/rules/**/*.md`, `CLAUDE.local.md`, `AGENTS.md` 순이다. rules 파일은 canonical 경로로 정렬한다. 관련 없는 하위 폴더는 탐색하지 않는다.
3. `RunRequest.contextPaths` 또는 API `context_paths`로 지정한 경로와, 기존 transcript에 기록된 관측 경로를 정규화한다. 존재하지 않는 새 파일도 허용하며 존재하는 상위 디렉터리까지 symlink를 확인한다. 각 대상 파일의 하위 폴더 지침도 상위 → 하위 순으로 추가한다.
4. `paths`가 있는 rules 파일은 해당 rules 폴더의 `.claude`를 포함한 프로젝트 폴더를 기준으로 대상 경로와 비교한다. 조건 없는 규칙은 그 폴더가 탐색될 때 적용한다. 여러 대상 중 하나에라도 규칙이 맞으면 포함한다.
5. 파일 내부의 Markdown `@path` 참조를 따라간다. 부모 파일을 먼저 넣고 참조 파일을 뒤에 넣는다. canonical 파일 경로를 기준으로 중복·순환을 제거한다. import는 부모의 적용 scope와 patterns를 상속한다.
6. 매 모델 호출 전에 다시 읽어 같은 내용이면 같은 fingerprint를 만든다. 변경·추가·삭제 시 새로운 전체 스냅샷으로 교체한다. 파일 내용 캐시는 아직 없으며 mtime만으로 갱신을 판단하지 않는다.

`Read`, `Write`, `Edit`의 성공 결과에는 실제 workspace 경로를 `metadata["iilocal.context_paths"]`로 기록한다. 이 경로는 다음 모델 호출부터 적용한다. 세션 artifact를 읽은 경로는 기록하지 않는다. `Glob`·`Grep`·`Bash` 문자열에서 접근 파일을 추측하지 않는다. 호스트 도구도 같은 metadata 계약을 사용할 수 있으나 모든 경로는 다시 루트 검증을 받는다.

새 파일에 처음 `Write`를 실행할 때 하위 지침을 미리 보게 하려면 `context_paths`를 지정한다. 현재 도구 실행을 사전에 중단하여 지침을 추가하고 재계획하는 기능은 없다. 기존 파일 편집에는 여전히 직접 `Read`와 변경 해시 검증이 필요하다. 지침 자동 읽기는 편집을 위한 읽기 이력을 만들지 않는다.

## 입력과 세션 기록

`ModelRequest.systemPrompt`는 호스트가 지정한 그대로 유지한다. 프로젝트 지침은 상대 파일명·적용 경로·본문·working_directory를 담은 JSON 형식의 임시 `MessageRole::User` 메시지로 전체 대화 앞에 넣는다. 프로젝트 파일이 호스트 지침을 재정의하거나 도구 권한을 부여하지 않는다는 설명을 붙인다. 원본 해시·fingerprint 등 감사용 값은 모델 입력에서 제외하여 토큰 낭비를 줄이고 API·이벤트 metadata로 제공한다. 실제 권한은 기존 `PermissionPolicy`·도구 검증이 계속 결정한다.

임시 조합 본문은 transcript에 저장하지 않는다. `context_paths`만 사용자 메시지 metadata에 저장하고, 도구 관측 경로는 도구 결과 metadata에 저장한다. resume와 fork는 이 경로를 유지하고 현재 파일 내용을 다시 읽는다. 과거 지침 원본의 영속 보관·정확한 과거 재생 기능은 아직 없다. 적용 경로는 세션에서 누적되며 자동 해제되지 않는다. 범위를 초기화하려면 새 세션을 만든다.

스냅샷이 바뀌거나 실행이 처음 시작되면 `instructions_loaded` 이벤트가 발생한다. 파일이 모두 삭제된 경우에도 비어 있는 새 스냅샷을 알린다. 이벤트에는 파일 경로, scope, parent, patterns, 원본 SHA-256, 변환 여부, 본문 byte 수, fingerprint를 넣고 본문은 넣지 않는다. API의 `agent.context.get`은 본문을 포함한 현재 스냅샷을 반환한다.

fingerprint는 `toJson(true)`에서 `fingerprint` 필드를 제외한 Qt compact JSON의 SHA-256이다. 파일 배열 순서·원본 SHA-256·변환 본문·정규화한 대상 경로가 포함되므로 이 중 하나가 달라지면 fingerprint도 달라진다. 파일별 `sha256`은 frontmatter와 주석을 포함한 원본 byte 기준이다.

입력이 모델 컨텍스트를 초과하면 기존 structured conversation의 `context_overflow`가 발생한다. 지침을 조용히 자르거나 오래된 대화를 자동으로 삭제하지 않는다. byte 제한은 tokenizer 기반 토큰 예산이나 요약을 대체하지 않는다.

## Markdown과 경로 규칙

Markdown은 MD4C의 CommonMark 파서로 구분한다. 일반 텍스트·제목·목록의 `@./file.md` 참조를 읽으며 상대 경로는 참조 파일의 실제 폴더를 기준으로 해석한다. UTF-8 파일명, `@./file\ name.md`, `@./file.md#heading`을 지원한다. 코드 블록과 인라인 코드의 참조는 읽지 않는다. block HTML comment의 닫힌 `<!-- ... -->` 구간은 제거하되 뒤의 일반 텍스트는 보존한다. 인라인 주석, 코드 안의 주석, 닫히지 않은 주석은 본문에 보존한다. 일반 HTML 태그 안의 참조는 읽지 않는다.

선두의 UTF-8 BOM은 파싱 전에 제거하고 원본 해시에 보존한다. LF·CRLF 줄바꿈을 지원한다. 파일 선두의 `---`부터 별도 줄의 `---`까지는 YAML frontmatter다. LibYAML로 파싱하며 `paths`는 문자열 또는 문자열 목록을 받는다. glob은 YAML에서 따옴표로 감싼다.

```markdown
---
paths:
  - "src/**/*.{cpp,h}"
  - "!src/generated/**"
---
이 폴더의 C++ 변경은 문서와 테스트를 함께 갱신한다.
@../../docs/cpp-conventions.md
```

지원 glob은 `*`, `**`, `?`, 문자 범위 `[a-z]`, 부정 문자 범위 `[!a-z]`, 쉼표 목록, `{cpp,h}` 확장, 앞의 `!` 제외다. 슬래시가 없는 패턴은 하위 폴더의 basename도 매칭한다. `/`로 시작하면 scope 루트에 고정한다. 디렉터리 이름은 그 하위에도 적용한다. 패턴 목록은 앞에서 뒤로 적용하여 마지막 매칭이 포함·제외를 결정한다. `paths: '**'`만 있으면 무조건 적용한다. extglob, 백슬래시 glob escaping, 전체 gitignore 규격은 지원하지 않는다. `paths` 조건은 `.claude/rules`에만 적용한다.

기본 탐색 파일이 없거나 참조한 일반 경로가 없으면 건너뛴다. 존재하는 파일의 잘못된 UTF-8·NUL·잘못된 YAML·지원하지 않는 paths 타입·깨진 symlink·루트 이탈은 오류다. YAML alias를 허용하지 않는다. 기준 구현의 잘못된 YAML 값 자동 quoting 복구는 구현하지 않았다.

## 상한과 호스트 제어

| 설정 | 기본값 |
|---|---:|
| 파일당 원본 byte | 128 KiB |
| 한 번의 조합에서 읽는 전체 byte | 512 KiB |
| 읽는 파일 수 | 256 |
| rules 탐색 entry 수 / scope 디렉터리 수 | 각각 4,096 |
| import 깊이 | 5단계, 최초 파일 depth 0 포함 |
| 대상 경로 수 | 128 |
| YAML frontmatter | 16 KiB, nesting 16, 이벤트 4,096 |
| 확장된 glob 목록 / 한 glob 길이 | 128개 / 512자 |

범위 밖 파일을 읽지 않으며 home·managed 설정·네트워크를 자동 탐색하지 않는다. `~/...`와 URL import는 지원하지 않는다. 루트 안의 symlink는 canonical 경로로 중복을 제거한다. Unix 파일 읽기는 root descriptor에서 `openat`·`O_NOFOLLOW`로 구성 요소를 다시 열고 일반 파일인지 검사한다. FIFO 같은 특수 파일은 읽지 않는다. Windows에서는 canonical 경로 검증과 Qt 파일 읽기를 사용하며 Unix descriptor 방식의 경합 방지는 적용되지 않는다. 같은 OS 사용자 전체를 격리하는 샌드박스는 아니다.

`ProjectContextOptions.excludes`는 canonical root-relative glob 목록이며 자동 파일과 import에 적용한다. daemon은 `--agent-context-exclude PATTERN`을 반복해서 받는다. `enabled=false` 또는 `--agent-no-project-context`는 자동 로딩을 끈다. 앱 RPC로 호스트의 root·상한·excludes·도구 정책을 바꿀 수 없다. 잘못된 신규 `context_paths`를 가진 실행은 사용자 메시지 기록과 모델 호출 전에 실패한다.

```cpp
iiLocalLLM::agent::RunRequest request{session.id, "이 파일을 검토하라"};
request.contextPaths = {"src/Example.cpp"};
const auto result = engine.run(request).result.get();
const auto context = engine.context(session.id);
```

```json
{"method":"agent.context.get","params":{"session_id":"SESSION_ID","context_paths":["src/Example.cpp"]}}
```

## 기준 소스와 남은 차이

관찰 기준은 `Exhen/claude-code-2.1.88`의 커밋 `c8cd253554319f32ff64ff7000636199f720c9bc`이다. 해당 소스는 동작 분석에만 사용했으며 iiLocalLLM에 복사하지 않았다.

- [claudemd.ts](https://github.com/Exhen/claude-code-2.1.88/blob/c8cd253554319f32ff64ff7000636199f720c9bc/source/src/utils/claudemd.ts): Markdown 변환·참조 추출, 탐색 순서, nested rules, 변경 감지. `processMemoryFile` 앞의 주석은 includes-first라고 쓰지만 실제 실행문은 부모를 먼저 push한다. 이 문서는 실행문을 기준으로 한다.
- [frontmatterParser.ts](https://github.com/Exhen/claude-code-2.1.88/blob/c8cd253554319f32ff64ff7000636199f720c9bc/source/src/utils/frontmatterParser.ts): YAML 문자열/목록, 쉼표·brace 패턴, 잘못된 YAML 값의 자동 quoting 복구.
- [attachments.ts](https://github.com/Exhen/claude-code-2.1.88/blob/c8cd253554319f32ff64ff7000636199f720c9bc/source/src/utils/attachments.ts): 도구 접근 후 하위 지침 적용과 관측 상태 관리.

현재 구현은 workspace 안의 프로젝트 지침에 한정한다. managed/user/추가 디렉터리 설정, 외부 import, worktree 중복 제외, 기준 구현 전체 glob 의미, 설정 이벤트 캐시, 자동·팀 메모리, IDE 첨부, 일반 첨부, 요약, microcompact와 압축 경계는 남아 있다. `AGENTS.md` 지원과 앱별 고정 경계는 iiLocalLLM의 호스트 계약이다. 전체 대응 상태는 `catalog/harness-parity.json`의 context 항목을 계속 partial로 유지한다.
