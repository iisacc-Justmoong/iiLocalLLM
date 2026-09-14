# 도구 권한과 스킬 호출 범위

0.18.0에서 C++ `RulePolicy` 인자 규칙과 스킬 `allowed-tools`를 추가했다. 현재 0.20.0은 추가 작업 디렉터리의 권한 경계를 연결하여 ABI가 **0.20**이다. 소비자는 헤더와 라이브러리를 함께 갱신한다. 전체 참조 권한 시스템은 아직 partial이다.

## 판정 순서

명시적 Deny가 먼저 적용된다. 다음으로 Plan 모드의 쓰기·외부 실행 제한, 명시적 Ask, 호스트와 현재 호출의 Allow, 기존 모드 기본값 순으로 판정한다. DontAsk에서는 최종 Ask를 Deny로 바꾼다. Bypass와 스킬 권한도 Deny·Ask를 무시하지 않는다. Plan의 내부 작업 상태·소유 실행 중단 예외는 유지한다. Read-only 도구는 기존처럼 기본 허용되므로 `Read(src/**)` 같은 Allow 하나가 나머지 Read 경로를 금지하는 것은 아니다.

`RulePolicy`가 `ToolContext::allowedTools`를 해석한다. 사용자 정의 `PermissionPolicy`의 결과를 별도 래퍼가 뒤집지 않는다. 자식 프로파일의 도구 목록·거부 목록·read-only·Plan·DontAsk, 부모 `toolFilter`와 재개 시 원래/현재 프로파일 교집합도 유지한다. 규칙은 도구의 입력 스키마, 작업 디렉터리 제한, 기존 파일의 읽기 및 변경 감지를 대체하지 않는다.

```cpp
using namespace iiLocalLLM::agent;
auto policy = std::make_shared<RulePolicy>(PermissionMode::Default,
    QList<PermissionRule>{
        {"Skill(review:*)", PermissionBehavior::Allow},
        {"Bash(git status:*)", PermissionBehavior::Allow},
        {"Write(private/**)", PermissionBehavior::Deny},
        {"Bash(rm:*)", PermissionBehavior::Ask}});
```

## 규칙 문법

`parsePermissionRules(QStringList)`는 괄호 밖의 쉼표·공백을 나눈다. 문자열과 문자열 목록을 같은 방식으로 처리한다. 괄호 안의 공백은 유지하고 `\(`·`\)`·`\\`를 지원한다. 닫히지 않은 괄호, 중첩된 비이스케이프 괄호, 잘못된 도구 이름, NUL은 오류이다. 참조가 일부 잘못된 괄호를 단순 도구 이름으로 취급하는 것과 다르다. 목록당 256개·64 KiB, 규칙당 4,096자이며 중복은 제거한다. 호스트와 호출의 합산 허용 목록도 이 제한을 지킨다.

| 규칙 | 범위 |
|---|---|
| `Write`, `mcp__society__*` | 도구 이름 또는 wildcard 전체 |
| `mcp__society` | 정확히 society 서버에 속하는 도구. societyOther에는 적용되지 않음 |
| `Write(src/**)`, `Edit(src/*.cpp)`, `Read(config.json)` | `path` 인자. `*`는 한 경로 요소, `**`는 여러 요소, `?`는 한 문자 |
| `Skill(review)` | `/`를 제거한 호출 이름의 정확한 일치 |
| `Skill(review:*)`, `Agent(reader:*)` | 스킬 이름 또는 `subagent_type`의 접두사 |
| `Bash(git status)` | 정확한 명령 또는 분리된 leaf 명령 |
| `Bash(git status:*)` | 완전한 접두사와 뒤 공백 경계 |
| `Bash(git * status)` | 분리한 명령 안의 wildcard |
| `Bash(*)`, `Bash()` | 도구 전체 권한 |

파일 규칙은 세션 작업 디렉터리 기준이며 절대 경로도 사용할 수 있다. `..`를 정리하고 존재하는 상위 경로의 canonical 경로를 해석한다. Allow는 원래 경로와 해석된 경로가 모두 패턴에 맞고 workspace 내부여야 한다. Deny/Ask는 둘 중 하나가 맞으면 적용된다. 따라서 `src/link`가 `private`를 가리키면 `Write(src/**)`로 새 권한을 얻지 않는다. 홈 확장·참조의 추가 디렉터리/설정 출처별 상대 경로 규칙은 제공하지 않는다. `Read`/`Write`/`Edit`, `Skill`, `Agent`, `Bash` 외 도구의 인자 패턴은 자동 허용하지 않는다. 실제 제품에서 다른 리소스 규칙이 필요하면 호스트 정책이 결정한다.

