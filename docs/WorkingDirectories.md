# 추가 작업 디렉터리

0.20.0은 `permissions.additionalDirectories`와 호스트 CLI의 추가 디렉터리를
C++ 파일 도구·권한 판정·자식 에이전트에 연결한다. 설정 로더·경로 해석은 기존
Qt와 C++을 사용하며 새 생산 의존성을 추가하지 않았다. `ToolContext`, 권한 정책의
가상 함수, 설정 Options/Snapshot이 바뀌므로 ABI는 0.20이다.

## 디렉터리 선택과 수명

```json
{
  "permissions": {
    "additionalDirectories": ["../shared-assets", "/host/selected/project"],
    "allow": ["Edit(//host/selected/project/output/**)"],
    "defaultMode": "dontAsk"
  }
}
```

사용자·프로젝트·로컬·flag·managed 출처의 배열을 읽고 명시적 CLI 디렉터리를
덧붙인다. `enabledSources`는 해당 파일 출처에도 적용된다. managed-only는
도구 규칙을 필터링하고 디렉터리 배열은 별도로 합친다. 상대 경로는 모든 출처에서
원래 workspace 기준이다. 파일 권한 패턴의 `/` 기준과 혼동하지 않는다.
`~/`와 `~`는 명시적 `homeDirectory`가 필요하고 다른 사용자 홈 치환은 받지 않는다.

동일 입력은 조회에 처음 출처만 표시하고 이미 포함된 경로는 중복 확장하지 않는다.
같은 입력을 여러 출처가 지정해도 canonical 바인딩은 출처마다 보관한다. 파일 설정과
CLI가 같은 링크를 지정한 뒤 파일 설정만 갱신·삭제해도 CLI의 최초 대상은 유지한다.
출처별로 유효한 canonical 대상이 서로 다르면 작업 범위에는 모두 포함한다. 존재하지 않는 경로,
일반 파일, 빈 입력은 범위에 포함하지 않고 조회에 상태를 남긴다. 디렉터리는
고유 입력 기준 기본 128개, 호스트 설정의 최대 1024개까지이며 입력은 4096 UTF-16 단위까지다.
잘못된 타입·NUL·한도 초과·명시하지 않은 홈 치환은 오류이다.

설정 변경·삭제는 다음 호출에서 반영된다. 새로 나타난 설정 경로도 검증한 뒤
포함한다. 디렉터리의 심볼릭 링크와 조상 링크는 최초 확인한 canonical 대상에
묶는다. 같은 출처 파일의 바이트가 유지되는 동안 링크를 바꿔도 새 대상에 권한을
주지 않는다. 원래 canonical 경로의 권한은 유지하며 조회에는 `target_changed`와
현재 관측 경로를 표시한다. 설정 파일의 SHA가 바뀌면 다시 바인딩한다. CLI 경로는
호스트 정책 객체의 수명 동안 유지한다. 사라진 디렉터리는 현재 범위에서 제외한다.

바인딩 표는 현재 설정 입력만 보관하고 성공한 snapshot의 검증이 끝난 후 교체한다.
설정 조회는 정책 객체 안에서 직렬화한다. 잘못된 설정이나 취소된 snapshot이
부분적인 디렉터리 권한을 남기지 않는다. 여러 설정 파일을 읽는 과정 전체가
파일 시스템의 원자적 트랜잭션인 것은 아니다.

0.26의 승인된 addDirectories/removeDirectories는 session/cliArg 상태를 세션별로 보존한다.
삭제는 해당 세션의 같은 입력을 출처 전반에서 억제하며 명시적 추가는 억제를 해제한다.
런타임 바인딩은 fork/clear/자식 접수에 복사하고 다른 세션의 기본 바인딩을 바꾸지 않는다.
저장 위치·재시작·세부 예외는 [PermissionUpdates.md](PermissionUpdates.md)를 따른다.

## 도구 실행

`Read`·`Write`·`Edit`는 추가 디렉터리의 절대 경로와 workspace 기준 상대 경로를
받는다. 권한의 Deny·Ask·mode·현재 스킬의 allowed-tools·자식의 도구 제한은
그대로 적용한다. 디렉터리 추가만으로 Write나 Bash의 실행 권한을 얻지는 않는다.
설정의 gitignore 패턴 기준은 [PermissionSettings.md](PermissionSettings.md)를 따른다.
workspace 밖 대상에는 `//absolute/path` 패턴을 사용한다.

원래 경로와 해석된 경로 모두 작업 디렉터리 중 하나에 있어야 한다. 파일 도구는
권한 판정 전에 대상을 고정하고 실행 직전에 다시 확인한다. 권한 callback이나
ToolStarted 중 경로를 바꾸면 다른 파일에 실행하지 않는다. 기존 파일은 같은
세션·압축 revision에서 완전히 읽어야 수정할 수 있고, 읽은 뒤 변경되면 다시 읽어야
한다. 추가 디렉터리의 루트 자체도 파일로 교체할 수 없다.

`Glob`은 선택적 `path`를 받는다. 기본은 workspace이며 `path`는 권한이 있는
디렉터리여야 한다. 결과 `paths`는 응답 `root` 기준 상대 경로이다. 최대 1000개
결과·10000개 파일을 검사하고 `truncated`를 반환한다. `Grep`은 기존 `path`로
추가 디렉터리나 파일을 선택한다. Grep 결과 경로는 기존과 같이 workspace 기준이다.
두 검색 도구는 선택한 시작 경로를 실행 전에 고정하고 호스트 비공개 경로를 제외한다.
개별 Read 규칙을 모든 검색 결과에 적용하는 참조의 전체 검색 의미까지 구현한
것은 아니다. 도구별 규칙을 명시하며 검색 의미의 나머지는 parity 목록에 남긴다.

