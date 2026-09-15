# 세션 계획 작성과 검토

0.34.0은 C++ `EnterPlanMode`·`ExitPlanMode`, 세션 소유 계획 파일, 검토 결과와 실행 전환을 제공한다. `EngineOptions.planToolsEnabled`는 임베디드 호스트에서 명시적으로 켠다. 데몬과 모델을 설정한 MCP CLI는 기본으로 켜며 `--agent-no-plan-mode`·`--no-plan-mode`로 끈다. 도구 검색 지연 공개는 `planToolsDeferred`로 선택한다. 공개 구조체가 변경되었으므로 ABI 0.34의 헤더와 라이브러리로 소비자를 함께 다시 빌드한다.

구현은 기존 Qt Core의 JSON·파일·원자적 저장·잠금과 C++ 동시성 도구를 사용한다. 새 런타임 라이브러리나 Python 의존성을 추가하지 않는다. Python은 실제 전송 회귀 검사에만 사용한다.

## 상태와 도구

`<sessionsDirectory>/plans/<sessionId>/state.json`에 상태를, 같은 디렉터리의 `plan.md`에 UTF-8 계획을 저장한다. 모델·API 입력은 소유자 외부 경로, 복원 모드, 승인 여부를 지정할 수 없다. API 소유자는 인증 앱별 Engine으로, MCP 소유자는 연결별 실제 Engine 세션으로 결정된다.

| 동작 | 결과 |
|---|---|
| `EnterPlanMode {}` | `planning`으로 전환하고 실제 `plan_file_path`를 반환한다. 이미 planning이면 같은 계획과 revision을 유지한다. |
| `Read`·`Write`·`Edit` | 해당 세션의 정확한 계획 파일에 접근한다. 기존 파일은 먼저 완전히 읽어야 하며 변경 감지·백업 계약을 유지한다. |
| `ExitPlanMode {allowedPrompts?}` | 디스크의 내용·존재 여부·revision·SHA-256을 고정하여 호스트 검토에 전달한다. |
| Allow | 검토한 상태와 파일이 같은지 다시 확인한 뒤 `approved`를 저장한다. 호스트의 수정 내용이 있으면 파일에도 원자적으로 반영한다. |
| Deny·기한 만료·취소 | planning을 유지한다. interrupt는 기존 권한 응답 계약에 따라 실행도 취소한다. |
| 승인 뒤 재진입 | revision을 증가시키고 기존 파일을 다시 검토할 수 있게 한다. |

`ExitPlanMode`는 기본 Allow·bypass 정책에서도 기존 PermissionRequest 경로의 결정을 요구한다. 명시적 Deny와 dontAsk 거부를 우회하지 않는다. 호스트 C++ 콜백, PermissionRequest 훅, 연결된 앱의 응답 채널을 사용할 수 있다. PreToolUse의 단순 allow만으로 검토를 생략하지 않는다. 이것은 SDK가 제공하는 제품 동작이며 이 SDK 개발 작업에 별도 사용자 승인을 요구하는 절차는 아니다.

`allowedPrompts`는 `{tool:"Bash", prompt:"run tests"}` 형태의 설명 목록이다. 최대 32개, 항목당 1,024자이다. 자동 명령 권한으로 변환하지 않는다. 실제 권한 변경은 기존의 신뢰된 `updatedPermissions` 계약과 명시적 호스트 정책에 따른다.

계획이 없는 상태의 Exit는 거부한다. planning에 진입했지만 파일을 아직 만들지 않았다면, 호스트는 `plan:null`을 검토하여 종료할 수 있다. 빈 파일과 존재하지 않는 파일을 구분한다.

## 호스트 검토와 수정

PermissionRequested 이벤트 또는 `PermissionRequests`의 `request.permission_preview._meta.plan`에 검토 대상 스냅샷을 제공한다. 실제 권한 채널 목록에서는 `requests[n].request.permission_preview`를 읽는다. 공개 계획 상태의 주요 필드는 `session_id`, `phase`, `revision`, `plan_file_path`, `plan`, `plan_sha256`, `file_exists`, `approval_current`, `max_plan_bytes`이다.

호스트가 수정하려면 Allow 응답의 `updatedInput.plan`을 사용한다. 기본 `plan` 입력 필드는 이 호스트 응답에만 허용한다. 모델이나 최초 MCP/API 요청이 이 필드를 넣으면 거부한다. C++에서는 `PermissionResponse.updatedArguments`를 사용한다. 파일 경로를 바꾸는 필드는 없다.

```json
{
  "request_id": "the-pending-request-id",
  "decision": {
    "behavior": "allow",
    "updatedInput": {"plan": "Inspect the parser, fix the boundary, and run the regression suite."}
  }
}
```

