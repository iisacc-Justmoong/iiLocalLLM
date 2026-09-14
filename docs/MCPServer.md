# C++ MCP 서버와 앱 도구 제공

`mcp::ServerSession`은 연결별 JSON-RPC 상태를 제공하고 `mcp::serveStdio`는 POSIX stdin/stdout 전송을 연결한다. `agent::mcpServerOptions`는 앱이 등록한 ToolRegistry를 기존 스키마·권한·훅을 유지하면서 MCP 도구로 공개한다. 배포 실행 파일은 `iillm-mcp`다. 생산 경로는 C++·Qt이며 공식 Python MCP SDK는 독립 교차 검증에만 사용한다.

stdio·C++ 내장 서버와 0.8.0의 인증된 [Streamable HTTP 서버](MCPHTTPServer.md)를 제공한다. 전체 MCP 요구사항 중 legacy SSE·OAuth, tasks, logging/completion 전용 API, 앱 자동 발견과 실제 Society/Dreamscapes 제품 연결은 남아 있다. Windows에서도 ServerSession을 내장할 수 있지만 이번 stdio 어댑터는 POSIX 전용이며 Windows 전송·실기기는 아직 검증하지 않았다.

## 실행 파일

```sh
build/iillm-mcp --workspace /absolute/project
build/iillm-mcp --workspace /absolute/project --allow Write --allow Edit
build/iillm-mcp --workspace /absolute/project --allow Bash
```

기본으로 Read·Glob·Grep을 허용한다. Write·Edit·Bash 등은 실행 파일의 `--allow` 설정으로 공개 실행 권한을 부여한다. MCP 클라이언트의 요청 인자로 이 설정을 바꿀 수 없다. 파일 도구의 기존 읽기 이력·변경 감지·작업 폴더 제한을 적용하며 Bash는 OS 샌드박스가 아니다. stdout에는 JSON-RPC만 기록하고 진단은 stderr로 보낸다. 모델 옵션을 생략하면 추론 모델을 로드하지 않고 파일·작업 상태·셸 도구를 제공한다.

설치된 로컬 모델을 사용하는 에이전트를 함께 제공하려면 다음과 같이 시작한다.

```sh
build/iillm-mcp --workspace /absolute/project \
  --models /absolute/Models --model model://qwen2.5-0.5b \
  --sessions /absolute/sessions --context 4096 --max-tokens 512
```

모델 URI는 실제 설치 카탈로그의 ID를 사용한다. `--model`은 iiLocalLLM.agent.run 실행을 활성화하며 내부 에이전트의 파일·셸 도구는 같은 허용 규칙을 따른다. 모델은 첫 추론 때 기존 Service의 상주 정책으로 로드된다. 이 실행 파일은 Service를 소유하는 독립 프로세스이므로 이미 다른 daemon이 소유한 동일 모델 카탈로그의 잠금을 공유하거나 탈취하지 않는다. 여러 연결에서 하나의 모델 서비스를 공유하려면 아래 C++ 내장 방식으로 하나의 Engine을 사용한다. 기존 iillm CLI의 Core/Network 전용 링크 경계는 유지한다.

0.18.0의 `--model-options FILE`은 `ModelLoadRequest.options`에 전달할 JSON 객체를 받는다. `--model`·`--models`와 함께 사용하며 작업 폴더 밖의 일반 파일, 최대 64 KiB로 제한한다. POSIX에서는 현재 사용자 소유이며 그룹·다른 사용자 접근 권한이 없어야 한다(예: `0600`). 파일 자체가 심볼릭 링크이면 거부한다. 파일을 한 번 읽어 고정하고, 지정한 모델을 실제로 로드한 뒤 stdio 요청을 처리하거나 HTTP endpoint를 공개한다. 로딩에 실패하면 시작도 실패한다. 이 옵션을 생략하면 기존 첫 추론 시 로딩 동작을 유지한다. 원격 MCP 요청은 모델 옵션을 바꿀 수 없다.

예를 들어 Qwen3에서 `{"enable_thinking": false, "tool_grammar": false}`를 저장한 비공개 파일을 지정하면 API의 같은 모델 로딩 설정을 재현할 수 있다. 지원 키와 값의 의미는 선택한 추론 백엔드의 계약을 따른다. 이 설정은 모델 출력의 정확성이나 한 번만 쓰기를 보장하지 않으므로 실제 도구 호출·반환값·파일 바이트를 함께 검증해야 한다.

