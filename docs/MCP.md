# MCP 클라이언트와 서버

0.20.0은 호스트의 추가 디렉터리를 MCP 파일 도구와 에이전트 실행에 반영한다.
`iiLocalLLM.agent.permissions.get`의 경로·출처 조회, `--add-dir`과 비공개 저장소
보호는 [WorkingDirectories.md](WorkingDirectories.md)를 따른다.

0.4.0은 C++ stdio MCP 클라이언트·서버와 에이전트 도구 어댑터를 제공한다. 서버 실행 파일·앱 도구 공개·연결별 로컬 에이전트 실행은 [MCPServer.md](MCPServer.md)를 참조한다. MCP 서버가 공개한 도구·리소스·프롬프트를 읽고, 기존 ToolRegistry·권한 정책·실행 엔진에서 외부 도구를 호출한다. 생산 런타임에는 Python이나 TypeScript 의존성이 추가되지 않는다.

Streamable HTTP 클라이언트와 세션 복원은 [MCPHTTP.md](MCPHTTP.md)를 참조한다. 전체 MCP 요구사항의 완료 상태는 아니다. 인증된 HTTP 서버는 [MCPHTTPServer.md](MCPHTTPServer.md)에 있다. legacy SSE·OAuth, tasks, 자동 앱 발견은 남아 있다. 지원 프로토콜은 2025-11-25, 2025-06-18, 2025-03-26이다. 2026-07-28 규격은 아직 지원 목록에 넣지 않는다.

## 연결과 수명

```cpp
#include <agent/McpTools.h>
#include <QtCore/QUrl>

iiLocalLLM::mcp::StdioOptions transport;
transport.program = "/absolute/path/to/app-mcp-server";
transport.arguments = {"--stdio"};
transport.workingDirectory = workspace;
iiLocalLLM::mcp::ClientOptions clientOptions;
clientOptions.roots = {QJsonObject{{"uri", QUrl::fromLocalFile(workspace).toString()}}};
auto client = std::make_shared<iiLocalLLM::mcp::StdioClient>(transport, clientOptions);
auto registry = std::make_shared<iiLocalLLM::agent::ToolRegistry>();
for (auto tool : iiLocalLLM::agent::mcpTools(client, {"society", "com.iisacc.society"}))
    registry->add(std::move(tool));
// registry를 기존 agent::Engine에 전달한다.
```

위 서버 경로는 앱이 실제 제공한 실행 파일로 설정한다. 이 예시는 Society 제품에 MCP 서버가 이미 들어 있다는 주장이 아니다. 앱 consumer 연동은 대응표에서 별도 검증한다.

생성자는 서버를 실행하고 initialize 응답 검증 및 notifications/initialized 전송 대기열 등록까지 수행한다. 공개 API는 blocking이며 서로 다른 호출 스레드에서 동시에 사용할 수 있다. 프로세스 입출력은 전용 스레드가 맡는다. 생성·요청·파괴는 UI 스레드 밖에서 실행한다. 서버 설명, instructions, 알림은 호스트가 읽을 수 있는 데이터이며 자동으로 시스템 프롬프트에 삽입하거나 실행하지 않는다.

stdio 환경은 기본적으로 현재 환경을 상속한다. 호스트가 StdioOptions.environment로 필요한 변수만 전달할 수 있다. 프로그램·인자를 셸 명령문으로 조합하지 않고 QProcess에 각각 전달한다. 연결 종료는 stdin EOF, 정상 종료 대기, terminate/kill 순서로 진행한다. Unix에서는 새 프로세스 그룹의 자식도 정리한다. Windows의 별도 자식 프로세스 트리 종료는 미검증이며 구현 범위에 남아 있다. 자식이 스스로 새 세션으로 분리하면 Unix 그룹 정리 범위를 벗어난다.

호스트 requestHandlers는 별도 제한된 스레드 풀에서 실행된다. 콜백은 CancellationToken에 협력해야 하며, 콜백 안에서 해당 클라이언트를 파괴하거나 close를 호출하지 않는다. 콜백 안에서 같은 클라이언트로 일반 request를 보내는 것은 지원한다. progress 콜백은 request를 호출한 스레드에서 순서대로 실행되므로 UI 갱신은 호스트가 UI 스레드로 전달한다.

