# 권한 요청 훅과 호스트 응답

0.25.0은 최종 정책이 Ask인 도구 호출에 `HookKind::PermissionRequest`를 연결한다. 명령 훅, C++ 구조화 응답, 기존 bool 콜백 순으로 판단하며 API·native IPC·MCP·자식 Engine의 도구 실행기가 이를 공유한다. 헤더의 PermissionDecision·HookResult·EngineOptions가 바뀌므로 ABI는 **0.25**이다. 헤더와 라이브러리, 이를 사용하는 C++ 앱을 함께 갱신한다. 0.26의 기본 정책 갱신과 현재 ABI는 [PermissionUpdates.md](PermissionUpdates.md)를 따른다.

새 생산 의존성 없이 기존 Qt JSON, 명령 프로세스 실행기와 C++ 콜백을 재사용했다. 외부 권한 프레임워크보다 현재 정책·준비된 도구의 계약에 직접 연결하는 편이 변경 범위와 배포 의존성을 줄인다. Python은 검증 클라이언트에만 사용한다.

## 실제 실행 순서

1. 원래 입력 검증, PreToolUse, 변경 입력 재검증과 `Tool::prepare`를 수행한다.
2. 호스트 정책으로 판정한다. Deny·Plan/DontAsk의 거부에는 권한 요청 훅을 호출하지 않는다. Allow도 요청 단계를 건너뛴다. 명시적 Ask는 PreToolUse allow로 사라지지 않지만 다음 요청 단계에서 승인할 수 있다.
3. Ask이면 기존 `permission_requested` 이벤트를 내고 PermissionRequest 훅을 실행한다. 이 이벤트는 요청 단계 진입을 뜻하며, 원격 클라이언트의 응답을 기다린다는 뜻은 아니다.
4. 훅 결정이 없으면 `permissionResponse` C++ 콜백, 이어 기존 `permission` bool 콜백을 사용한다. 처리기가 없으면 거부한다. 콜백 목록에서는 첫 결정이 선택된다.
5. 허용 응답의 updatedInput을 대입하고 도구 스키마를 검사한다. 잘못된 스키마이면 권한을 갱신하지 않는다.
6. updatedPermissions가 있으면 검증 후 호스트 `permissionUpdates`, 없으면 정책 `applyUpdates`로 전달한다. 완료 뒤 작업 디렉터리·입력 도메인·준비 대상·정책을 다시 읽는다. 변경 입력만 있어도 다시 준비하고 최종 호스트 Deny를 확인한다. 정책의 Deny가 남거나 작업 경계 밖이면 실행하지 않는다. 입력·권한 변경이 없으면 처음 준비한 스냅샷을 유지한다.

일반 거부는 오류 ToolResult로 다음 모델 턴에 전달한다. deny의 interrupt:true는 같은 취소 토큰을 취소하고 Cancelled를 전파한다. 모델 실행은 cancelled로 끝나며, 직접 MCP 도구 호출은 기존 JSON-RPC cancelled 오류로 끝난다. `continue:false`도 기존 중단 계약을 유지한다. 취소·시간 제한은 명령/콜백의 기존 협조적 취소 경계를 따른다.

입력 변경 뒤에는 prepare가 다시 호출될 수 있다. prepare는 외부 실행 부작용이 없어야 한다. callback이 정책을 비동기로 바꾸는 경우 전체 실행을 하나의 정책 트랜잭션으로 잠그지 않으므로 호스트가 동기화해야 한다. 허용 요청과 파일 I/O 전체에 걸친 OS 샌드박스나 트랜잭션은 아니다.

## 명령 입력과 출력

```json
{"hooks":{"PermissionRequest":[{"matcher":"Write|Edit","hooks":[{
  "type":"command","command":"/absolute/private/approve.sh","timeout":10
}]}]}}
```

기존 호스트 전용 `--agent-hooks`/`--hooks` 파일에서 설정한다. 매처는 도구 이름이며 if 조건도 기존 인자 규칙으로 검사한다. 공통 식별자·cwd·permission_mode·transcript_path에 tool_name·tool_input·tool_use_id를 더한다. 호스트 정책의 `PermissionDecision::suggestions`가 있으면 permission_suggestions에 전달한다. SDK 확장 permission_reason과 준비 도구의 permission_preview도 제공한다. 기본 RulePolicy/SettingsPermissionPolicy는 자동 제안 생성기를 구현하지 않으며 빈 suggestions를 반환한다.

```json
{"hookSpecificOutput":{"hookEventName":"PermissionRequest","decision":{
  "behavior":"allow",
  "updatedInput":{"path":"approved.txt","content":"Approved content"}
}}}
```

```json
{"hookSpecificOutput":{"hookEventName":"PermissionRequest","decision":{
  "behavior":"deny","message":"Rejected by host policy","interrupt":true
}}}
```

allow는 updatedInput·updatedPermissions, deny는 message·interrupt만 받는다. ask/passthrough 응답, 잘못된 타입·필드 조합·알 수 없는 필드는 거부한다. 명령 결과 스키마 오류는 비차단 진단이고 다음 C++ 처리기로 넘어간다. 처리기가 없으면 자동 허용하지 않는다.