`--sessions` 기본값은 작업 폴더의 `.iilocal-llm/sessions`, `--artifacts`는 `.iilocal-llm/artifacts`다. `--request-timeout`은 밀리초 단위이며 기본 60초다. `--temperature`, `--context`, `--max-tokens`는 호스트 시작 설정이다. MCP 요청의 파라미터는 아래 도구 계약에 한정한다.

| 도구 | 입력 | 동작 |
|---|---|---|
| iiLocalLLM.agent.run | prompt, 선택 new_session·max_turns·context_paths | 해당 MCP 연결의 로컬 대화에서 에이전트 실행. 상태·텍스트·턴·사용량·실행/세션 ID를 structuredContent로 반환 |
| iiLocalLLM.agent.session | 선택 include_messages | 해당 연결의 대화 ID·모델·메시지 수, 선택 transcript 반환. 다른 세션 ID를 입력받지 않음 |

에이전트 이벤트는 요청의 progressToken이 있을 때 증가하는 progress와 `_meta["iisacc/agentEvent"]`로 전달한다. 클라이언트 취소는 MCP 요청 → Engine RunHandle → 실제 추론·도구로 전파한다. 완료·실패 후 도구 결과는 기존 JSONL 복구 계약을 따른다. 연결 종료 시 연결과 대화 사이의 메모리 매핑을 제거하고 영속 transcript는 보존한다. 다른 연결의 기존 대화를 자동으로 재개하지 않는다. 인증된 재개·fork API는 아직 남아 있다.

## 앱에 내장

```cpp
#include <agent/McpServer.h>

auto registry = std::make_shared<iiLocalLLM::agent::ToolRegistry>();
// 앱이 구현한 Tool을 registry->add(...)로 등록한다.
auto policy = std::make_shared<iiLocalLLM::agent::RulePolicy>();
iiLocalLLM::agent::McpServerOptions bridge;
bridge.workingDirectory = workspace;
bridge.appId = "com.iisacc.example";
auto options = iiLocalLLM::agent::mcpServerOptions(registry, policy, bridge);
iiLocalLLM::mcp::serveStdio(options);
```

QCoreApplication을 만든 프로세스에서 호출한다. 선택적으로 bridge.engine·model·generation을 지정하면 로컬 에이전트 도구도 공개한다. Engine의 내부 ToolRegistry와 외부 공개 도구 집합은 분리되어 iiLocalLLM.agent.run의 자기 재귀를 만들지 않는다. ServiceModel이 참조하는 Service는 Engine과 서버보다 오래 살아 있어야 한다. 실제 앱이 제공한 도구만 등록하며 appId는 원격 MCP 도구 정의의 `_meta["iisacc/appId"]`에 들어간다. 이 태그는 인증을 대신하지 않는다.

일반 앱 전송은 연결마다 ServerSession을 하나 만든다. 개별 JSON 객체는 receive, 구형 배열은 receiveBatch로 전달하고 takeMessages에서 객체 또는 배열 응답을 가져간다. 전송은 이 JSON 값을 직렬화한다. 메서드 호출은 스레드 안전하지만 생성·close·파괴는 UI 스레드 밖에서 수행한다. 핸들러 및 onClosed 콜백 안에서 해당 세션을 종료하거나 파괴하지 않는다.

하위 mcp 계층은 agent를 참조하지 않는다. 앱 도구 어댑터가 상위에서 두 계층을 연결한다. 실행마다 정의·검증기·핸들러를 함께 스냅샷으로 가져온다. 같은 bridge를 사용하는 연결들은 동시 실행이 안전한 도구를 함께 실행하고 나머지는 배타적으로 실행한다. ToolRunner의 입력별 동시 실행 분류와 입력을 바꾸는 훅도 적용한다. 앱이 별도 경로에서 같은 자원을 수정한다면 해당 앱의 도구 구현에서도 동시 변경 계약을 지켜야 한다.

## 저수준 서버 계약

