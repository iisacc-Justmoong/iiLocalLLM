# 인증된 로컬 MCP HTTP 서버

0.8.0의 `mcp::HttpServer`는 C++ `ServerSession`을 `127.0.0.1:<port>/mcp`의 Streamable HTTP로 제공한다. `iillm-mcp`에서도 같은 전송을 선택할 수 있다. 기존 cpp-httplib 0.54.1(MIT)과 Qt Core를 재사용하며 생산 실행에 Python 서버나 새 HTTP 라이브러리가 필요하지 않다. 공식 Python MCP SDK는 교차 검증에만 사용한다.

## 실행

```sh
build/iillm-mcp --workspace /absolute/project --http-port 8787 \
  --credentials /absolute/private/clients.json --state /absolute/private/mcp-state
```

포트 0은 사용 가능한 포트를 선택한다. 하나의 주소·포트는 서버 하나가 소유하며 같은 포트를 요청한 두 번째 서버는 바인딩에 실패한다. 바인딩 실패 후 다른 포트로 다시 시도하거나 close 후 같은 인스턴스를 재사용할 수 있다. HTTP 모드 stdout의 첫 줄은 실제 `endpoint`를 담은 JSON이다. stdio 기본 모드의 stdout에는 계속 MCP 메시지만 기록한다. 종료 신호 SIGINT/SIGTERM을 받으면 수락한 작업을 취소하고 핸들러가 종료할 때까지 합류한다. stdin 종료는 HTTP 서버를 종료하지 않는다.

자격 증명 파일은 `{ "com.iisacc.example": "<임의 생성한 비밀 토큰>" }` 형식이며 현재 사용자 소유의 일반 파일, POSIX 권한 0600이어야 한다. 파일 자체의 symlink를 거부한다. 최대 64개 client ID는 영문·숫자로 시작하며 이후 영문·숫자·점·밑줄·하이픈으로 128자까지 쓴다. 토큰은 서로 다른 32~256자의 URL-safe 영문·숫자·밑줄·하이픈 문자열이다. 전체 문자열을 검증하므로 끝의 개행도 허용하지 않는다. 파일 크기는 64 KiB 이하다. 토큰을 명령 인자·모델 입력·진단에 넣지 않고 SHA-256 해시로 보관하여 비교한다. CLI는 시작 때 파일을 읽으므로 키 변경을 적용하려면 서버를 다시 시작한다.

인증 파일은 작업 폴더 밖에 있어야 한다. `--state`도 작업 폴더와 상하위 관계가 없는 별도 경로이며 권한을 0700으로 설정하고 프로세스 소유 잠금을 건다. HTTP 모드에서는 `--sessions`·`--artifacts` 대신 이 상태 폴더를 사용한다. 기록은 state/sessions, 큰 결과는 state/artifacts에 저장한다. 클라이언트의 clientInfo나 도구의 appId 태그를 인증으로 사용하지 않는다.

CLI에 등록한 모든 클라이언트는 명시한 하나의 작업 폴더와 도구 허용 규칙을 공유한다. 같은 도구 bridge를 재사용하여 연결 사이의 배타적 파일 변경 계약을 유지한다. 각 MCP 연결의 대화는 분리되며 다른 연결의 대화 ID를 지정해 조회·재개하는 도구는 제공하지 않는다. 앱마다 서로 다른 폴더·정책·모델을 제공하려면 아래 C++ factory를 사용한다. Read·Glob·Grep 기본 허용, `--allow`, Bash의 OS 샌드박스 제한은 [MCP 서버](MCPServer.md)와 같다.

`--models /absolute/Models --model model://<설치-ID>`를 추가하면 로컬 에이전트 도구도 HTTP로 제공한다. context·max-tokens·temperature·request-timeout은 기존 시작 옵션을 사용한다. 실제 서비스의 모델 카탈로그 잠금 및 하드웨어 정책을 유지한다.

## C++ 호스트 연결

