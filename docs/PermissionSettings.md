# 계층형 권한 설정

0.19.0의 C++ `SettingsPermissionPolicy`는 파일 기반 권한 설정을 읽는다.
`PermissionRule` 레이아웃과 `PermissionPolicy` 가상 함수가 바뀌므로 ABI는
0.19이다. 헤더·라이브러리·소비자를 함께 다시 빌드한다. 기존 네이티브
`RulePolicy` 문법·스킬 권한 수명은 [Permissions.md](Permissions.md)를 따른다.

## 출처와 우선순위

호스트가 지정한 원래 workspace를 기준으로 아래 순서로 읽는다. 배열은 값을
합치며 scalar는 뒤의 값이 우선한다. 규칙은 출처와 기준 경로를 유지하고
Deny → Plan 제한 → Ask → Allow → mode 순서로 판단한다.

| 순서 | 파일 | `/pattern` 기준 |
|---|---|---|
| userSettings | `userDirectory/settings.json` | 지정한 사용자 설정 디렉터리 |
| projectSettings | `workspace/.claude/settings.json` | 원래 workspace |
| localSettings | `workspace/.claude/settings.local.json` | 원래 workspace |
| flagSettings | 호스트의 `flagFiles`, 그 뒤 `inlineSettings` | 파일의 부모, 인라인은 workspace |
| policySettings | `managedDirectory/managed-settings.json`, 그 뒤 `managed-settings.d/*.json` | 원래 workspace |

관리 조각은 숨김 파일을 제외한 `.json` 파일을 이름순으로 읽는다. 존재하지
않는 일반 출처는 생략하고 명시한 flag 파일은 필수이다. 빈 파일은 빈 객체다.
POSIX에서는 파일·중간 경로의 심볼릭 링크, 비정규 파일, 읽기 실패, 잘못된
JSON·규칙을 오류로 처리한다. 관리 디렉터리 읽기 오류도 실패한다. 읽지 못한
Deny를 조용히 버리지 않는다. 명시한 최상위 경로 자체는 호스트의 권한 선택이다.

`enabledSources`는 `user`, `project`, `local`만 선택한다. flag·managed는
필터로 끌 수 없다. 홈·사용자·관리 경로는 명시하며 SDK가 다른 앱의 HOME 설정을
자동으로 읽지 않는다. 각 도구 판단과 조회에서 다시 읽으므로 수정·삭제는 다음
호출부터 반영된다. 여러 파일의 읽기는 파일 시스템 전체의 원자적 snapshot이
아니다. 갱신자는 임시 파일과 rename을 사용하는 것이 적절하다. 진행 중인 도구의
이미 끝난 판단을 소급 변경하지 않는다.

관리 출처의 `allowManagedPermissionRulesOnly: true`는 사용자·프로젝트·로컬·
flag 규칙과 `--allow`/`--agent-allow`를 최초 판단부터 제외한다. 호스트 고정 규칙,
설정 쓰기 보호, 현재 스킬 호출의 추가 권한은 유지한다. 같은 플래그를 사용자나
프로젝트에 넣어도 관리 출처를 흉내 낼 수 없다. mode·scalar 병합은 규칙 필터와 별개다.

## 지원 설정

```json
{
  "permissions": {
    "allow": ["Read", "Edit(/output/**)", "Bash(git status:*)"],
    "deny": ["Read(.env)", "Edit(/output/private/**)"],
    "ask": ["Bash(rm:*)"],
    "defaultMode": "dontAsk",
    "disableBypassPermissionsMode": "disable"
  }
}
```

mode는 `default`, `acceptEdits`, `dontAsk`, `bypassPermissions`, `plan`이다.
명시적 호스트 mode → 병합된 defaultMode → fallbackMode → default 순서이며
`disableBypassPermissionsMode: "disable"`이면 bypass 후보를 건너뛴다.
`disableAutoMode: "disable"`은 받지만 auto mode·자동 분류기는 아직 없다.

비어 있지 않은 `permissions.additionalDirectories`와 알 수 없는 permissions
필드는 `unsupported_features`에 기록하고 실행을 실패시킨다. 추가 디렉터리는
파일 도구의 workspace 범위와 아직 연결하지 않았다. 일반 `env`, model, hooks
등은 적용하지 않고 이름만 미지원으로 보고하며 값은 조회에 노출하지 않는다.

현재 읽은 설정 경로와 workspace 하위 `.claude/settings.json`·`settings.local.json`
쓰기에 호스트 Ask를 적용한다. 일반 Edit/Write/acceptEdits로 권한 설정 자체를
덮어쓸 수 없고 `dontAsk`에서는 Deny가 된다. 임베딩 호스트의 기존 승인 callback은
Ask를 처리할 수 있지만 원격 호출자가 이를 바꾸는 API는 없다. Bash 프로그램
전체의 부작용을 차단하는 OS 샌드박스는 아니며 전체 BashSecurity는 구현 중이다.

## 파일 규칙과 한도

설정 파일 패턴은 gitignore 방식이며 설정 `Edit`는 `Write`에도 적용한다.
네이티브 생성한 `PermissionRule`은 기본 `settingsSyntax=false`로 기존 문법이다.

- bare basename은 하위 디렉터리에도 적용한다. `Read(.env)`는 `sub/.env`도 잡는다.
- `/src/**`는 출처 기준, `//absolute/path`는 파일 시스템 루트, `~/path`는
  호스트 homeDirectory 기준이다. 나머지는 workspace 기준이다. 부정 규칙의
  root는 참조의 계산 순서대로 workspace라서 사용자 출처의 `/...` 긍정 규칙과
  `!/...` 부정 규칙의 기준이 다를 수 있다. 이 문법이 workspace 밖 실행을 허용하지 않는다.