ServerOptions.lists에 tools/list·resources/list·resources/templates/list·prompts/list의 전체 목록 콜백을 지정한다. handlers에는 tools/call·resources/read·prompts/get 및 명시적인 추가 요청 핸들러를 지정한다. 지원 기능은 실제 등록된 콜백에서 계산하며 초기화 결과에 광고한다. 도구 목록/호출, 프롬프트 목록/조회 쌍이 불완전하면 생성 시 실패한다. 앱 도구 스키마·권한 검증은 agent 어댑터가 수행한다. 저수준 handlers를 직접 사용하는 호스트는 자신의 도메인 입력·출력 및 접근 권한을 검증해야 한다.

목록은 전체 결과의 이름 중복·항목·용량을 검증한 뒤 페이지로 나눈다. 페이지 커서는 해당 연결과 메서드에 묶인 일회용 값이며 다음 페이지가 소비될 때 교체된다. 원래 목록이 바뀌어도 진행 중인 페이지의 내용은 유지한다. 커서는 기본 60초 후 만료한다. 목록을 바꾼 호스트는 notify로 list_changed를 보낸다.

resources/subscribe 핸들러는 호스트가 해당 URI의 구독을 허용하는지 검증한다. 성공한 구독은 연결별로 기록하고 resources/updated 알림을 해당 구독이 있을 때만 전달한다. unsubscribe는 구독을 해제하며 선택 핸들러를 통해 호스트에 반영할 수 있다. 업데이트 알림 자체가 자료를 읽거나 실행하지 않는다.

핸들러는 ServerRequestContext의 연결 ID·요청 ID·클라이언트 정보·기능·프로토콜 버전·취소 토큰을 받는다. requestClient는 살아 있는 부모 요청에 묶여 roots/list·sampling/createMessage·elicitation/create·ping을 전송한다. 기능을 협상하지 않았거나 해당 버전에서 불가능한 요청은 거부한다. 응답·취소·시간 초과·종료를 구분하며 자동 재실행하지 않는다. sampling/elicitation의 상세 모델·UI 처리는 호스트 책임이며 전체 적합성 검증과 task 연계는 남아 있다. 수신 알림은 takeNotifications로 호스트에 데이터로 전달한다.

협상 버전은 2025-11-25·2025-06-18·2025-03-26이다. 2025-03-26에서 요구하는 JSON-RPC 배열 수신과 응답 결합을 stdio·HTTP 전송에 적용한다. 큰 결과를 한도 오류로 바꿀 때에도 해당 오류를 원래 배열 안에 유지한다. 이후 버전에서는 배열 프레임을 거부한다. 구형 서버 응답의 structuredContent·resource_link는 텍스트로 전달하고 outputSchema 광고를 생략한다. 최신 2026-07-28 규격은 아직 지원 목록에 없다.

| 한도 | 기본값 |
|---|---:|
| 실행 중 요청 / 대기 요청 | 8 / 32 |
| 역방향 요청 | 8 |
| 프레임 / 출력 대기열 / 목록 스냅샷 전체 용량 | 8 MiB / 16 MiB / 16 MiB |
| 목록 페이지 / 전체 항목 / 활성 커서 | 64 / 10,000 / 8 |
| 알림·구독 / 한 배열의 항목 | 128 / 128 |
| 연결 내 고유 요청 ID | 100,000 |

요청 ID를 재사용하면 연결을 종료한다. 입력·출력·대기열 상한 및 비정상 전송을 무시하고 계속 실행하지 않는다. 클라이언트 취소는 응답을 생략하고, 서버 시간 초과는 오류를 반환한다. 취소된 핸들러가 종료할 때까지 실행 슬롯을 점유하므로 호스트 핸들러는 취소 토큰에 협력해야 한다. 종료는 모든 수락된 핸들러를 취소하고 합류한 다음 onClosed를 호출한다. 이미 완료한 파일 변경을 취소가 되돌리는 것은 아니다.

stdio는 응답이 없는 유휴 상태에도 출력 파이프의 연결 종료를 확인한다. 쓰기 가능 상태를 기다리며 반복 실행하지 않도록 즉시 상태 확인과 입력 대기를 분리하며, SIGPIPE 차단은 실제 write 호출의 스레드·구간에 한정한다.

## 검증