```cpp
#include <mcp/HttpServer.h>

iiLocalLLM::mcp::HttpServerOptions http;
http.authenticate = [&](const QByteArray& bearer) {
    return hostAuthentication.lookupPrincipal(bearer);
};
iiLocalLLM::mcp::HttpServer server(
    [&](const QString& principal) { return appExports.at(principal); }, http);
if (!server.listen(0)) throw std::runtime_error(server.errorString().toStdString());
```

`hostAuthentication`과 `appExports`는 호스트 소유 객체를 나타낸 예시다. authenticate는 각 POST/GET/DELETE마다 호출되며 검증된 고정 principal을 반환한다. 빈 값은 401, 공급자 예외는 내용을 노출하지 않는 503이다. 인증된 다른 principal이 세션 ID를 제시하면 404다. 이미 열린 SSE의 인증을 주기적으로 다시 검사하지는 않는다. 즉시 권한 회수가 필요하면 호스트가 서버·세션 수명과 권한 정책을 함께 관리해야 한다.

factory는 인증된 principal에 해당하는 `ServerOptions`를 반환한다. 호스트가 미리 만든 `agent::mcpServerOptions`도 반환할 수 있다. 같은 공유 자원을 제공하는 연결들은 같은 bridge의 옵션을 재사용해야 배타적 실행이 조정된다. factory를 매 연결 새 bridge 생성에 사용하면서 동일 파일을 공유하면 서로 다른 bridge 사이의 변경은 호스트가 조정해야 한다. `sessionIds(principal)`, `notify(sessionId, ...)`, `takeNotifications()`로 호스트가 연결별 알림을 제어한다. 수신 알림에는 세션 ID와 인증된 principal을 함께 붙인다.

QCoreApplication이 필요하다. authenticate·factory는 동시 호출될 수 있으므로 스레드 안전하고 신속해야 한다. 도구와 역방향 요청 핸들러는 취소에 협력해야 한다. 생성/listen/close/port/endpoint/errorString은 직렬 수명 작업이며 UI 스레드 밖에서 실행한다. 서버 자신을 authenticate·factory·도구·onClosed 콜백 안에서 close/파괴하지 않는다. 서버와 Engine은 참조한 Service보다 먼저 종료한다.

## 전송과 재개

단일 `/mcp`에 POST·GET·DELETE를 제공한다. 초기화는 JSON 결과와 무작위 세션 ID를 반환한다. 나머지 요청은 SSE이며 관련 진행 알림·역방향 요청·최종 응답을 원래 요청의 스트림으로 전달한다. 알림·역방향 응답을 담은 POST는 본문 없는 202다. 새 GET은 별도 알림 스트림이다. 동일 알림 스트림의 두 번째 일반 GET은 409로 거부하고 다른 요청의 응답을 방송하지 않는다.

SSE의 첫 이벤트는 ID·retry와 빈 data로 재개 위치를 알린다. ID는 스트림 UUID와 순번이다. 연결이 끊겨도 작업을 취소하거나 원래 POST를 재실행하지 않는다. 클라이언트는 Last-Event-ID를 담은 GET으로 해당 스트림의 이후 이벤트를 재개한다. 재개 GET이 수락되면 이전 reader의 lease를 교체한다. 다른 스트림의 이벤트를 섞지 않는다. 이미 처리한 작업의 결과를 재전달할 수 있으므로 호출자는 이벤트 ID를 관리해야 한다.

취소는 명시적인 `notifications/cancelled`로 처리한다. `ServerFrame`은 JSON 메시지에 포함되지 않는 channel을 보존한다. 취소된 핸들러 종료 시 message=Undefined와 cancelledRequestId를 담은 내부 프레임을 보내며, HTTP 전송은 앞선 역방향 취소·배치 응답 뒤에 스트림을 닫는다. 이 내부 프레임을 JSON으로 직렬화하지 않는다. `takeMessages()`는 내부 프레임을 제외한다. `takeMessages`와 `takeFrames`는 같은 큐의 대체 소비 API이며 동시에 소비하면 안 된다.