`ToolContext.approvedToolPreview`에는 원래 준비한 메타데이터를 ToolRunner만 주입한다. 수정 응답으로 재준비할 때도 원래 검토의 revision·해시를 확인하므로, 검토 도중 바뀐 계획을 새 파일로 바꿔치기하여 승인하지 않는다. ToolStarted 관찰자 이후 실행 직전에도 다시 확인한다. 같은 revision에 대한 두 검토가 동시에 진행되면 한 결과만 전환에 성공한다. PostToolUse 이후 오류·차단은 이미 저장된 계획 변경을 되돌리지 않는다. 권한 응답의 accepted는 도구 실행 완료를 뜻하지 않으며, 최종 도구 결과를 확인해야 한다.

## 실행 경계와 수명

planning에서는 읽기 전용 도구, 정확한 소유 계획 파일, 기존 내부 작업 상태·소유 실행 중단, Engine이 다시 권한을 판단하는 대화 제어만 허용한다. 계획 파일의 예외에도 명시적 Write/Edit Deny·Ask가 적용된다. 커스텀 정책이 무조건 Allow를 반환해도 ToolRunner의 계획 경계는 유지한다. 명령 Bash와 다른 앱의 변경 도구는 이 경계에서 거부한다. 기존에 시작한 백그라운드 프로세스나 호스트 훅 자체를 종료하는 OS 샌드박스는 아니다.

매 도구 호출에서 계획 상태를 새로 읽는다. 실제 실행 구간은 소유자별 공유 잠금이며 Enter/Exit는 배타 잠금을 사용한다. 일반 읽기 도구끼리는 병렬 실행할 수 있고, 검토 응답을 기다리는 동안 실행 잠금을 점유하지 않는다. 도구를 준비한 뒤 계획 상태가 바뀌면 실행을 거부한다. MCP의 Engine 위임 래퍼는 내부 Engine 도구가 이 구간을 관리하므로 외부에서 같은 잠금을 중첩하지 않는다. 실행·progress 콜백에서 같은 소유자의 도구를 재진입시키는 호스트 코드는 지원하지 않는다. 다른 PlanMode 인스턴스·프로세스의 임의 파일 작업까지 이 인메모리 실행 잠금이 직렬화하지는 않는다.

상태 갱신은 세션별 QLockFile과 QSaveFile을 사용하며 손상된 JSON·다른 소유자·심볼릭 링크·잘못된 파일 형식을 거부한다. 상태와 본문은 두 파일이므로 단일 트랜잭션은 아니다. 호스트 수정은 파일을 먼저 저장하고 승인을 나중에 저장한다. 그 사이 중단되면 검토 상태를 유지한다. 승인한 파일이 외부에서 바뀌거나 삭제되면 `approval_current:false`가 되며 다음 도구부터 계획 제한을 다시 적용한다. EnterPlanMode로 다시 검토할 수 있다. 완전한 openat/디스크립터 기반 파일 경합 방어는 아직 아니다.

기본 계획 본문은 65,536바이트이며 NUL을 허용하지 않는다. 독립 C++ PlanMode는 최대 262,144바이트까지 설정할 수 있다. 원격 권한 미리보기는 별도로 PermissionRequests의 바이트 상한을 적용한다. 다른 계획, 상태 JSON, 잠금 파일은 Read와 검색에서 제외한다. 승인 후에는 계획 파일을 읽을 수 있고 재진입해야 수정할 수 있다.

계획 디렉터리가 작업 폴더 안에 있어도 자식의 파일 도구를 준비하고 실행할 때 같은 비공개 경계를 유지한다. 일반 하위 에이전트는 부모 계획 디렉터리의 파일을 직접 읽거나 검색하지 못한다. 검증 훅은 검증 대상 세션의 정확한 계획 파일만 읽을 수 있으며, 다른 계획이나 상태 파일의 조회·검색과 소유 계획 수정은 거부한다. 자식에서 계획 전환 도구를 끄는 것만으로 파일 접근이 제한되는 것은 아니다.

승인 후에는 현재 호스트의 일반 권한 정책을 적용한다. 처음 선택한 기본 모드가 plan이었다면 해당 계획 승인 후 default로 복귀한다. 일회 스킬 허용·실행 콜백을 계획 파일에서 복원하지 않는다. 일반적인 acceptEdits·bypass와 실시간 명시적 규칙은 기존 정책이 판단한다.