## 프로토콜과 오류

로컬 RPC 기한이 소진되면 `mcp::RequestTimeoutError`로 메서드(최대 128자), 적용 기한, 단조 시계로 측정한 경과 시간, 전송 계층 제출 여부를 조회한다. 기존 `ErrorCode::Timeout` 처리는 유지된다. 제출은 상대 프로세스의 수신·실행 증거가 아니며 원격 결과는 알 수 없을 수 있다. 시간은 요청 준비부터 계산하며 프로세스 시작·종료 대기는 제외한다. 전송 계층 자체의 실패나 서버가 응답한 RPC 오류까지 이 형식으로 바꾸지는 않는다.

- UTF-8 JSON-RPC 2.0 객체를 줄바꿈으로 구분한다. 여러 프레임 합침, 프레임 분할, UTF-8 분할을 처리한다. 2025-03-26에서는 JSON-RPC batch를 수신하고 비동기 호스트 응답을 같은 배열로 결합한다. 이후 규격의 batch, 잘못된 인코딩·ID·JSON, 끝나지 않은 프레임은 거부한다. stderr는 별도 제한된 진단 버퍼다.
- 시간 초과·취소 오류에는 요청 메서드의 앞 128자까지 포함하고 요청 인자는 포함하지 않는다. 초기화 지연과 개별 도구 지연을 구분할 수 있으며 실패한 요청을 자동 재실행하지 않는다.
- 요청 ID별로 응답을 결합하므로 뒤에 보낸 요청이 먼저 끝나도 각 호출자에게 올바른 결과가 전달된다. 연결당 기본 128개 요청, 4개 역방향 호스트 요청, 8 MiB 프레임, 16 MiB 전송·알림 버퍼 한도를 적용한다. 목록은 전체 페이지를 합쳐 16 MiB·10,000개 항목 및 요청 시간 제한을 적용하며 반복 커서와 중복 식별자를 거부한다.
- initialize 응답의 버전·capabilities·serverInfo를 확인한다. 도구·리소스·프롬프트 전용 메서드는 서버가 해당 기능을 협상했을 때 호출한다. request는 호스트가 명시적으로 선택한 추가 메서드를 보내는 저수준 경계다.
- 도구 호출의 isError 결과와 JSON-RPC error를 구분한다. 저수준 RPC 오류는 mcp::RpcError의 rpcCode·data에 보존한다. 전송·프레이밍·수명 오류는 기존 iiLocalLLM::Error로 반환한다.
- 취소와 시간 초과는 notifications/cancelled를 보내고 호출자를 해제한다. initialize는 취소 알림 없이 연결을 정리한다. 늦게 도착한 응답을 다른 요청에 붙이지 않으며, 연결 실패 후 도구를 자동 재실행하지 않는다. 취소는 외부에서 이미 일어난 변경의 롤백을 보장하지 않는다.
- progressToken은 클라이언트가 요청별로 만든다. progress의 순서·수치 형식을 검사하고 콜백 예외는 ConsumerFailure로 반환한다. 기본 60초의 요청 제한은 progress 알림으로 무한 연장되지 않는다.

## 도구·자료·호스트 요청

listTools, callTool, listResources, listResourceTemplates, readResource, subscribeResource, unsubscribeResource, listPrompts, getPrompt를 제공한다. 원본 JSON을 보존하고 도구·프롬프트의 콘텐츠 블록 및 리소스의 text/blob 필드를 검증한다. takeNotifications는 목록 변경·리소스 갱신 등 수신 알림을 호스트에 전달한다. 자동 새로고침·도구 교체는 아직 수행하지 않으므로 호스트가 실행 경계에서 목록을 갱신한다.