Bash 리다이렉션의 파일 규칙도 추가 디렉터리를 인식한다. 실행 cwd는 계속 원래
workspace이다. 영구 cwd 변경과 OS 샌드박스는 포함하지 않는다. 일반 프로그램의
부작용이나 파일 시스템 검사와 OS 파일 열기 사이의 모든 경쟁 조건을 차단하는
기능으로 해석하지 않는다.

## 호스트 비공개 저장소

추가 루트가 호스트 저장소의 부모를 포함하더라도 C++ 파일·검색 도구에서
인증 파일·호스트 설정·다른 세션 저장소를 노출하지 않는다. 독립 실행 호스트는
자신의 state, 인증 파일, 명시적 권한/프로파일/MCP 설정과 앱 발견 레지스트리를
비공개 경로로 등록한다. MCP의 기본 `.iilocal-llm` 저장소도 해당한다.
현재 세션의 artifacts와 소유한 shell 출력은 읽을 수 있고 파일 도구로 수정하지 않는다.
비공개 경로의 원래 이름과 canonical 경로를 모두 검사한다. macOS의 현재 검사
볼륨에서는 대문자로 바꾼 인증 파일·비공개 디렉터리의 읽기/쓰기 요청도 거부했다.
모든 파일 시스템의 Unicode 정규화·이름 별칭 동작을 검증한 결과는 아니다.

임베딩 앱은 소유한 비공개 경로를 등록한다. SDK가 다른 앱의 임의 저장소 위치를
알아서 추측하지 않는다.

```cpp
#include <agent/PermissionSettings.h>

iiLocalLLM::agent::PermissionSettingsOptions settings;
settings.workingDirectory = workspace;
settings.additionalDirectories = {sharedAssets};
auto policy = std::make_shared<iiLocalLLM::agent::SettingsPermissionPolicy>(settings);
auto tools = std::make_shared<iiLocalLLM::agent::ToolRegistry>();
iiLocalLLM::agent::registerWorkspaceTools(*tools, workspace, {},
    QStringList{privateState, credentialFile});
```

`ToolRunner`는 호출자가 채운 `ToolContext.workingDirectories`를 현재 정책의
`workingDirectories(context)` 결과로 교체한다. `RulePolicy`와 기본 사용자 정의
정책은 원래 workspace만 제공한다. 사용자 정의 정책이 추가 범위를 제공하려면
해당 가상 함수를 구현한다. 원격 tool 인자로 이 필드를 지정하는 경로는 없다.

## API·CLI·MCP·자식 에이전트

- daemon: `--agent-add-dir DIR`, 반복 가능.
- MCP: `--add-dir DIR`, 반복 가능.
- 이 플래그만 사용하면 디스크 설정은 읽지 않는다. 계층형 파일 설정은 기존
  `--agent-permission-settings FILE` 또는 `--permission-settings FILE`로 선택한다.
- C++ `Engine::permissions`, 인증 API `agent.permissions.get`, CLI
  `agent permissions get`, MCP `iiLocalLLM.agent.permissions.get`은
  `working_directories`와 `additional_directories`를 반환한다.

추가 디렉터리의 출처 레코드에는 input, source, path, canonical_path, status가 있다.
status는 active/already_covered/not_found/not_directory/empty/target_changed 중 하나다.
target_changed에는 observed_canonical_path도 있다. 없는 필드는 해당 상태에서
관측되지 않은 값이다. `working_directories`에는 원래 workspace와 유효한 경로
별칭·canonical 대상이 들어간다. 비공개 경로와 도구 규칙은 그 범위 안에도 적용된다.

에이전트는 매 모델 턴에 현재 디렉터리를 별도 문맥으로 받는다. 이 메시지나
도구 호출 기록을 나중의 권한으로 복원하지 않는다. 원래 workspace 밖의 파일 작업은
원래 프로젝트의 instruction target 목록을 늘리지 않는다. 추가 디렉터리의
CLAUDE.md/AGENTS.md 자동 로딩을 켜는 별도 옵션은 아직 제공하지 않는다.
자식 에이전트도 현재 부모 정책의 경로를 사용하고 자신의 도구·읽기 전용·Plan
제한을 유지한다. 부모 설정에서 경로를 지우면 이후 자식 실행도 접근할 수 없다.

## 참조와 검증 범위

공개 미러 커밋 `c8cd253554319f32ff64ff7000636199f720c9bc`의
`commands/add-dir/validation.ts:31-92`, `utils/permissions/permissionSetup.ts:993-1024`,
`filesystem.ts:667-705`, `PermissionUpdate.ts:122-147,244-264,297-313`을 조사했다.
미러의 진위와 서버 측 gate는 검증되지 않았다. TypeScript 구현이나 프롬프트를 복사하지 않았다.

참조는 시작 시 검증한 디렉터리와 세션 중 추가/삭제 명령을 관리한다. SDK는 현재
파일 변경에 따른 갱신을 수행하고 설정 SHA에 canonical 바인딩을 연결한다.
slash `/add-dir` UI, 원격 디렉터리 변경/승인·설정 쓰기 API, 자동 추가 instruction
로딩, 모든 플랫폼의 경로 정규화와 전체 BashSecurity는 남아 있다. 디렉터리 조회가
파일마다 최종 권한을 허용한다는 뜻도 아니다. 최종 실행 증거는
[Verification.md](Verification.md)에 별도로 기록한다.