재시작과 SessionEnd는 같은 계획을 유지한다. fork는 현재 본문을 독립 파일로 복사하며 새 승인을 요구한다. 과거 메시지 경계에 해당하는 옛 계획을 복원하는 기능은 아니며, 기존 SessionStore의 아티팩트 분기 제한은 유지한다. clear는 새 소유자의 빈 계획으로 시작한다. 초기 포크나 권한 상속과 계획 복사가 하나의 파일 트랜잭션인 것은 아니다. 각 모델 턴에 현재 계획 상태를 다시 주입하므로 압축 후에도 최신 계획을 볼 수 있다. 부모가 계획 제한 상태에서 시작한 자식은 그 제한을 유지하며 Enter/Exit 도구를 제공하지 않는다. 팀 리더 승인이나 자식의 독립 계획 전환은 이번 구현 범위가 아니다.

## C++·API·MCP

```cpp
agent::EngineOptions options;
options.sessionsDirectory = hostPrivateSessions;
options.planToolsEnabled = true;
options.permissionResponse = reviewWithHost;
// Engine(model, registry, policy, options)
// engine.runPlanTool(sessionId, "EnterPlanMode")
// engine.planStatus(sessionId)
```

| 전송 | 인터페이스 |
|---|---|
| 인증 API·IPC | `agent.plan.enter {session_id}`, `agent.plan.exit {session_id,allowedPrompts?}`, `agent.plan.get {session_id}` |
| CLI | `iillm ... rpc agent.plan.get parameters.json`; 변경도 동일한 generic RPC 형식 |
| MCP 모델 도구 | `EnterPlanMode`, `ExitPlanMode`, `iiLocalLLM.agent.plan.get`; 소유 session_id를 입력받지 않음 |
| MCP 제어 | `iisacc/plan/status`; 별도 제어 스트림 용량을 사용하며 도구 목록에는 없음 |
| 기능 인식 | API `plan_tools_enabled`, MCP experimental `iisacc/planMode` (`iisacc.plan/1`) |
| 검토 응답 | API `agent.permissions.pending/respond`, MCP `iisacc/permissions/pending/respond` |

계획 상태 조회는 진행 중인 모델·검토 요청의 transcript 잠금을 기다리지 않는다. API에는 별도 제어 응답 용량과 입력 제어 작업 풀이 있으며 MCP raw status도 예약 제어 처리기를 사용한다. 0.34는 일반/제어/초과 요청을 처리할 스레드를 처음부터 생성한다. 동적으로 늘어나는 cpp-httplib 풀이 아직 idle로 집계된 마지막 스레드에 긴 작업과 제어 요청을 연달아 넣으면, 제어 요청이 긴 작업 뒤에 남는 경합을 실제 검사에서 재현하여 수정했다. 최대 응답·큐·스트림 용량 자체는 기존 값이다.

## 참조와 검증 범위

고정 미러 커밋 `c8cd253554319f32ff64ff7000636199f720c9bc`의 `source/src/tools/EnterPlanModeTool/EnterPlanModeTool.ts`, `tools/ExitPlanModeTool/ExitPlanModeV2Tool.ts`, `utils/plans.ts`를 직접 조사했다. Enter의 빈 입력·일반 에이전트 제한, Exit의 파일 기반 검토·호스트 수정·이전 모드 복원, plans의 resume/fork 파일 분리를 대응 근거로 삼았다. 미러의 유출 출처 자체를 독립적으로 인증한 것은 아니다. TypeScript 구현이나 프롬프트를 복사하지 않고 C++로 작성했다.

이 버전은 전체 참조와 동일하지 않다. UI 인터뷰·AskUserQuestion, 승인 화면의 컨텍스트 초기화/모드 선택, 자동 분류 기반 semantic allowedPrompts, auto 모드 복귀, 팀 리더 mailbox 승인, 원격 파일 스냅샷 복구, 계획 실행 검증 훅, 실제 Society/Dreamscapes 승인 UI는 남아 있다. 전체 하네스 대응표는 partial을 유지한다.

`tests/plan_mode_tests.cpp`는 권한·파일·변경 충돌·취소·병렬 검토·검색·영속·분기·초기화와 모델 상태 주입을 검증한다. `tests/plan_mode_wire.py`는 실제 데몬 HTTP, CLI IPC, 인증 MCP HTTP의 소유권과 검토 응답을 검증한다. `tests/plan_mode_runtime_smoke.cpp`는 호스트가 단계별 도구를 선택하는 유도된 실제 Qwen3 8B 검사이다. 모델이 Enter·Write·Exit의 실제 호출을 생성하고, 최초 입력에 없는 호스트 수정 코드를 최종 답변에 사용해야 통과한다. 모델 대역 검사와 실제 추론을 구분하며 일반 모델의 자율 계획 품질을 보장하지 않는다. 최종 수치와 설치 소비자 증거는 [Verification.md](Verification.md)에 기록한다.

0.35는 C++ AskUserQuestion과 API·MCP 질문/답변을 추가한다. 호스트 응답, 변경 검증, 미리보기와 남은 앱 화면 범위는 [UserQuestions.md](UserQuestions.md)를 따른다.
