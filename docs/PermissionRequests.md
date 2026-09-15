# 앱의 권한 요청과 응답

0.27은 Ask 판정을 앱에 전달하고 앱 응답과 로컬 PermissionRequest 훅 중 먼저 확정된 결정을 적용한다. 생산 구현은 Qt JSON과 C++ mutex/condition_variable/future를 재사용한다. 새 외부 의존성은 없다. Python은 전송·공식 MCP SDK·실제 모델 검증용이다. 전체 하네스 상태는 [partial](HarnessParity.md)이다.

## 활성화

ApiOptions.permissionRequests 또는 McpServerOptions.permissionRequests에 PermissionRequestsOptions를 지정한다. API는 인증된 클라이언트마다, MCP는 연결마다 채널을 만든다. EngineOptions/ToolRunnerOptions의 공유 채널을 이 호스트에 전달하면 시작을 거부한다. 독립 C++ Engine/ToolRunner는 자체 채널을 사용할 수 있다. RunRequest/ToolContext의 포인터는 신뢰된 C++ 호스트 전용이며 원격 JSON·모델 입력·트랜스크립트에서 복원하지 않는다.

CLI는 workspace 밖의 소유자 전용 JSON으로 활성화한다. `{}`는 기본 한도를 사용한다. 잘못된 타입·범위·미지 필드·공개 권한·workspace 내부 파일은 모델 초기화 전에 거부한다. 이 파일은 도구 접근에서도 보호한다.

```sh
iiLocalLLMD ... --agent-permission-requests /private/requests.json
iillm-mcp ... --permission-requests /private/requests.json
```

```json
{"timeout_ms":120000,"max_pending":64,"max_history":512,"max_request_bytes":1048576,"max_pending_bytes":8388608}
```

| 설정 | 기본값 | 범위 |
|---|---:|---:|
| timeout_ms | 120000 | 1–3600000 |
| max_pending | 64 | 1–1024 |
| max_history | 512 | 1–65536 |
| max_request_bytes | 1048576 | 1024–4194304 |
| max_pending_bytes | 8388608 | max_request_bytes 이상, 67108864 이하 |

활성화한 CLI의 기본 모드는 default이며 명시한 호스트 모드·설정 모드가 우선한다. 비활성 CLI는 기존 dontAsk 기본값을 유지한다. 활성화 자체가 정책 deny를 해제하지 않는다. 외부 API/MCP/클라이언트 기한은 승인 대기와 모델 실행에 충분하게 설정해야 한다. 짧은 외부 기한이 먼저 오면 취소된다. API는 단일 프롬프트가 maxResultBytes−256 안에 들어야 시작된다. MCP 브리지는 최대 5 MiB 페이지를 기본 8 MiB 프레임에 담는다. 프레임을 줄인 임베디드 호스트는 요청 크기·페이지 건수도 함께 제한해야 한다.

## 앱 인터페이스

| 전송 | 대기 조회 | 응답 |
|---|---|---|
| 인증 HTTP /v1/rpc, native IPC | agent.permissions.pending | agent.permissions.respond |
| MCP JSON-RPC | iisacc/permissions/pending | iisacc/permissions/respond |
| iillm --auth-file TOKEN | agent permissions pending [FILE] | agent permissions respond FILE |

목록 인자는 `{"after":0,"limit":32}`이며 생략 가능하다. after는 0–2^53−1 정수, limit는 1–128이다. 결과 requests는 현재 대기 중인 항목이다. 다음 페이지가 있으면 next_cursor를 준다. 페이지 순회 후 새 조회는 after=0부터 시작한다. cursor는 영속 이벤트 구독 위치가 아니다. pending_count는 전체 대기 수이고 closed는 채널 종료 여부이다.

```json
{"request_id":"prompt-id","decision":{"behavior":"allow","updatedInput":{"path":"approved.txt","content":"APPROVED"},"decisionClassification":"user_temporary"}}
```

```json
{"request_id":"prompt-id","decision":{"behavior":"deny","message":"User declined","interrupt":true}}
```

대기 항목은 schema=iisacc.permission-request/1, request_id, sequence, status, session_id, run_id, cwd, created_at, expires_at와 request 객체를 가진다. request에는 subtype=can_use_tool, tool_name, tool_use_id, input, permission_suggestions, decision_reason, 선택적 permission_preview가 있다. 권한 요청 ID와 바깥 RPC 호출 ID는 다르다. session_id는 하위 에이전트의 세션일 수도 있다. 응답에서 소유자나 session_id를 선택할 수 없다.

허용 응답의 updatedInput/updatedPermissions는 선택적이다. 누락 또는 빈 updatedInput 객체는 원래 입력을 유지한다. 이는 모바일 응답 처리이며 기존 C++ 훅의 명시적 빈 객체 변경 계약과 다르다. 거부 응답의 message/interrupt도 선택적이다. interrupt=true는 실행 취소 토큰에 전달한다. 선택적 toolUseID는 요청의 호출 ID와 같아야 한다. decisionClassification은 user_temporary/user_permanent/user_reject 중 하나인 기록용 값이며 자체로 규칙을 저장하지 않는다. 저장은 [updatedPermissions](PermissionUpdates.md)의 명시적 연산으로 요청한다.