기본 CTest mcp_server는 독립 연결, 초기화, 용량, 취소·역방향 요청, 목록·구독·구형 배열, 스키마·정책, 실행 순서·파일 읽기 상태·대화 격리를 검사한다. 공식 SDK 1.26.0을 설정하면 mcp_server_official이 실제 실행 파일의 파일 읽기·허용/거부·입력 검증·경로 제한과 Bash 및 자식 프로세스 취소를 확인한다. Qwen fixture를 설정하면 mcp_server_inference가 외부 MCP 클라이언트 → C++ 서버 → 실제 로컬 모델 → Read → 최종 답변·transcript·진행 알림을 검증한다. 관측 결과와 설치 소비자 증거는 Verification.md에 별도로 기록한다.

분석 참조는 Exhen/claude-code-2.1.88의 c8cd253554319f32ff64ff7000636199f720c9bc에서 entrypoints/mcp.ts의 도구 공개·stdio 경로다. 독립 C++ 구현의 기준은 공식 [수명 규격](https://modelcontextprotocol.io/specification/2025-11-25/basic/lifecycle), [stdio 전송](https://modelcontextprotocol.io/specification/2025-11-25/basic/transports), [도구 규격](https://modelcontextprotocol.io/specification/2025-11-25/server/tools), [2025-03 배열 규칙](https://modelcontextprotocol.io/specification/2025-03-26/basic), [2025-06 변경 기록](https://modelcontextprotocol.io/specification/2025-06-18/changelog)이다.

프로젝트 지침은 0.5.0부터 내부 Engine에서 매 모델 호출 전에 조합한다. `context_paths`는 최대 128개 파일 경로이며 해당 연결의 세션에만 유지한다. 경로별 규칙을 첫 호출 전에 적용하려면 이 값을 지정한다. `instructions_loaded` 이벤트는 기존 agent progress metadata로 전달한다. 상세 계약은 [ProjectContext.md](ProjectContext.md)에 있다.

## 수동 대화 압축 (0.6.0)

Engine을 설정한 서버는 `iiLocalLLM.agent.compact`를 제공한다. 선택적 `instructions`만 받으며 현재 MCP 연결의 기존 대화를 요약한다. 다른 session_id나 new_session을 받지 않는다. 같은 연결의 `iiLocalLLM.agent.session`에 압축 수와 최근 체크포인트가 나타난다. 실행·취소·진행·정책 경로는 agent.run과 같다. 원본 기록과 자동 압축의 계약은 [Compaction.md](Compaction.md)에 있다.

## 0.9.0 MCP 연결 관리와 도구 검색

호스트가 지정한 MCP 설정 파일의 연결·복구, 대화별 `ToolSearch`, 인증된 `agent.mcp.status` 및 CLI 옵션을 추가했다. 설정과 실행 권한, 수명 및 미지원 범위는 [ToolDiscovery.md](ToolDiscovery.md)를 참조한다.

## 0.10.0 구조화 결과 전달

`structuredContent`가 있을 때 설명 텍스트가 있더라도 실제 JSON을 모델 입력에서 보존한다. 서버 bridge는 JSON text block을 함께 반환하고 클라이언트 어댑터는 상대 서버가 빠뜨린 경우 모델용 텍스트에 추가한다. 이미 같은 JSON이 있으면 공백 형식과 무관하게 중복하지 않는다. 원래 MCP content·구조화 데이터·host용 `_meta`는 각각 보존한다. `_meta`는 모델 텍스트에 추가하지 않는다. 실제 앱 연동과 검증은 [LocalApplications.md](LocalApplications.md)에 기록한다.

## 작업 목록 도구 (0.11.0)

iillm-mcp는 기본 작업·Todo 도구 일곱 개를 추가하며 --no-tasks로 비활성화한다. 모델이 있으면 현재 연결의 Engine 대화와 목록을 공유하고, 모델이 없으면 연결별 TaskStore를 사용한다. 원격 목록 ID 선택은 허용하지 않는다. 직접 C++ 호스트는 McpServerOptions.taskStore 또는 taskToolsEnabled인 Engine을 선택한다. [Tasks.md](Tasks.md)에 연결 수명·권한·스키마·저장 계약을 기록한다.

공식 Python SDK의 stdio 클라이언트는 제한된 환경 변수만 자동 상속한다. 수락 검사는 하위 서버에 독립 앱 등록 경로와 임시 경로를 명시적으로 전달하여 실행 중인 사용자 앱의 도구가 fixture 목록에 섞이지 않게 한다. HTTP 검사도 같은 격리 경로를 사용한다.

## 백그라운드 셸 도구 (0.12.0)

데스크톱 POSIX iillm-mcp는 Bash의 `run_in_background`와 TaskOutput·TaskStop·ShellTaskList를 제공하며 `--no-background`로 비활성화한다. 계획 작업의 `--no-tasks`와 독립적이다. 모델 없이도 실행할 수 있고, Engine이 있으면 파일·셸 도구와 에이전트가 같은 대화 소유권을 사용한다.

TaskOutput 대기 또는 agent.run 중에도 같은 연결에서 TaskStop을 처리할 수 있다. 제어 도구만 실행 잠금 밖에서 처리하며 기존 스키마·권한·소유권 검사는 유지한다. 연결 종료와 `new_session=true`는 이전 대화의 실행을 중단한다. 다른 연결은 작업 ID나 출력 경로를 알아도 접근할 수 없다. 저장소 위치와 수명은 [BackgroundTasks.md](BackgroundTasks.md)에 있다. 이 기능은 일반 tools/call이며 MCP 비동기 tasks 규격은 아직 미완료이다.

## 입력 큐 도구 (0.13.0)

Engine과 모델을 설정하면 `iiLocalLLM.agent.inputs.enqueue/list/remove/run`을 제공한다. 현재 연결의 대화를 사용하며 다른 session_id를 받지 않는다. enqueue는 text 및 선택 kind·priority·context_paths, list는 offset·limit, remove는 input_id, run은 max_turns·context_paths를 받는다. 생성 설정은 기존 MCP 호스트 설정을 따른다. list는 읽기 전용이고 나머지는 호스트 정책이 허용해야 한다. 실행 파일에서는 `--allow 'iiLocalLLM.agent.inputs.*'`로 지정할 수 있다.

진행 중 agent.run이 실행 잠금을 점유해도 enqueue/list/remove를 처리한다. 도구 스키마와 정책은 유지하며 now는 해당 대화의 현재 연산을 협력 취소한다. 전송 작업자 포화는 별도 한도이다. 입력 큐와 transcript는 영속화하지만 새 MCP 연결의 자동 재개·유휴 실행은 제공하지 않는다. 공식 SDK의 `inputs_mcp_stdio`·`inputs_mcp_http`와 내장 서버의 실행 중 요청 검사를 구분한다. 상세 계약은 [InputQueue.md](InputQueue.md)에 있다.

엔진을 연결한 서버는 `iiLocalLLM.agent.skills.list`와 `iiLocalLLM.agent.run`의 `skill`/`skill_arguments`를 지원한다. 호출은 연결별 대화에 인라인으로 저장된다. [스킬 계약](Skills.md)을 참조한다.

0.15.0은 C++ 서브에이전트와 `agent.agents.run/output/stop/list`, 대응 MCP·CLI 경로를 추가한다. 소유권·취소·분기·재개와 설정 계약은 [Subagents.md](Subagents.md)를 따른다.

0.16.0의 `agent.agents.profiles`(MCP: `iiLocalLLM.agent.agents.profiles`, CLI: `agent agents profiles SESSION`)는 프로파일 메타데이터·출처·가려진 정의·오류를 반환한다. C++ 호스트는 `Subagents::attach`로 현재 프로파일을 각 턴에 연결한다. 독립 데몬과 MCP 서버의 `--agent-profiles FILE`·`--no-agent-profiles`, 모델 사용 범위와 훅 계약은 [AgentProfiles.md](AgentProfiles.md)를 따른다.

0.17.0에서는 `context: fork` 스킬을 같은 API·MCP·CLI 호출로 별도 자식에서 실행한다. 직접 호출은 자식 결과를 반환하고 모델의 `Skill` 호출은 후속 부모 턴에 결과를 전달한다. 본문 분리·권한·모델·사용량·큐 입력과 참조 차이는 [Skills.md](Skills.md)의 별도 자식 실행 계약을 따른다.

0.18.0의 스킬 allowed-tools와 인자 권한 규칙은 [Permissions.md](Permissions.md)를 따른다. API·IPC·MCP 입력은 allowed_tools/prompt_metadata 같은 호스트 전용 권한·출처 필드를 받지 않는다. 모델 Skill의 PermissionRequested 이벤트에는 고정된 permission_preview가 있다. 원격 권한 응답 중개는 아직 지원하지 않는다.
