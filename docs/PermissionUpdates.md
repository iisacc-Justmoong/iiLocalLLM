# 승인된 권한의 갱신과 저장

0.26.0은 `SettingsPermissionPolicy`에 승인된 규칙·모드·작업 디렉터리의 갱신을 추가한다. C++ 호스트나 PermissionRequest의 `updatedPermissions`를 통해 적용하며, Engine·API·native IPC·MCP 실행기가 같은 정책을 사용한다. `PermissionPolicy` 가상 함수와 설정 Options가 바뀌므로 ABI는 **0.26**이다. 헤더·라이브러리·C++ 소비자를 함께 다시 빌드한다.

기존 Qt JSON·파일 기능과 POSIX 파일 잠금/rename을 사용한다. 현재 출처 병합·경로 바인딩·관리 정책에 직접 연결해야 하는 기능이라 외부 설정 프레임워크를 도입하지 않았다. 새 생산 의존성은 없으며 Python은 전송·실제 모델 검증에만 사용한다.

## 진입점과 신뢰 경계

```cpp
#include <agent/PermissionSettings.h>
using namespace iiLocalLLM::agent;
PermissionSettingsOptions settings;
settings.workingDirectory = workspace;
settings.enabledSources = {"local"};
auto policy = std::make_shared<SettingsPermissionPolicy>(settings);
QJsonArray updates{QJsonObject{
    {"type", "addRules"}, {"destination", "localSettings"}, {"behavior", "allow"},
    {"rules", QJsonArray{QJsonObject{{"toolName", "Write"}, {"ruleContent", "/output/**"}}}}
}};
policy->applyUpdates(updates, ToolContext{sessionId, {}, workspace});
```

직접 호출은 신뢰된 호스트 작업이다. 모델이나 원격 클라이언트에게 설정 변경 메서드를 내보내지 않는다. PermissionRequest 훅 또는 호스트의 구조화 응답이 allow와 updatedPermissions를 반환하면 ToolRunner가 먼저 입력 스키마를 확인한다. 명시한 `permissionUpdates` C++ 콜백이 있으면 그 콜백만 호출하고, 없으면 정책의 `applyUpdates`를 호출한다. 기본 `RulePolicy`와 갱신을 구현하지 않은 사용자 정책은 RuntimeUnavailable로 실패한다. [PermissionRequest.md](PermissionRequest.md)의 Ask 전용 요청·일반 거부·interrupt 계약을 유지한다.

갱신 뒤 작업 디렉터리를 다시 읽고 입력 도메인·작업 경계·준비 스냅샷·호스트 Deny를 재검사한다. 따라서 한 승인에서 외부 디렉터리를 추가하고 입력을 그 안의 경로로 바꿀 수 있다. 잘못된 입력 스키마는 갱신 전에 실패한다. 뒤의 도메인 검사·호스트 거부·실행이 실패해도 이미 승인하여 저장한 설정을 되돌리지는 않는다. prepare는 여러 번 호출될 수 있으므로 외부 실행 부작용이 없어야 한다.

CLI 호스트는 설정 파일을 지정하지 않아도 SettingsPermissionPolicy를 사용하며 조회의 provider는 `settings`이다. 이 경우 파일 출처는 모두 꺼져 있고 fallback은 dontAsk이다. HOME이나 프로젝트 설정을 자동으로 읽지 않는다. 파일 저장은 호스트가 `--agent-permission-settings` 또는 `--permission-settings`로 해당 출처를 켠 경우에만 가능하다. 직접 생성한 C++ RulePolicy의 provider는 계속 `rules`이다. CLI의 일반 Write/Edit 허용도 설정 파일·잠금 파일 쓰기 보호를 우회하지 않는다.

## 갱신 형태

모든 갱신에는 type과 destination이 필요하다. 배열 안의 연산을 순서대로 적용한다.

| type | 추가 필드 | 동작 |
|---|---|---|
| addRules | behavior, rules | 해당 출처·behavior에 규칙 추가, 정규화한 중복 제거 |
| replaceRules | behavior, rules | 해당 출처의 해당 behavior 목록만 교체 |
| removeRules | behavior, rules | 해당 출처·behavior에서 정규화한 규칙 제거 |
| setMode | mode | 현재 세션 모드 변경; 파일 목적지이면 defaultMode도 저장 |
| addDirectories | directories | 정규화한 절대 경로를 해당 출처에 추가 |
| removeDirectories | directories | 해당 출처에서 삭제하고 현재 세션의 같은 입력 경로를 억제 |

behavior는 allow/deny/ask, rule은 `{toolName, ruleContent?}`이다. 규칙의 괄호·역슬래시를 escape하여 저장하며 빈 내용 또는 `*`인 전체 도구 규칙을 bare 이름으로 정규화한다. 예를 들어 Bash(*)와 Bash는 제거·중복 검사에서 같다. 파일·session의 새 규칙은 출처 기준 설정 문법이다. cliArg의 기존 네이티브 규칙은 원래 문법·경로 메타데이터를 보존하고 새로 추가한 규칙만 설정 문법을 사용한다.