roots/list에는 호스트가 명시한 file: URI만 반환한다. 절대 경로·중복·상위 경로 이동을 검증하고 setRoots가 바뀌었을 때 list_changed 알림을 보낸다. roots는 OS 샌드박스나 파일 접근 권한을 만들지 않는다. 호스트가 노출 권한과 실제 접근 가능 여부를 관리한다.

sampling/createMessage와 elicitation/create는 ClientOptions의 명시적 capability 및 requestHandlers가 함께 있을 때만 호스트로 전달한다. SDK가 임의로 모델을 실행하거나 사용자를 대신해 답하지 않는다. 역방향 요청도 취소 토큰을 받고, 호스트에서 지원하지 않는 메서드는 -32601로 응답한다. 상세 sampling/elicitation 자료형과 사용자 UI, task 연계 적합성은 아직 전체 검증하지 않았다.

mcpTools는 전체 도구 목록의 스키마를 컴파일한 뒤 도구 집합을 반환한다. 이름은 mcp__서버__도구 형식이며, 유효하지 않은 문자나 길이는 해시를 포함해 구분한다. 원래 이름·정의·app_id는 metadata에 보존한다. 기본값에서는 원격 readOnlyHint로 자동 허용하지 않는다. 호스트가 신뢰한 서버에만 trustAnnotations를 지정한다. 기존 ToolRunner가 입력·출력 스키마, 훅, 실행 정책을 적용한다.

ToolResult·Message의 content와 metadata는 MCP 원본 콘텐츠와 _meta를 유지한다. structuredContent는 기존 data에 저장하므로 outputSchema를 그대로 검증한다. 세션 JSONL과 ToolFinished 이벤트에도 내용이 보존된다. 현재 ServiceModel은 텍스트·리소스 링크·텍스트 리소스를 사용한다. 이미지·음성·blob을 지원하지 않는 네이티브 어댑터는 RuntimeUnavailable을 반환하며, 해당 자료를 조용히 버리지 않는다. 사용자 정의 Model은 원본 content를 처리할 수 있다.

공개 Message·ToolResult 구조가 늘어 ABI를 0.4로 구분했다. 이전 소비자는 새 헤더·라이브러리로 다시 빌드해야 한다. 기존 JSONL의 content·metadata가 없는 메시지도 읽는다.

기존 세션의 레코드당 4 MiB 제한은 유지한다. 그보다 큰 MCP 콘텐츠를 자동으로 외부 미디어 artifact에 옮기는 기능은 아직 없으므로, 이런 결과는 세션 저장 시 ResourceLimit으로 실패한다. 대형 멀티모달 결과 보관은 전체 하네스 대응표의 미완료 범위다.

## 검증과 의존성

