# C++ 에이전트 실행 계층

0.24.0은 Engine::clearSession과 즉시 SessionStart(clear), 백그라운드 작업의 소유권/알림 전환을 제공한다. [SessionClear.md](SessionClear.md)에 실제 초기화 범위와 부분 실패 계약을 기록한다.

0.23.0의 Engine::endSession/close는 활성 세션의 종료 훅, 실행·입력 접수 차단, 진행/대기 작업 취소와 기록 보존을 API·MCP에 연결한다. [SessionEnd.md](SessionEnd.md)에 정확한 수명·시간 예산·남은 호환 범위를 기록한다.

전체 목표와 미완료 영역은 [HarnessParity.md](HarnessParity.md) 및 `catalog/harness-parity.json`에서 추적한다. 이 문서는 현재 추가한 네이티브 실행 계층의 실제 계약을 설명한다. 기존 daemon의 HTTP에는 모델의 함수 도구 호출을 연결했다. C++ stdio MCP 클라이언트와 도구 어댑터는 [MCP.md](MCP.md), 외부에서 앱 도구와 로컬 에이전트를 호출하는 서버는 [MCPServer.md](MCPServer.md)에 설명한다. 인증된 에이전트 실행·세션 관리 IPC·HTTP/SSE는 [AgentAPI.md](AgentAPI.md)에 설명한다. 전체 MCP/하네스 호환 및 실제 앱 연결은 아직 미완료이다.

## 계층

`agent::Engine` → `agent::Model` / `agent::ToolRunner` / `agent::SessionStore`로 나뉜다. ToolRunner는 ToolRegistry와 PermissionPolicy를 사용한다. 기본 도구와 앱 도구는 같은 계약을 사용하며 UI·HTTP·MCP를 참조하지 않는다. `ServiceModel`만 기존 추론 Service를 참조하는 어댑터이다. 하위 추론 런타임은 에이전트 계층을 참조하지 않는다.

공개 헤더는 `agent/Types.h`, `agent/Tools.h`, `agent/SessionStore.h`, `agent/Engine.h`이며 헤더와 구현은 같은 디렉터리에 둔다. 설치 후에도 `<agent/Engine.h>`로 사용한다. JSON Schema의 jsoncons 타입은 외부 ABI에 노출하지 않는다.

## 모델과 도구

Model은 대화·도구 정의·생성 옵션을 받고 ModelReply를 반환한다. 스트림과 취소는 별도 콜백/토큰이다. Model 구현은 서로 다른 실행에서 동시에 호출될 수 있다. 모델이 생성한 도구 ID가 없으면 호스트가 부여하며, 중복·재사용 ID 또는 잘못된 결과 연결은 실행 전에 거부한다.

ServiceModel의 기본 지침은 파일 내용을 도구로 확인하고 관측값을 예문으로 치환하지 않도록 요구한다. 사용자가 정확한 원문을 요구할 때에는 관측한 텍스트를 문자 그대로 반환하도록 명시한다. 이는 모델에 주는 지침이며 출력 형식이나 답변 정확도를 강제하는 검증기는 아니다. 실제 소형 모델이 지침을 어긴 사례, 입력·캐시 대조와 수정 후 수락 시험은 Verification.md에 기록한다.

0.13.1부터 ServiceModel은 각 tool 메시지의 content를 `{"text": 원문, "data": 구조화 결과, "is_error": 오류 여부}` JSON 문자열로 전달한다. 이전에는 text만 전달해 구조화 결과만 있는 앱 도구, 부분 읽기의 complete=false, 검색의 truncated/limit_reached, 셸 exit_code·interrupted 정보를 모델 입력에서 누락했다. 이제 빈 text와 중첩 data도 보존하며 원문 공백·줄바꿈·따옴표를 JSON 인코딩으로 왕복한다. 오류 여부를 별도 필드로 전달하므로 원문에 `Tool error:`를 덧붙이지 않는다. 정확한 파일 답변은 JSON을 해석한 text 값이다. Message.metadata는 관측 객체에 복사하지 않는다. 도구가 data 안에 포함한 metadata 값은 다른 구조화 결과와 함께 그대로 전달한다.

에이전트 대화의 예산 측정과 생성은 같은 변환을 사용하므로 구조화 결과가 컨텍스트 예산에도 반영된다. 요약용 JSON 기록은 기존 원본 보존 계약을 따른다. 결과가 너무 크면 기존 압축·한도 오류 경로를 따르며 data를 몰래 버리지 않는다. 일반 `Service::converse` 호출자의 content 형식과 저장된 원본 transcript는 이 어댑터 변환의 대상이 아니다. 이 변경은 모델에 관측값을 전달하는 계약을 고치며, 모델이 올바르게 답한다고 보증하지 않는다.