- `*`, `**`, `?`, 문자 집합·범위, 주석·escape, trailing slash, `!` 부정을 처리한다.
  마지막 `/**`는 참조처럼 먼저 제거한다. 제외된 부모 내부를 부정 규칙 하나로
  다시 포함하지 않는다. 같은 root의 동일 패턴은 순서를 중복하지 않는다.
- Allow는 lexical·canonical 양쪽이 workspace 안이며 둘 다 일치해야 한다.
  Deny/Ask는 어느 쪽이든 일치하면 적용한다. 파일 도구의 실행 직전 대상 재검증도 유지한다.

libgit2 v1.9.7의 작은 C wildmatch를 비공개로 사용한다. Git repository나
`.gitignore`를 읽지 않으며 별도 프로세스를 시작하지 않는다. 한 패턴 집합 검사에
500,000 단계, 100ms 협력식 기한, 재귀 깊이 128을 적용한다. 초과는 ResourceLimit이며
Deny 미일치로 취급하지 않는다. C callback과 C++ 경계에서 취소를 확인한다.
입력은 패턴 4,096·경로 16,384 UTF-16 단위까지다.

Unicode는 Qt lowercase 후 정렬한 비ASCII UTF-16 단위를 바이트 알파벳에 매핑한다.
동시에 다른 비ASCII 단위가 128개를 넘으면 실패한다. `?`는 UTF-16 한 단위다.
JavaScript의 모든 Unicode case-folding 예외, Windows drive/UNC 규칙, POSIX
문자 클래스까지 동일하다고 주장하지 않는다. 별도 node-ignore 7.0.5로 산출한
50개 사례의 URL·SHA·기대값을 `tests/permission_settings_patterns.json`에 보존한다.
node-ignore는 검사 기준으로만 사용했고 제품 의존성에 포함하지 않았다.

기본 읽기 한도는 파일 128KiB, 전체 1MiB, 출처 128개다. C++ Options의 허용
최대는 1MiB/16MiB/1,024개다. 전체 RulePolicy 규칙 한도에는 설정 쓰기 보호 규칙도
포함한다. 홈 접두사 규칙에는 homeDirectory가 필요하다.

## 호스트·앱 연결

독립 실행 호스트는 명시적으로 opt in한다. 호스트 설정 파일은 workspace 밖의
현재 사용자 소유 일반 파일이어야 하고 POSIX 권한은 0600 등으로 제한한다.
심볼릭 링크는 받지 않으며 128KiB까지 읽는다. 상대 경로는 이 파일의 부모 기준이다.

```json
{
  "user_directory": "user",
  "managed_directory": "managed",
  "home_directory": "/host-selected/home",
  "enabled_sources": ["user", "project", "local"],
  "flag_files": ["team-settings.json"],
  "settings": {"permissions": {"defaultMode": "dontAsk"}}
}
```

`iiLocalLLMD --agent-permission-settings FILE`과 `iillm-mcp --permission-settings FILE`은
같은 로더를 사용한다. 선택적 `mode`는 호스트 override다. 설정하지 않은 호스트는
기존 RulePolicy다. 설정 오류는 서비스·모델 초기화 전에 검사하며 시작 뒤의 오류는
다음 판단을 실패시킨다. 경로·mode·설정 객체를 원격 run 입력으로 받지 않는다.

| 연결 | 조회 |
|---|---|
| C++ | `SettingsPermissionPolicy::snapshot()`, `Engine::permissions(sessionId)` |
| 인증 HTTP·IPC | `agent.permissions.get` + `session_id` |
| 얇은 CLI | `iillm --auth-file FILE agent permissions get SESSION` |
| MCP stdio·인증 Streamable HTTP | `iiLocalLLM.agent.permissions.get`, 빈 인자 |

provider·mode·규칙·출처 파일·SHA·미지원 이름을 반환한다. API는 앱별 세션 소유권을
확인하고 MCP는 호스트 workspace 정책만 조회한다. 이는 현재 호스트 정책이며
과거 호출의 스킬 추가 권한·자식 profile까지 합친 역사적 실행 권한 보고서가 아니다.
사용자 정의 정책은 `describe()`를 구현하지 않으면 inspection_supported=false다.

## 참조와 남은 범위

참조는 공개 미러 커밋 `c8cd253554319f32ff64ff7000636199f720c9bc`의
`utils/settings/{constants,settings,types}.ts`, `utils/permissions/`
`{permissionsLoader,permissionSetup,permissions,filesystem}.ts`다. 미러의 진위나
외부 서버 gate까지 검증하지 않았다. 참조 TypeScript·프롬프트를 SDK로 옮기지 않았다.

참조의 잘못된 설정을 보고·생략하는 경로 대신 실행을 실패시킨다. managed-only의
초기 CLI seed와 이후 sync 정리 사이의 차이도 재현하지 않고 처음부터 CLI 규칙을
제외한다. 참조의 동일 resolved 파일 출처 중복 제거, session 전용 `.claude/**`
쓰기 예외, 모든 특수 경로 보호까지 동일성을 검증하지 않았다. command/스킬 권한은
참조 sync의 정리 대상에서 빠져 있으며 현재 구현도 호출 수명 안에서 유지한다.

추가 디렉터리·원격 관리·MDM·Windows 레지스트리·플러그인 설정·환경 적용·
마이그레이션·자동 분류·원격 승인 응답·OS 샌드박스는 남아 있다. 전체 하네스나
전체 settings 호환이 완료된 것은 아니다.