응답 결과는 request_id, session_id, run_id, tool_use_id, status, source, behavior, resolved_at, accepted, replayed를 제공한다. accepted=true는 결정 접수이며 도구·저장 성공 증거가 아니다. 앱은 실행 결과를 별도로 확인해야 한다. 동일 JSON 결정 재전송은 이력이 남아 있는 동안 accepted=true/replayed=true이다. 다른 결정은 AlreadyExists, 훅·취소·기한·종료가 먼저 확정되면 accepted=false이다. 다른 소유자 또는 제거된 이력의 ID는 NotFound이다. 같은 API 자격 증명의 두 창은 같은 소유자이다. MCP는 같은 토큰의 새 연결도 별도 채널이다.

API 제어는 실행/입력 작업 풀이나 Engine 세션 잠금에 진입하지 않는다. MCP는 ServerOptions.controlHandlers의 별도 작업 풀(기본 동시 2, 대기 16)을 사용한다. 도구 큐가 승인을 기다려도 응답을 처리할 수 있다. 0.28은 HTTP 응답과 MCP 활성·보관 SSE 스트림에도 별도의 제어 용량을 둔다. 일반 응답이 한도에 도달해도 승인 조회·응답을 처리하며 제어 용량 자체는 제한된다. 연결 큐·읽기/쓰기 기한 등 전체 조건은 [ControlCapacity.md](ControlCapacity.md)를 따른다. 제어 메서드는 tools/list와 모델 ToolRegistry에 노출하지 않는다. 이미 허용된 임의 호스트 코드의 자격 증명 접근까지 막는 OS 샌드박스는 아니다.

agent.info.permission_requests_enabled와 MCP 초기화의 capabilities.experimental["iisacc/permissionRequests"]에서 지원 여부를 확인한다. MCP 값에는 schema/pendingMethod/respondMethod가 있다. iisacc 확장이며 MCP 표준 권한 기능이나 Claude SDK control_request 프로토콜과 동일하다고 주장하지 않는다. 기본 JSON-RPC·메타데이터 규칙은 [MCP 2025-11-25](https://modelcontextprotocol.io/specification/2025-11-25/basic/index)를 따른다.

## 실행과 수명

ToolRunner는 PreToolUse·스키마·작업 경계·준비·정책을 먼저 평가하고 Ask일 때만 등록한다. permission_requested 이벤트를 보내고 로컬 훅과 앱 응답을 동시에 기다린다. 훅이 답하지 않으면 구조화 콜백, 기존 bool 콜백 순으로 진행한다. 모든 로컬 처리기에 결정이 없으면 원격 응답을 기한까지 기다린다.

먼저 확정된 전체 응답만 적용한다. 앱이 이기면 로컬 토큰을 취소하고 콜백을 회수한다. 진 쪽의 updatedInput/updatedPermissions/추가 피드백은 적용하지 않는다. 명령 훅은 프로세스 취소·회수를 따른다. 취소에 협조하지 않는 임의 C++ 콜백은 강제 중단할 수 없어 반환이 지연될 수 있다. 콜백 자체가 수행한 임의 외부 부작용은 되돌리지 않는다.

permission_resolved 이벤트는 출처와 상태를 제공한다. MCP 진행 알림의 _meta["iisacc/agentEvent"]로 전달하며 목록 조회도 가능하다. 중첩 실행·도구·훅은 한 MCP 요청의 단조 증가 step 카운터를 공유한다. 원래 progress/total은 _meta["iisacc/sourceProgress"]에 보존한다. 승인 후 입력 스키마, 정책 갱신, 준비 대상과 최종 deny를 다시 검사한다. 변경된 대상이 거부되거나 설정 저장이 실패하면 도구를 실행하지 않는다. 설정 저장과 도구 작업 전체는 트랜잭션이 아니다.

잘못된 응답은 대기 요청을 소비하지 않는다. 취소·기한·채널 종료는 대기를 제거한다. Ticket/실행 동안 본문과 응답이 남을 수 있지만 완료 이력에는 입력 본문을 보관하지 않는다. 이력은 오래된 항목부터 한도만큼 제거하고 재시작에서 복원하지 않는다. parent/child/clear 실행에도 채널 소유권을 전달한다. MCP 재연결은 이전 연결의 승인 권한을 복원하지 않는다.

## 참조와 검증

정적 비교 대상은 c8cd253554319f32ff64ff7000636199f720c9bc의 source/src/cli/structuredIO.ts(sendRequest/createCanUseTool)와 source/src/utils/permissions/PermissionPromptToolResultSchema.ts이다. 참조의 훅/SDK 경쟁·요청 ID·취소·늦은 응답·빈 모바일 입력을 비교했다. 참조에는 잘못된 updatedPermissions를 무시하는 경로와 훅 결정 반환 전에 권한 갱신을 수행하는 경로가 있다. iiLocalLLM은 응답을 엄격히 검증하고 이긴 응답의 정책만 갱신한다. TypeScript 코드를 실행하거나 이식하지 않았다.

tests/permission_requests_tests.cpp는 실제 파일, 경쟁·중복 응답, 채널 격리, 포화 API/MCP 큐, 하위 에이전트 전달과 기한·취소를 검사한다. tests/permission_requests_wire.py는 별도 daemon/CLI/MCP 프로세스와 선택적 공식 stdio·실제 모델을 검사한다. 관측·설치 증거는 [Verification.md](Verification.md)에 별도로 기록한다. 실제 Society/Dreamscapes 승인 UI 통합·배포, 플랫폼별 검증, 영속 승인 복구, 자동 분류/제안과 나머지 하네스 기능은 이번 범위에 포함되지 않는다.

0.35는 C++ AskUserQuestion과 API·MCP 질문/답변을 추가한다. 호스트 응답, 변경 검증, 미리보기와 남은 앱 화면 범위는 [UserQuestions.md](UserQuestions.md)를 따른다.