현재 `ServiceModel`은 `Service::converse`의 구조화 대화 API를 사용한다. `ConversationRequest`는 OpenAI function-call 형식의 텍스트 메시지·도구 정의와 `auto/required/none` toolChoice를 받는다. llama.cpp에 고정된 upstream common의 Jinja 채팅 템플릿, 도구 문법 샘플러, PEG 응답 파서를 사용하며 도구 결과를 user 메시지로 바꾸지 않는다. 미지원 런타임은 RuntimeUnavailable을 반환한다. MLX 네이티브 도구 호출은 아직 미완료이다.

구조화 대화도 기존 Service의 단일 추론 스케줄러, 모델 무결성/메모리 정책, 제한된 KV 캐시를 사용한다. contextId는 모델별로 분리해 재사용하며 빈 ID는 일회성 컨텍스트이다. 대화 기록은 호출자가 소유한다. 잘못된 도구 ID 연결, 알 수 없는 도구, 잘린 호출을 거부한다. 컨텍스트 초과 시 도구 결과를 임의로 잘라내지 않고 ContextOverflow를 반환한다. 현재 텍스트 Delta는 완성된 응답의 파싱 후 전달한다. 토큰/도구 인자/추론 블록의 점진적 스트리밍은 후속 구현 대상이다.

Tool은 정의, 실행 함수, 선택적 도메인 검증, 입력별 동시 실행 판정을 갖는다. JSON Schema 2020-12를 기본 dialect로 사용하며 `$schema`가 있으면 지정 dialect로 검증한다. 네트워크 `$ref` resolver는 설치하지 않는다. 입력 변경 훅 이후에는 스키마와 도메인 검증을 다시 수행한다. 출력 스키마가 있으면 성공 결과의 data를 검증한다.

한 모델/도구 턴은 도구 정의·스키마·실행 함수를 같은 스냅샷으로 고정한다. 실행 도중 registry를 갱신해도 이전 정의와 새로운 실행 함수가 섞이지 않으며 다음 모델 호출부터 갱신 내용을 반영한다. 직접 ToolRunner를 사용할 때도 한 실행의 입력/출력 검증과 실행 함수는 같은 등록 항목에 묶인다.

정책 거부·스키마 실패·도구 오류는 isError 도구 결과로 모델에 전달한다. 취소·소비자 콜백 오류는 실행 자체를 종료한다. 긴 텍스트 결과는 세션 artifact에 보관하고 참조를 반환한다. 파일 쓰기·셸·외부 도구의 부작용이 자동 롤백되는 계약은 아니다.

## 세션·중단·동시성

SessionStore는 `<sessions>/<uuid>/transcript.jsonl`을 사용한다. 첫 줄은 버전·모델·작업 디렉터리를 담고, 이후 메시지는 parent_id로 연결한다. 실행 동안 QLockFile을 유지해 다른 프로세스의 동시 기록도 막는다. 마지막 불완전 레코드는 버리고, 중간 JSON 오류나 연결 불일치는 손상으로 처리한다. 프로세스 중단 복원을 지원하지만 fsync 기반 전원 장애 내구성을 보장하는 것은 아니다.

도구 요청은 실제 실행 전에 기록한다. 재개 시 결과가 기록되지 않은 요청은 `결과 미확인` 오류 결과로 닫으며 자동 재실행하지 않는다. 취소·실패 뒤에도 가능한 경우 남은 도구 요청에 대응 결과를 기록한다. 동일 세션에는 동시에 하나의 실행만 수락한다.

`SessionStore::fork`/`Engine::forkSession`은 실행 중이 아닌 transcript를 지정 메시지까지 별도 UUID로 게시한다. 결과가 빠진 도구 호출 경계는 거부한다. 0.47은 artifact와 파일 체크포인트의 독립 복사·경로 갱신·실패 정리를 추가한다. 생성 헤더와 복사 기록의 용량도 게시 전에 검사한다. [SessionFork.md](SessionFork.md)의 전체/메시지 경계 분기 계약을 따른다.

