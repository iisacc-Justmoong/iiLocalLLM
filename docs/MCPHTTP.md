# MCP Streamable HTTP 클라이언트

`mcp::HttpClient`는 외부 앱의 MCP HTTP endpoint에 연결한다. `StdioClient`와 같은
`mcp::Client` 요청 처리 계층을 사용하므로 도구·리소스·프롬프트, 역방향 호스트 요청,
진행·취소, 목록 검증과 `agent::mcpTools`를 공통으로 사용한다. 생산 구현은 C++이며
이미 사용하는 Qt 6.8.3 Network 이외의 의존성을 추가하지 않았다.

```cpp
#include <mcp/HttpClient.h>
#include <agent/McpTools.h>

iiLocalLLM::mcp::HttpOptions http;
http.endpoint = QUrl("https://app.example/mcp"); // 호스트가 선택한 실제 앱 endpoint
http.bearerToken = [credentials] { return credentials->currentAccessToken(); };
auto client = std::make_shared<iiLocalLLM::mcp::HttpClient>(http);
auto tools = iiLocalLLM::agent::mcpTools(client, {"society", "com.iisacc.society"});
for (auto& tool : tools) registry->add(std::move(tool));
```

주소와 credentials는 호스트가 제공하는 값이며 Society에 이 주소나 credential API가
이미 구현되어 있다는 의미가 아니다. 실제 제품 endpoint·자동 발견은 별도 작업이다.
생성·요청·파괴는 UI 스레드 밖에서 실행한다. 공개 API는 blocking이고 동시 호출을
지원한다. 호스트 콜백 안에서 같은 클라이언트의 request를 사용할 수 있지만 close나
파괴를 호출하면 안 된다. 콜백은 전달된 취소 토큰에 협력해야 한다.

## 전송 및 복원

- 지원 버전은 `2025-11-25`, `2025-06-18`, `2025-03-26`이다. 초기화에서 선택한
  버전과 서버의 세션 식별자를 후속 HTTP 헤더에 반영한다. 초기화 완료 알림의 202
  응답을 받은 후 일반 요청을 전송하여 서버의 초기화 처리와 경합하지 않는다.
- POST는 JSON 및 SSE를 수신하고, 선택적 GET 채널에서 서버 알림·요청을 받는다.
  `listenForNotifications=false`는 이 GET 채널을 끈다. 서버의 GET 405도 처리한다.
  역방향 요청에 대한 클라이언트 응답과 일반 알림은 개별 POST로 전송한다.
- SSE의 CR/LF/CRLF, UTF-8 및 BOM, 여러 data 행, 주석, id·retry, 분할 입력을 처리한다.
  수신 응답의 ID가 해당 HTTP 요청과 일치하는지 확인한다. 원래 POST의 SSE가 끊어지면
  마지막 이벤트 ID를 사용한 GET으로만 복원한다. 서버의 retry 지연을 따른다.
- SSE 이벤트 ID 없이 응답 전에 연결이 끝나면 호출자에게 원격 결과를 알 수 없다는
  오류를 반환한다. 전송 중이던 도구 POST를 다시 실행하지 않는다. Qt 내부의 업로드 재전송도 되감기를 거부하는 QIODevice로 차단한다. 기본 재연결 간격은
  1초, 연속 복원 시도 한도는 8회이며 요청의 전체 시간 제한은 계속 적용된다.
- 2025 규격의 취소·시간 초과는 `notifications/cancelled`를 전송하고 해당 응답 채널을
  닫는다. 네트워크 단절만으로 원격 작업을 취소하거나 변경을 되돌렸다고 간주하지 않는다.
- 세션 ID가 붙은 요청에 404가 오면 기존 요청들을 HttpError(404)로 종료하고 새
  initialize를 전송한다. 처리 중이던 요청은 새 세션에 옮기지 않는다. 새 협상이 끝나면
  `connectionGeneration()`이 증가한다. 새 초기화가 실패하면 연결은 종료된다.
- `mcpTools`가 만든 도구는 발견 당시 연결 세대를 보존한다. 큐에 등록하는 시점에도
  세대를 검사하여 이전 스키마로 새 세션에 요청하지 않는다. 호스트는 새 세션의 도구를
  다시 발견하고 실행 경계에서 registry를 교체한다. `request`·`callTool`의 마지막
  `expectedGeneration` 인자는 직접 사용하는 호스트에도 같은 조건을 제공한다.