mode는 default/acceptEdits/bypassPermissions/plan/dontAsk이다. 승인된 세션 모드 → 호스트 modeOverride → 병합 defaultMode → fallback 순서로 선택한다. setMode의 현재 세션 변경은 destination과 관계없이 즉시 반영된다. 관리 출처의 bypass 금지는 갱신 전후에 확인한다. managed-only가 켜져 있으면 session·cliArg·일반 파일 규칙도 판단에서 제외한다. 고정 호스트 Deny와 자식 프로파일의 도구 범위·Plan/DontAsk 제한은 유지한다. auto 분류 모드는 아직 지원하지 않는다.

| destination | 저장 위치와 수명 |
|---|---|
| userSettings | 명시한 userDirectory/settings.json; user 출처가 켜져 있어야 함 |
| projectSettings | workspace/.claude/settings.json; project 출처 필요 |
| localSettings | workspace/.claude/settings.local.json; local 출처 필요 |
| session | 같은 정책 객체의 sessionId별 메모리 |
| cliArg | 같은 세션의 CLI 규칙·디렉터리 재정의; argv나 파일을 변경하지 않음 |

flagSettings·policySettings는 갱신 목적지가 아니다. 비활성 출처에 대한 저장은 오류이며 다른 출처로 대신 저장하지 않는다. 파일에 있던 비권한 필드와 수정하지 않은 behavior는 보존한다. JSON의 공백·순서는 재직렬화된다. 영속 파일을 함께 읽는 다른 세션·앱도 다음 조회/판단부터 그 변경을 받는다. 반대로 session/cliArg 갱신은 다른 세션이나 정책 객체·프로세스로 전파되지 않는다. 임베디드 호스트는 신뢰 주체를 분리할 sessionId와 정책 객체를 선택해야 한다.

## 작업 디렉터리와 세션 수명

상대 디렉터리는 원래 workspace 기준이며 `~`는 명시한 homeDirectory가 필요하다. 파일 규칙의 `/pattern` 기준과 구분한다. removeDirectories는 선택한 출처 파일만 수정하지만 현재 세션에서는 다른 출처의 동일한 정규화 입력도 억제한다. 다른 세션의 메모리 억제 목록은 바뀌지 않는다. 이후 같은 경로를 명시적으로 추가하면 억제를 해제한다. 같은 canonical 대상을 가리키는 서로 다른 입력 경로까지 제거하지는 않는다. workspace 자체는 이 억제 목록으로 취소할 수 없다. ToolContext::workingDirectories는 실행용 스냅샷이며 ToolRunner가 준비할 때 정책 결과로 교체한다. 호출자가 목록을 공급해도 제거된 경로를 다시 허용하지 않는다.

파일/기본 CLI와 실행 중 추가한 디렉터리의 canonical 바인딩을 구분한다. session/cliArg 추가는 현재 세션에서 같은 입력의 기본 grant보다 우선하며 다른 세션의 최초 바인딩을 바꾸지 않는다. fork/clear로 복사한 바인딩도 링크 변경만으로 새 대상을 허용하지 않는다. 호스트가 명시적으로 같은 런타임 경로를 다시 추가하면 새 대상을 선택할 수 있다. 파일 바이트가 바뀌었을 때의 출처 전체 재바인딩은 기존 [WorkingDirectories.md](WorkingDirectories.md) 계약을 따른다.

`inheritSession(from,to)`는 메모리 규칙·모드·억제·런타임 경로 바인딩을 값으로 복사한다. 이후 양쪽의 메모리 갱신은 독립적이다. 부모의 런타임 항목이 없으면 대상의 기존 항목도 제거한다. Engine fork와 clear는 새 세션 게시 뒤 이 함수를 호출한다. 자식 에이전트는 접수 시 부모 상태를 받고, 재개 시 현재 부모 상태로 다시 시작한다. SubagentStart에도 상속 후의 모드를 제공한다. 자식 실행이 완료되고 셸 정리가 성공하면 자식의 메모리 항목을 버린다. 파일 설정은 계속 다시 읽으므로 이 상속이 영속 권한의 고정 스냅샷을 뜻하지는 않는다.

일반 세션 종료는 재개를 위해 런타임 항목을 유지한다. 호스트는 더 사용하지 않을 때 `forgetSession(context)`를 호출할 수 있다. 정책 객체가 사라지면 모든 메모리 상태가 사라지며 transcript에 저장하거나 재시작 후 복원하지 않는다. 기본 상한은 1,024세션, 호스트 허용 범위는 1..65,536이다. 가득 차면 오류이며 임의 세션의 권한을 축출하지 않는다. clear/fork가 새 기록을 만든 뒤 상속에 실패하면 기록은 남는다. clear는 permissions 단계 진단을, fork 오류는 생성된 ID를 제공한다. 다중 저장소 전체의 원자적 생성은 아니다. inheritSession 구현은 Engine에 재진입하면 안 된다.

`snapshot()`은 파일/기본 CLI 설정 조회이고 session 메모리를 포함하지 않는다. 세션별 실제 판단은 `describe(context)`, `workingDirectories(context)`, `Engine::permissions(id)`와 인증 API/MCP의 permissions.get으로 조회한다. 억제한 디렉터리는 가능한 경우 removed 상태로 표시한다.