Engine은 제한된 QThreadPool과 수락 대기열을 사용한다. 도구는 입력별 동시 실행 가능 여부에 따라 묶으며 기본 최대 10개이다. 동시 실행 불가 도구는 앞선 묶음의 완료 후 실행한다. 입력을 바꿀 수 있는 훅이 있으면 현재 스케줄러는 도구 실행을 직렬화한다. 이벤트 콜백은 실행별로 직렬화하지만 서로 다른 실행 간에는 동시에 호출될 수 있다.

모델 응답 전체가 반환된 뒤 도구를 스케줄링하는 경로가 현재 구현이다. Claude 분석본의 콘텐츠 블록 완료 시점부터 실행하는 최적화는 별도 미완료 항목이다.

## 권한과 기본 도구

RulePolicy는 Default/AcceptEdits/DontAsk/Bypass/Plan을 제공한다. 명시적 deny가 먼저 적용되고 ask는 allow보다 우선한다. Plan은 읽기 전용 도구만 허용한다. DontAsk는 남은 ask를 deny로 바꾼다. Ask는 호스트의 PermissionCallback이 있어야 허용할 수 있다. 자동 분류기·내용별 세부 규칙·OS 샌드박스는 아직 추가되지 않았다.

`registerWorkspaceTools`는 한 canonical workspace에 Read/Write/Edit/Glob/Grep/Bash를 등록한다. 이 registry를 다른 작업 디렉터리의 세션에 재사용하지 않는다. 파일 작업은 기존 symlink 해석을 거쳐 workspace 밖을 거부하고, 현재 세션의 artifact는 읽기만 허용한다. 경로 검증은 OS 샌드박스나 외부 프로세스의 경로 교체까지 막는 파일 잠금과 다르다.

- Read: UTF-8, 최대 1 MiB, 줄 범위. 완전 읽기 상태와 SHA-256을 기록한다.
- Write/Edit: 기존 파일의 완전 읽기·내용 일치를 요구한다. 변경 전 백업과 QSaveFile 쓰기를 사용한다.
- Glob: 파일 패턴, 최대 1,000개. Git ignore·완전한 globstar 호환은 미완료이다.
- Grep: Qt 정규식, 최대 10,000개 파일·100개 일치, 파일당 1 MiB. rg 고급 검색은 미완료이며, LSP 코드 탐색은 [Lsp.md](Lsp.md)를 따른다.
- Bash: 작업 디렉터리에서 별도 프로세스 실행, 시간/출력 제한, Unix 프로세스 그룹 취소. 0.12.0부터 ShellTasks를 등록한 데스크톱 POSIX 호스트에서 명시적 백그라운드 실행·출력·중단을 제공한다. OS 샌드박스, 셸 환경/cwd 지속, Windows 프로세스 트리는 미완료이다.

## 앱에서 사용하는 예

```cpp
#include <agent/Engine.h>

auto tools = std::make_shared<iiLocalLLM::agent::ToolRegistry>();
iiLocalLLM::agent::registerWorkspaceTools(*tools, workspace);
iiLocalLLM::agent::EngineOptions options;
options.sessionsDirectory = sessionsDirectory;
iiLocalLLM::agent::Engine engine(
    std::make_shared<iiLocalLLM::agent::ServiceModel>(service), tools,
    std::make_shared<iiLocalLLM::agent::RulePolicy>(), options);
auto session = engine.createSession("model://my-model", workspace);
auto handle = engine.run({session.id, "Read the project and explain it"}, onEvent);
// UI 스레드에서 future.get()을 호출하지 않는다.
```

앱은 ToolRegistry::add로 자체 기능을 등록할 수 있다. metadata의 app_id 같은 식별자는 앱의 도구 출처를 표현하며 인증을 대신하지 않는다. mcpTools는 외부 stdio MCP 도구를 같은 registry에 연결한다. content·metadata는 세션과 이벤트에 보존하고 structuredContent는 data의 출력 스키마를 검증한다. 실제 앱별 연동, 자동 발견 및 남은 MCP/API 전송은 대응표에서 별도 검증한다.

Service는 ServiceModel과 Engine보다 오래 살아야 한다. Engine 파괴는 수락한 실행을 취소하고 작업 스레드를 join한다. 이벤트·모델·도구 콜백에서 Engine을 파괴하거나 자신의 future를 기다리지 않는다. Qt UI를 갱신할 때는 앱이 자신의 UI 스레드로 이벤트를 전달한다.

## 검증