## Bash 검사

고정된 tree-sitter 0.27.0과 tree-sitter-bash 0.25.1의 C 파서로 AST를 만든다. 명령 전체 문자열에 접두사를 한 번 적용하지 않고 `&&`·`;`·파이프·줄바꿈·서브셸 안의 명령을 각각 검사한다. 여러 Allow 규칙의 합으로 모든 명령을 덮어야 자동 허용한다. Deny/Ask는 중첩 명령 중 하나만 맞아도 적용한다. 따옴표·이스케이프로 쓴 명령 이름과 명령 앞의 변수 할당도 거부 검사를 피하지 못한다.

자동 허용은 정적으로 확인한 명령에 제한한다. 변수/명령/프로세스 치환, heredoc, 백그라운드 연산자, 디렉터리 변경, 제어문, 함수와 분석하지 못한 구문은 접두사 허용을 얻지 않는다. `env`, `timeout`, `sudo`, `xargs`, `exec`, 셸 인터프리터·`eval` 등의 래퍼는 이번 구현에서 안전하게 풀어내지 않으며 인자 Deny/Ask가 있으면 보수적으로 해당 결정을 적용한다. 예를 들어 `Bash(rm:*)` Deny는 `env -i printf ok`도 거부할 수 있다. 참조의 정교한 안전 환경변수·래퍼 제거·읽기 전용 명령 분류는 아직 없다. 실행 파일의 별칭이나 `/bin/rm`과 `rm`의 동일성을 추론하지 않는다. Bash 패턴의 wildcard는 `*`·`?`이며 대괄호는 문자 그대로 검사한다.

리다이렉션은 정적인 workspace 내부 경로와 기본 파일 디스크립터 복제만 자동 허용한다. 출력/입력 대상에는 Write/Read Deny·Ask도 적용한다. 고정한 파서가 오류로 반환하는 `<>`, 대상과 후행 인자가 같은 노드에 묶이는 구문, 알 수 없는 대상이나 cwd 변경 뒤의 파일 대상은 파일 제한을 증명할 수 없어 보수적으로 처리한다. 이때 Read 또는 Write 인자 Deny·Ask가 하나라도 있으면 해당 결정을 적용하고 접두사 자동 허용을 하지 않는다. 파일 규칙은 임의 프로그램 인자의 의미를 해석하지 않는다. 예를 들어 `Write(private/**)`는 Bash에서 실행되는 임의 프로그램의 모든 파일 쓰기를 가로채는 기능이 아니다. 정확한 명령 전체나 `Bash` 전체를 명시적으로 허용하면 복합 구문도 호스트가 허가한 실행으로 취급한다. 명시적 거부는 여전히 우선한다.

따옴표·이스케이프를 해석한 실행 파일명에 공백이 있으면 접두사 자동 허용을 하지 않는다. 따라서 `'git status'`라는 단일 실행 파일을 `git`과 `status` 인자로 잘못 합쳐 `Bash(git status:*)` 권한을 부여하지 않는다. `'git' 'status'`처럼 실제 단어 경계가 유지된 정적 호출은 검사할 수 있다.

입력은 64 KiB, AST 순회는 8,192노드, 파싱·순회 시간은 100ms로 제한하고 취소를 확인한다. 오류·상한 도달 시 접두사 자동 허용을 하지 않는다. 셸 의미 전체, git alias/외부 프로그램의 동작, 동적 코드 실행을 증명하거나 OS 접근을 격리하는 샌드박스는 아니다. 기존 Bash 실행기와 별도로 전체 참조 BashSecurity·경로/도메인 분류·OS 샌드박스 구현은 계속 필요하다.

## 스킬 권한의 수명

```yaml
allowed-tools: 'Read, Bash(git status:*) Write(src/**)'
```