이 동작은 [2025 전송 규격](https://modelcontextprotocol.io/specification/2025-11-25/basic/transports)에
대응한다. [2026-07-28 전송](https://modelcontextprotocol.io/specification/2026-07-28/basic/transports/streamable-http)은
요청별 메타데이터와 취소 방식이 바뀌었으므로 현재 지원 버전에 포함하지 않는다.
legacy HTTP+SSE endpoint 협상과 최신 규격 지원은 전체 대응표의 미완료 항목이다.

## 인증·자원·수명

TLS는 서버 인증서를 검증한다. 기본 HTTP 허용 범위는 loopback이며, 신뢰한 LAN
HTTP를 사용하려는 호스트는 `allowInsecureHttp`를 명시한다. URL의 사용자 정보나
fragment는 허용하지 않는다. 리다이렉트를 따라가지 않으며 쿠키 저장·재사용도 하지 않는다.

`bearerToken`은 HTTP 교환마다 I/O 스레드에서 호출한다. 빠르게 반환해야 하며 자체
네트워크 작업·사용자 UI를 실행해서는 안 된다. HTTP 상태·전송·자격 증명 공급자 오류의
what에는 토큰, URL, 요청 본문, 서버 HTTP 오류 본문을 넣지 않는다. JSON-RPC의
`RpcError`는 서버가 보낸 메시지를 포함한다. `HttpError::statusCode()`와 `wwwAuthenticate()`는
호스트가 인증 거부와 challenge를 처리하도록 제공한다. challenge는 원격 데이터다.
OAuth discovery·PKCE·토큰 저장·대화형 로그인은 아직 이 클라이언트가 수행하지 않는다.

`headers`에는 호스트의 추가 헤더를 넣을 수 있지만 인증·세션·프로토콜·프레이밍 헤더를
덮어쓰거나 이름·값에 개행을 주입할 수 없다. 자격 증명과 세션을 같은 사용자에게 연결하는 검사는
원격 MCP 서버도 수행해야 한다.

기본 공통 한도는 128개 요청, 4개 호스트 요청, 메시지 8 MiB, 대기·입력 버퍼 16 MiB다.
HTTP 응답은 64 KiB 단위로 읽고 전체 교환의 입력 누적량도 제한한다. HTTP 관리자는
제한된 교환 수 안에서 재사용하며, 오래 유지되는 SSE가 역방향 응답이나 호스트의 중첩
요청 전송을 막지 않도록 각각의 교환에 독립 연결 풀을 배정한다.

close는 SSE·전송 중인 요청을 정리하고 지정된 종료 시간 안에서 DELETE를 시도한다.
QObject와 네트워크 연결은 생성한 I/O 스레드에서 정리한다. stdio의 QProcess 수명과
Unix 자식 프로세스 그룹 정리는 별도 전송에 남아 있다.

공통 Client 및 공개 옵션 구조 변경으로 SDK/SOVERSION은 **0.7.0/0.7**이다.
소비자는 헤더와 라이브러리를 함께 갱신해 다시 빌드해야 한다.

## 검증

`iiLocalLLM.mcp_http_client`는 공식 SDK와 독립적인 Python 표준 라이브러리 HTTP peer로
JSON/SSE, 동시 역방향 요청 및 그 안의 중첩 요청, 진행, 취소·시간 초과, 복원 시 POST
횟수, 세션 404와 이전 도구 차단, 서로 다른 클라이언트 세션, 인증·리다이렉트 및 자원
한도를 검사한다. `iiLocalLLM.mcp_stdio`는 같은 공통 계층의 기존 stdio 회귀 시험이다.

`iiLocalLLM.mcp_http_official`은 설치된 공식 Python SDK 1.26.0(MIT)의 실제
Streamable HTTP 서버를 별도 프로세스로 띄워 초기화, 도구, 구조화 결과, 진행,
리소스, 프롬프트, 역방향 roots 및 에이전트 registry 어댑터를 검증한다. 설치 consumer에도
같은 시험을 적용한다. 이 Python 의존성은 시험 전용이며 생산 패키지에는 포함하지 않는다.
실행 결과는 [Verification.md](Verification.md)에 기록한다.