`iiLocalLLM.agent`는 스키마·정책·훅 재검증·세션 잠금/복원·도구 반복·취소·병렬 실행·턴 제한·파일 변경 감지·셸 제한을 검사한다. `iiLocalLLM.agent_local_inference`는 실제 Qwen GGUF가 Read를 선택하고, 프롬프트에 없는 임의의 파일 값을 최종 답변에 반환하는지 확인한다. 설치된 패키지의 `iiLocalLLM.installed_agent_consumer`는 외부 C++ 프로그램에서 앱 도구·모델·세션 ABI를 검증한다. 각각의 실행 결과는 Verification.md에 별도 기록한다.

Native ServiceModel의 도구 오류는 tool 역할의 `Tool error:` 결과로 전달한다. 기본 llama.cpp 로그에서는 생성 토큰을 포함하는 debug 메시지를 내보내지 않는다. 모든 CTest 임시 디렉터리는 build/tmp 아래에 생성한다.

MCP 서버는 [MCPServer.md](MCPServer.md)의 C++ ToolRegistry 공개와 연결별 로컬 에이전트 실행을 제공한다. 파일 읽기 이력·권한·동시 실행 경계를 유지한다. 인증된 HTTP 전송과 실행 중인 앱 발견은 각각 [MCPHTTPServer.md](MCPHTTPServer.md), [LocalApplications.md](LocalApplications.md)에 구현 범위를 기록하며 전체 제품·플랫폼 검증은 남아 있다.

## 프로젝트 지침과 입력 조합 (0.5.0)

`EngineOptions.projectContext`의 기본 동작은 세션 작업 폴더 안의 CLAUDE.md·AGENTS.md·경로별 규칙을 불러오는 것이다. `Engine::context()`로 현재 조합을 조회하며 `RunRequest.contextPaths`로 시작 시 적용할 파일 경로를 지정한다. `Read`·`Write`·`Edit`이 실제 관측한 workspace 경로도 다음 모델 호출부터 적용한다. 조합된 지침은 host system prompt를 바꾸지 않는 임시 User 메시지이며 원본 transcript에 중복 저장하지 않는다. 자세한 계약과 기준 소스 대비 차이는 [ProjectContext.md](ProjectContext.md)에 있다. `EngineOptions`·`RunRequest` ABI 변경으로 0.5 헤더를 사용하는 앱은 0.5 라이브러리에 링크해야 한다.

## 대화 압축 (0.6.0)

매 모델 호출 전 네이티브 입력 예산을 확인하고 오래된 도구 결과 축소·대화 요약을 수행한다. `Engine::compact()`는 수동 요약을 같은 실행 큐에 제출한다. 원본 메시지와 별도 압축 체크포인트를 저장하고 재개·분기 시 모델 뷰만 복원한다. `Session`·`ToolContext`·`RunUsage`·`Model` 인터페이스는 0.6 ABI를 사용한다. 옵션·저장 형식·원문 조회·훅·검증 한계는 [Compaction.md](Compaction.md)에 있다.

## 0.9.0 MCP 연결 관리와 도구 검색

호스트가 지정한 MCP 설정 파일의 연결·복구, 대화별 `ToolSearch`, 인증된 `agent.mcp.status` 및 CLI 옵션을 추가했다. 설정과 실행 권한, 수명 및 미지원 범위는 [ToolDiscovery.md](ToolDiscovery.md)를 참조한다.

## 영속 작업 상태 (0.11.0)

EngineOptions.taskToolsEnabled로 작업 도구를 활성화한다. TaskStore는 transcript와 별도 잠금을 사용하며 runTaskTool은 모델 실행 중에도 동일 정책·훅을 적용한다. TaskCreated/TaskCompleted는 게시 전에 block할 수 있다. SessionStore::metadata와 Engine::sessionMetadata는 활성 실행 중에도 불변 대화 헤더만 읽고 messages/compactions는 비워 반환한다. 목록 격리·재시작·fork·컨텍스트 및 정확한 저장 계약은 [Tasks.md](Tasks.md)를 참조한다.

## 백그라운드 실행 상태 (0.12.0)

`registerWorkspaceTools(registry, workspace, shells)`로 C++ ShellTasks를 연결한다. Engine의 `runShellTool`은 별도 대화 기록 잠금 없이 동일 정책·훅과 세션 소유권을 적용하며 진행 중인 모델 실행과 독립적으로 사용할 수 있다. 각 모델 턴에는 최대 32개의 실행 상태를 임시 데이터 메시지로 조합하고 토큰 예산에 포함한다. 출력 본문은 미리보기에 넣지 않으며 TaskOutput 또는 허용된 Read로 조회한다. 제어 도구는 기본 지연 공개이고 실제 eager 추론 검증은 별도 조건이다. [BackgroundTasks.md](BackgroundTasks.md)에 저장·수명·권한과 미완료 범위를 기록한다.