협상 버전은 2025-11-25·2025-06-18·2025-03-26이다. 2025-03의 배열 요청·취소·오류 응답 결합을 지원한다. 이후 버전에서 배열은 400이다. 세션 이후의 프로토콜 헤더는 협상 값과 같아야 하며, 생략된 경우 서버에 저장된 협상 값을 사용한다. DELETE/만료 후에는 404, 유효한 세션에서 재개 기록이 소실된 경우는 410이다. 410을 새로운 세션이나 원래 POST의 재시도 지시로 해석하지 않는다.

## 접근과 용량

Host는 실제 포트의 127.0.0.1 또는 localhost만 허용한다. Origin이 있으면 정확한 로컬 origin 또는 호스트의 allowedOrigins와 비교하며 불일치는 403이다. CLI의 `--origin`은 정확한 추가 HTTP(S) origin을 등록한다. 허용된 Origin의 OPTIONS에는 제한된 CORS preflight를 응답하며, 실제 요청의 bearer 인증은 별도로 적용한다. 인증 헤더·세션·버전·Origin 등의 중복 값, 비정상 UTF-8/JSON, 잘못된 Content-Type/Accept를 거부한다.

| 전송 한도 | 기본값 |
|---|---:|
| 전체 세션 / 일반 활성 SSE | 32 / 64 |
| 세션별 일반 보존 스트림 | 64 |
| 제어 활성 SSE / 세션별 제어 보존 스트림 | 4 / 4 |
| 대기 TCP 연결 | 64 |
| POST 본문 / 세션별 재개 데이터 | 8 MiB / 16 MiB |
| 세션별 이벤트 / 호스트 수신 알림 | 1,024 / 128 |
| 완료 스트림 보존 / 유휴 세션 | 60초 / 10분 |
| HTTP 읽기·쓰기 제한 / heartbeat / retry | 5초 / 15초 / 1초 |

SSE 외에 네 개의 HTTP worker 여유를 두어 취소·역방향 응답·DELETE를 처리한다. 세션·스트림 한도는 429다. 오래된 완료 스트림은 보존 기한 또는 용량 압박에서 제거한다. 이벤트 데이터는 세션 전체에서 오래된 순으로 제거하며, reader가 뒤처지면 누락 데이터를 건너뛰지 않고 연결을 종료한다. 유휴 만료는 reader와 미완료 요청이 모두 없는 세션에 적용한다. 취소가 끝나지 않은 호스트 작업은 종료 합류와 세션 용량 회수를 지연시킨다. 새 정리 스레드를 만들 수 없는 자원 오류에서는 닫힌 세션을 계속 용량에 산입하고 서버 close에서 합류한다.

TLS listener·원격 proxy 배치·OAuth·legacy SSE·2026 규격·tasks·자동 앱 발견은 이 구현의 지원 범위에 포함되지 않는다. 실제 Society/Dreamscapes 제품 등록·배포도 별도 작업이다. 표준 기준은 [MCP 2025-11-25 전송 규격](https://modelcontextprotocol.io/specification/2025-11-25/basic/transports)이며, 검증 결과는 [Verification.md](Verification.md)에 기록한다.

## 0.9.0 MCP 연결 관리와 도구 검색

호스트가 지정한 MCP 설정 파일의 연결·복구, 대화별 `ToolSearch`, 인증된 `agent.mcp.status` 및 CLI 옵션을 추가했다. 설정과 실행 권한, 수명 및 미지원 범위는 [ToolDiscovery.md](ToolDiscovery.md)를 참조한다.

## 0.28 제어 스트림 용량

maxStreams/maxStreamsPerSession은 일반 활성·보관 스트림 한도이다. maxControlStreams(기본 4)는 별도 전역 활성·연결별 보관 제어 스트림 한도이며 등록된 controlHandlers와 ping에 적용한다. 일반 용량 포화 중에도 제어 응답을 처리한다. 재접속은 원래 분류를 유지하고 일반 요청이 섞인 2025-03-26 배치는 일반 용량을 사용한다. CLI 설정·검증 및 연결 큐 등 남은 자원 제한은 [ControlCapacity.md](ControlCapacity.md)를 따른다.