PermissionRequest에서는 **정상 종료 0의 JSON만 승인 응답으로 해석**한다. 종료 2는 stdout에 allow가 있더라도 stderr를 이유로 거부한다. 다른 비정상 종료는 비차단 오류이며 stdout으로 승인하지 않는다. 일반 stdout만 있는 성공 명령은 결정 없이 다음 처리기로 넘어간다. 이 종료 코드 우선순위는 기존 PreToolUse의 JSON 우선 처리와 구분한다.

한 명령 설정에 일치하는 명령들은 같은 입력으로 병렬 실행한다. 첫 번째로 완료한 결정의 behavior·입력·갱신 목록을 하나로 보존한다. 뒤에 끝난 명령의 거부나 다른 입력과 섞지 않는다. 모든 시작한 명령의 종료/진단은 수집하므로 첫 응답 즉시 반환하거나 나머지 명령을 자동 취소하는 구현은 아니다. 별도 C++ 훅 목록은 등록 순서로 호출하고 결정이 있으면 후속 PermissionRequest 훅을 호출하지 않는다. 일반 이벤트의 기존 deny 우선 병합은 유지한다.

## C++ 호스트 처리

```cpp
using namespace iiLocalLLM::agent;
EngineOptions options;
options.permissionResponse = [](const ToolCall& call,
    const PermissionDecision& request, const ToolContext& context) {
    PermissionResponse result;
    // 호스트가 실제 검사를 마친 경우에만 허용한다.
    result.behavior = PermissionBehavior::Allow;
    return result;
};
```

같은 필드는 ToolRunnerOptions에도 있고, ApiOptions.engine과 McpServerOptions.tools에 설정할 수 있다. McpServerOptions.engine에 전달할 Engine에도 생성 전에 EngineOptions로 설정한다. legacy bool 콜백은 수정 없이 유지된다. 구조화 콜백이 있는 호출은 입력 변경 가능성이 있으므로 도구의 병렬 안전 표시와 무관하게 직렬 분류한다. 요청 콜백은 실행 worker에서 호출되므로 UI 호스트는 자신의 이벤트 루프에 중개하고 취소 토큰을 확인해야 한다. 같은 실행의 종료를 콜백 안에서 기다리지 않는다.

`permissionUpdates`는 `void(const QJsonArray&, const ToolContext&)`이다. 갱신을 전부 반영한 뒤 반환하거나 오류를 던져야 한다. 적용 중 외부 저장소에 이미 남긴 일부 효과를 ToolRunner가 되돌리지는 않는다. 0.26부터 명시적 콜백이 없으면 정책의 applyUpdates로 전달한다. SettingsPermissionPolicy는 기본 갱신을 구현하고, 고정 RulePolicy 등 미지원 정책은 RuntimeUnavailable ToolResult를 반환한다. CLI의 파일 출처·세션 격리·저장 한계는 [PermissionUpdates.md](PermissionUpdates.md)에 기록한다.

검증하는 PermissionUpdate 형태는 addRules/replaceRules/removeRules(rules, behavior), setMode(mode), addDirectories/removeDirectories(directories)이며 모두 destination이 필요하다. destination은 userSettings/projectSettings/localSettings/session/cliArg이다. rule은 toolName과 선택 ruleContent이다. 모드는 default/acceptEdits/bypassPermissions/plan/dontAsk이다. 목록은 256개·64 KiB, 업데이트당 규칙 256개·디렉터리 128개, 문자열은 경로/내용 4096자·도구 이름 512자이다. C++ 응답에도 같은 형태·상한 검사를 적용한다. 형식 검증이 해당 목적지의 자동 저장이나 managed 정책 수정 권한을 뜻하지 않는다.

## 검증과 남은 범위

permission_request_tests는 Ask 전용 호출, 원본과 변경 스냅샷 구분, 변경 입력의 스키마/거부 재검사, 일반 거부와 취소, legacy fallback, 실제 호스트 갱신 이후 다음 호출의 정책 변화, 실패 시 실행 방지 및 인증 API 경로를 검사한다. command_hooks_tests는 실제 프로세스·stdin·응답 파싱·종료 2·첫 완료 결정의 묶음 보존을 검사한다. command_hooks_wire.py는 API 작업 게시와 IPC CLI, MCP HTTP/공식 stdio, 실제 모델의 입력 변경과 interrupt를 확인한다. 최종 실행 결과는 [Verification.md](Verification.md)에 기록한다.

참조는 미러 커밋 c8cd253554319f32ff64ff7000636199f720c9bc의 entrypoints/sdk/coreSchemas.ts:425/875, utils/hooks.ts:4157, hooks/toolPermission/PermissionContext.ts:217, cli/structuredIO.ts:787, utils/permissions/permissions.ts:398이다. 구조와 호출 순서를 관찰하여 C++로 작성했으며 참조 TypeScript를 이식하지 않았다. SDK는 입력 수정 후 호스트 Deny를 다시 검사하고 명령들이 끝날 때까지 기다린다는 자체 경계를 둔다.

기본 계층형 설정의 지속 갱신은 0.26에서 구현했다. 권한 제안 생성, 원격 클라이언트가 request_id로 답하는 중개 API, SDK 응답과 훅의 경합·취소, 자동 분류기의 PermissionDenied 재시도는 남아 있다. 모든 일반 거부를 PermissionDenied 훅으로 대체하지 않는다. 실제 Society/Dreamscapes 앱 패키징·플랫폼 검증 및 전체 하네스 목표는 partial이다.