0.13.0의 대화별 [입력 큐](InputQueue.md)는 우선순위·원자적 저장·중복 복구와 실행 중 모델/도구 연산 취소를 기존 Engine에 연결한다. 실행 토큰과 연산 토큰을 분리하여 now 입력이 전체 실행 취소로 바뀌지 않게 한다. 유휴 큐는 명시적으로 시작하며 자동 기동·완료 알림 생산은 아직 남아 있다.

로컬 스킬은 메타데이터 목록을 모델 컨텍스트에 넣고 `Skill` 호출 뒤 본문을 사용자 메시지로 저장한다. 도구 결과와 본문 사이의 중단도 복구하며, 새 권한은 부여하지 않는다. [스킬 계약](Skills.md)을 참조한다.

0.15.0은 C++ 서브에이전트와 `agent.agents.run/output/stop/list`, 대응 MCP·CLI 경로를 추가한다. 소유권·취소·분기·재개와 설정 계약은 [Subagents.md](Subagents.md)를 따른다.

0.16.0의 `agent.agents.profiles`(MCP: `iiLocalLLM.agent.agents.profiles`, CLI: `agent agents profiles SESSION`)는 프로파일 메타데이터·출처·가려진 정의·오류를 반환한다. C++ 호스트는 `Subagents::attach`로 현재 프로파일을 각 턴에 연결한다. 독립 데몬과 MCP 서버의 `--agent-profiles FILE`·`--no-agent-profiles`, 모델 사용 범위와 훅 계약은 [AgentProfiles.md](AgentProfiles.md)를 따른다.

0.17.0에서는 `context: fork` 스킬을 같은 API·MCP·CLI 호출로 별도 자식에서 실행한다. 직접 호출은 자식 결과를 반환하고 모델의 `Skill` 호출은 후속 부모 턴에 결과를 전달한다. 본문 분리·권한·모델·사용량·큐 입력과 참조 차이는 [Skills.md](Skills.md)의 별도 자식 실행 계약을 따른다.

0.18.0의 스킬 allowed-tools와 인자 권한 규칙은 [Permissions.md](Permissions.md)를 따른다. API·IPC·MCP 입력은 allowed_tools/prompt_metadata 같은 호스트 전용 권한·출처 필드를 받지 않는다. 모델 Skill의 PermissionRequested 이벤트에는 고정된 permission_preview가 있다. 원격 권한 응답 중개는 아직 지원하지 않는다.

0.22는 사용자 제출·세션 활성화 훅을 C++ Engine에 연결한다. 내부 자식 지시와 모델 Skill 실행에는 사용자 제출 훅을 반복하지 않고 자식의 기존 SubagentStart/Stop을 유지한다. 원문·차단·중단과 prepare/persist 큐의 실패 계약은 [InputLifecycle.md](InputLifecycle.md)에 명시한다.

## 0.27 앱 권한 요청

인증 API/native IPC와 MCP 연결별 승인 채널, 독립 제어 처리, 훅·앱 경쟁, 취소·기한·중복 응답을 추가한다. [PermissionRequests.md](PermissionRequests.md)에 활성화·요청·응답 계약과 참조 차이를 기술한다. 전체 하네스와 실제 앱 통합 완료를 의미하지 않는다.

C++ 호스트가 등록한 `Tool.completesRun` 도구는 성공할 때 추가 모델 턴 없이 실행을 완료한다. 항상 직렬 경계이며 같은 응답에서 뒤따르는 도구는 실행하지 않고 이력에 `not_executed`로 기록한다. 실패한 호출은 완료로 취급하지 않는다. 이 속성은 모델/MCP 결과나 metadata에서 읽지 않는다.

0.40은 네이티브 `SessionSearch`와 Engine 직접 호출, 인증 API·MCP·CLI를 추가한다. 메모리 정리에 필요한 큰 JSONL 기록 검색을 제공하며, 의미 검색과 자동 dream 정리는 별도 미완료 항목이다. [SessionHistory.md](SessionHistory.md)에 소유 범위와 읽기 한도를 기록한다.

## 0.41 프로젝트 메모리 정리