## 저장·동시 실행·실패 경계

전체 갱신의 형태·목적지·규칙·디렉터리를 먼저 검사한다. 대상 파일을 경로 순으로 잠근 후 최신 JSON을 읽고 수정한다. 모든 제안 문서와 최종 병합 정책의 검증을 마치기 전에는 JSON을 쓰지 않는다. 메모리 갱신도 마지막에 게시한다. 실패한 사전 검사로 디렉터리나 잠금 파일은 남을 수 있다.

POSIX는 호스트가 고른 최상위 디렉터리를 열고 그 아래를 openat/O_NOFOLLOW로 순회한다. 설정·잠금의 심볼릭 링크와 비정규 파일을 거부하며 잠금에는 hard link도 허용하지 않는다. 최초 동시 생성 때 이 Mac의 APFS에서 O_CREAT가 ENOENT를 반환하는 경쟁을 재현하여, 기존 파일 열기와 O_EXCL 생성을 분리하고 한도 안에서 재시도한다. 프로세스 간 flock으로 갱신자를 직렬화하고 원본 바이트를 게시 직전에 다시 비교한다. 새 파일은 0600 임시 파일에 쓴 뒤 fsync·같은 디렉터리의 rename으로 교체한다. 열린 디렉터리 fd를 기준으로 쓰므로 부모 경로가 바뀌어도 다른 링크 대상을 따라 쓰지 않는다. 다만 이름이 바뀐 원래 디렉터리에 게시될 수 있고 외부 프로세스의 임의 변경을 막는 OS 샌드박스는 아니다.

잠금 파일은 `.settings.local.json.iillm-permissions.lock` 같은 이름으로 남겨 잠금 inode를 유지한다. 기본 대기는 5초이며 C++ 설정 범위는 1..60,000ms이다. 대기 중 취소를 확인한다. POSIX 외에는 QLockFile/QSaveFile을 사용하며 경로 검사와 실제 쓰기 사이 경쟁에 대해 POSIX fd와 같은 보장을 하지 않는다. 이 단계의 실행 검증은 macOS에서 수행한다.

파일 하나의 교체는 원자적이지만 여러 파일의 배치는 트랜잭션이 아니다. 게시 직전 마지막으로 취소를 확인하고, 게시를 시작하면 취소가 와도 접수된 파일 쓰기를 마친 뒤 실행기가 취소를 처리한다. I/O 오류가 나면 앞서 커밋된 목적지를 오류 메시지에 남기며 그 파일들은 유지된다. rename 뒤 디렉터리 fsync가 실패하면 현재 파일도 이미 게시되었을 수 있다. 디렉터리 fsync를 지원하지 않는 파일 시스템에서는 교체의 원자성과 전원 손실 시 내구성을 구분한다. 같은 파일을 가리키는 별칭 목적지는 동일 경로 오류 또는 잠금 시간 초과로 실패할 수 있다. 협조하지 않는 외부 작성자의 최종 바이트 검사 직후 쓰기까지 완전히 직렬화하지는 않는다.

갱신 목록은 256개/64KiB, 각 rules는 256개, directories는 128개이다. 파일·전체 설정·작업 디렉터리의 기존 상한도 적용한다. 세션의 누적 메모리 규칙·경로는 maxFileBytes 한도와 제거 디렉터리 수 한도 안에 있어야 한다. 저장 배치와 도구 실행 전체를 하나의 트랜잭션으로 취급하지 않는다.

## 검증과 참조

permission_updates_tests는 실제 파일 저장·재개, session/cliArg 격리, 관리 정책, 갱신 순서, 새 작업 디렉터리와 입력 변경, 링크 재바인딩, 동시 작성자, 잠금 시간 제한/취소, 사전 검사 실패와 게시 후 실행 거부를 검사한다. subagent_tests는 접수 이후 부모 변경과 자식 재개 경계를 검사한다. command_hooks_wire.py는 API·IPC·MCP HTTP/공식 stdio, 대화 초기화 상속, 다른 클라이언트 격리, 프로세스 재시작과 선택적 Qwen3 실제 도구 실행을 확인한다. 실행 결과와 소스/설치 증거는 [Verification.md](Verification.md)에 기록한다.

참조는 미러 커밋 c8cd253554319f32ff64ff7000636199f720c9bc의 `source/src/utils/permissions/PermissionUpdate.ts`와 `permissionRuleParser.ts`이다. 여섯 연산, 목적지별 규칙, 현재 모드 반영과 파일 저장의 차이, 디렉터리 삭제, 규칙 정규화를 관찰했다. C++은 공유 정책의 세션 격리·한도·검증 후 게시·파일 잠금을 별도로 둔다. 참조 코드나 TypeScript 실행기를 포함하지 않는다.

자동 권한 제안, 원격 request_id 승인 중개, 자동 분류/PermissionDenied 재시도, 전체 BashSecurity·OS 샌드박스·설정 공급자와 앱/플랫폼 검증은 남아 있다. [HarnessParity.md](HarnessParity.md)의 전체 목표는 partial이다.