배열 표기도 지원한다. 목록 조회·프로파일 사전 로딩·과거 메시지 복원만으로 권한을 추가하지 않는다. 직접 사용자 스킬 호출은 해당 스킬의 권한을 적용한다. 모델의 `Skill` 호출은 비어 있지 않은 목록이 있으면 read-only로 취급하지 않고 별도의 권한 판정을 거친다. 호스트는 `Skill(name)` Allow나 기존 콜백으로 이를 허용할 수 있다. 인증 API/MCP 호출도 이 동일한 엔진 경로를 이용한다.

인라인 모델 호출의 권한은 성공한 native Skill 결과가 저장된 뒤 활성화된다. 같은 모델 응답에서 뒤따르는 직렬 도구에도 적용되며, `AfterTool` 차단이나 실패 결과에서는 활성화되지 않는다. Skill은 병렬 실행의 경계이다. 자동 압축과 내부 후속 턴은 현재 권한을 유지한다. 실행 종료, 새 `Engine::run`, 세션 분기·재시작·복원에서는 스킬 권한을 복원하지 않는다. 실행 중 새 큐 prompt가 전달되면 스킬이 추가한 권한을 초기화하며 단순 notification은 초기화하지 않는다.

fork 스킬은 권한을 자식에만 더한다. 일반 Agent와 fork 자식은 호출 시 부모의 현재 권한을 상속하되 프로파일 제한 안에서 실행한다. 백그라운드 시작 시 그 범위를 고정한다. 재개는 현재 호출자의 범위를 사용하므로 이전 실행의 스킬 권한이 지속되지 않는다. 호스트 전용 `RunRequest::allowedTools`는 해당 실행의 기본 권한으로 남고 API·IPC·MCP JSON에서는 받지 않는다.

## 권한 판정과 실행 스냅샷

`Tool::prepare`는 입력 검증과 `BeforeTool` 인자 변경 후 한 번 호출한다. 실행 부작용 없이 파일을 읽어 `PreparedTool`의 구체적인 정의와 실행 콜백을 만든다. 이름·입출력 스키마를 바꿀 수 없다. 도구 실행기는 이 정의로 권한을 판정하고 같은 콜백을 실행한다. 기존 `Tool::execute` 구현은 그대로 동작한다.

native Skill은 파일 경로·SHA-256·인자·allowed_tools를 고정한다. 권한 판정 뒤 파일이 바뀌어도 다시 읽지 않는다. `PermissionRequested` 이벤트의 `permission_preview._meta.skill`과 호스트 콜백의 decision.reason에는 요청한 권한과 출처가 있으며 본문은 포함하지 않는다. 원격 클라이언트가 권한 응답을 보내는 대화형 중개 API는 아직 없다. 데몬/MCP 서버의 `--agent-allow`/`--allow`는 같은 인자 규칙을 받는다.

Read/Write/Edit도 canonical 대상을 준비하고 실행 직전에 원래 경로와 고정된 대상이 같은지 재확인한다. 권한 질문이나 ToolStarted 관찰자 동안 경로가 다른 링크로 교체되면 실패한다. 기존 파일 도구의 경합 검사를 유지하지만 파일 I/O 전체를 openat 디스크립터로 고정한 OS 수준 경합 방어는 아니다. 현재 비-POSIX 실행기의 Bash라는 도구는 cmd.exe를 실행하므로 Bash 인자 규칙으로 자동 허용하지 않으며, 인자 Deny/Ask는 보수적으로 적용한다. 해당 플랫폼의 전체 셸 의미는 별도 구현이 필요하다.

비교 근거는 참조 커밋 `c8cd253554319f32ff64ff7000636199f720c9bc`의 `tools/SkillTool/SkillTool.ts`, `screens/REPL.tsx`, `utils/forkedAgent.ts`, `utils/permissions/permissionSetup.ts`, `permissionRuleParser.ts`, `tools/BashTool/bashPermissions.ts`이다. 참조 TypeScript나 프롬프트를 SDK에 복사하지 않았다. 파일 기반 관리/사용자/프로젝트 권한 설정 계층은 0.19.0의 [PermissionSettings.md](PermissionSettings.md)에 구현 범위를 기록했다. 추가 디렉터리는 [WorkingDirectories.md](WorkingDirectories.md)에 구현 범위를 기록한다. 외부 관리 공급자, 자동 권한 분류, 참조 전체 인자 의미·별칭, 원격 질문 중개와 OS 샌드박스는 구현이 남아 있다.