Qt 6.8.3의 QProcess·JSON·스레드 풀과 기존 jsoncons 스키마 검증기를 재사용했다. [공식 SDK 목록](https://raw.githubusercontent.com/modelcontextprotocol/modelcontextprotocol/main/docs/docs/2026-07-28/sdk.mdx)에 C++ SDK가 없어 새 언어 런타임을 도입하지 않고 기존 Qt 전송에 프로토콜을 연결했다. 공식 Python SDK 1.26.0(MIT)은 별도 프로세스 교차 검증에만 사용한다. 패키지 생산 의존성이나 설치 산출물에 Python MCP를 포함하지 않는다.

기본 CTest iiLocalLLM.mcp_stdio는 Qt를 사용하지 않는 독립 Python 시험 peer로 순서 역전·취소·시간 초과·프로세스 종료·잘못된 프레임·목록 페이지·권한·구조화 결과·세션 보존을 검증한다. 선택 시험은 아래와 같이 준비한다.

```sh
uv venv build/mcp-env
UV_CACHE_DIR="$PWD/build/uv-cache" uv pip install --python build/mcp-env/bin/python mcp==1.26.0
cmake -S . -B build -DIILOCALLLM_TEST_MCP_PYTHON="$PWD/build/mcp-env/bin/python"
cmake --build build --parallel
ctest --test-dir build -R 'mcp' --output-on-failure
```

iiLocalLLM.mcp_official은 공식 SDK 서버의 도구·자료·프롬프트·진행 알림·역방향 roots를 검사한다. Qwen GGUF도 설정하면 iiLocalLLM.agent_mcp_inference에서 로컬 모델이 실제 MCP 도구를 선택해 프롬프트에 없는 임의 파일 값을 최종 응답으로 반환하는지 검사한다. 설치 소비자는 같은 공식 SDK 교차 검증을 설치된 헤더·라이브러리만으로 수행한다. 관측한 실행 결과는 Verification.md에 기록한다.

분석 참조는 Exhen/claude-code-2.1.88의 c8cd253554319f32ff64ff7000636199f720c9bc에서 services/mcp/client.ts의 stdio·roots·도구 변환 경로다. 구현은 다음 공식 프로토콜 계약을 기준으로 작성했다: [수명](https://modelcontextprotocol.io/specification/2025-11-25/basic/lifecycle), [전송](https://modelcontextprotocol.io/specification/2025-11-25/basic/transports), [도구](https://modelcontextprotocol.io/specification/2025-11-25/server/tools), [진행](https://modelcontextprotocol.io/specification/2025-11-25/basic/utilities/progress), [취소](https://modelcontextprotocol.io/specification/2025-11-25/basic/utilities/cancellation), [roots](https://modelcontextprotocol.io/specification/2025-11-25/client/roots).

## 프로젝트 지침 경로 (0.5.0)

`iiLocalLLM.agent.run`의 선택적 `context_paths`는 최대 128개 workspace 파일 경로를 받는다. 지침을 첫 모델 호출 전에 적용하며 연결별 Engine 세션에만 범위를 유지한다. 잘못된 경로·루트 이탈은 isError 결과이고, workspace의 파일을 읽은 경우에도 다음 모델 호출부터 하위 지침을 적용한다. `instructions_loaded`는 기존 MCP progress의 `iisacc/agentEvent`로 전달된다. [ProjectContext.md](ProjectContext.md)의 순서·상한·미구현 항목을 함께 참조한다.

## 0.9.0 MCP 연결 관리와 도구 검색

호스트가 지정한 MCP 설정 파일의 연결·복구, 대화별 `ToolSearch`, 인증된 `agent.mcp.status` 및 CLI 옵션을 추가했다. 설정과 실행 권한, 수명 및 미지원 범위는 [ToolDiscovery.md](ToolDiscovery.md)를 참조한다.

## 0.10.0 구조화 결과 전달

`structuredContent`가 있을 때 설명 텍스트가 있더라도 실제 JSON을 모델 입력에서 보존한다. 서버 bridge는 JSON text block을 함께 반환하고 클라이언트 어댑터는 상대 서버가 빠뜨린 경우 모델용 텍스트에 추가한다. 이미 같은 JSON이 있으면 공백 형식과 무관하게 중복하지 않는다. 원래 MCP content·구조화 데이터·host용 `_meta`는 각각 보존한다. `_meta`는 모델 텍스트에 추가하지 않는다. 실제 앱 연동과 검증은 [LocalApplications.md](LocalApplications.md)에 기록한다.

0.19.0의 `iiLocalLLM.agent.permissions.get`은 빈 인자로 호스트 권한·출처·SHA·미지원 이름을 조회한다. 모델 활성화 없이도 사용할 수 있고 stdio 또는 인증 HTTP의 연결 권한을 따른다. 일반 설정 값은 반환하지 않는다. `--permission-settings FILE`의 동작은 [PermissionSettings.md](PermissionSettings.md)를 따른다.

0.33.0의 비동기 명령 훅은 세션/연결별로 완료 결과를 보관하고 문맥을 notification/next로 전달한다. asyncRewake 종료 코드 2는 횟수를 제한한 유휴 실행을 요청한다. 세션 종료·clear는 훅을 취소·정리하며, 자식 Engine은 실행이 끝난 뒤 자동 기동하지 않는다. 상태/취소 메서드와 기존 요청별 권한·콜백을 재사용하지 않는 경계는 [AsyncHooks.md](AsyncHooks.md)를 따른다.