C++ MemoryDream은 기본 OFF인 자동 실행, 24시간·5개 대화·10분 목록 주기, QLockFile의 프로세스 간 배제와 성공 시각 저장을 추가한다. 부모 문맥·도구 정의·읽기 캐시를 유지하고 추출과 공통 실행 코드를 사용하며, 주 transcript와 분리된 진행/완료 상태를 API·MCP·CLI로 제공한다. 수동 요청도 동일 권한·잠금·한도를 적용한다. [MemoryDream.md](MemoryDream.md)에 참조의 낙관적 타임스탬프/기능 플래그와의 차이, 부분 쓰기와 협력적 취소, 남은 앱 UI·worktree·팀·원격·플랫폼 범위를 명시한다. 대응표는 23 partial·8 pending·0 complete를 유지한다.


0.42는 C++ WebFetch의 익명 URL 조회, HTML5/문자 인코딩 변환, 소유자별 캐시, 로컬 모델 추출과 도메인 권한을 제공한다. [WebFetch.md](WebFetch.md)에 상한과 참조 차이 및 남은 웹 기능을 기록한다.

0.43은 C++ LSP로 정의·참조·호버·심볼·구현·호출 계층을 조회한다. 문서 동기화·세션별 서버 수명·권한과 API/MCP 노출은 [Lsp.md](Lsp.md)를 따른다. 편집기 연결과 전체 프로젝트 파일 watcher는 남아 있다.


0.44의 `EnterWorktree`·`ExitWorktree`와 소유 상태 조회는 [Worktrees.md](Worktrees.md)에 정의한다. `agent.worktrees.enter/exit/status`, CLI `agent worktrees`, MCP `iiLocalLLM.agent.worktrees.status`가 같은 실행 경로·권한·보존 계약을 사용한다. 활성 worktree의 clear와 팀/서브에이전트 전체 격리는 남아 있다.


## Jupyter 노트북 셀 편집 (0.45)

C++ `NotebookEdit`와 `agent.notebooks.read/edit`, CLI `agent notebooks`, MCP `iisacc/notebooks`는 [Notebooks.md](Notebooks.md)의 읽기 선행·경로 권한·세션/revision·원자적 저장·백업 계약을 공유한다. API의 노트북 읽기는 raw UTF-8 JSON이며 kernel 실행과 셀 이미지 렌더링은 포함하지 않는다. 생산 의존성은 기존 Qt Core이며 공식 nbformat은 독립 검증에만 사용한다.

## 파일 체크포인트 (0.46)

C++ 네이티브 Write·Edit·NotebookEdit의 원본을 사용자 메시지 경계에 기록하고 목록·수동 생성·파일 복원을 제공한다. 인증 API의 `agent.checkpoints.*`, CLI의 `agent checkpoints`, 연결 소유자에 묶인 MCP `iiLocalLLM.agent.checkpoints.*`가 같은 Engine을 사용한다. 저장 한도, 세션 잠금, preview·권한·작업 트리 경계와 남은 참조 기능은 [FileCheckpoints.md](FileCheckpoints.md)에 기록한다. 전체 대응 상태는 27 partial·4 pending·0 complete를 유지한다.

C++ 로컬 팀, 공유 작업 목록, 메일함과 API/MCP/CLI 제어는 [Teams.md](Teams.md)에 설명한다. 일반 Subagents와 함께 사용할 때 Subagents::attach 후 Teams::attach 순서로 구성한다.

단일 도구 호출이 필요한 호스트는 EngineOptions::maxToolCallsPerTurn=1을 사용한다. ModelRequest::parallelToolCalls=false가 ServiceModel의 측정·생성 요청으로 전달되며 실제 응답의 호출 개수 검사도 적용한다.

0.49는 고정된 llama.cpp의 JSON 도구 호출 문법도 보정한다. 호출별 태그를 반복하는 바깥 문법이 `parallel_tool_calls=false`에서도 여러 호출을 허용하던 경로를 한 건으로 제한한다. 생성 문법을 끄면 이 강제 제약은 없지만 서비스의 응답 개수 검사는 유지한다. 기존 병렬 호출 설정은 유지하며 호출을 잘라내거나 도구 결과를 만들어 넣지 않는다. `tests/native_grammar_tests.cpp`는 가중치 없이 Qwen 2.5·3의 auto/required, 단일/병렬 조합에서 실제 생성 문법의 수락·거절을 검사한다. 모델을 사용한 자동 작업 수락 검사는 [Teams.md](Teams.md)와 [Verification.md](Verification.md)에 별도로 기록한다.
