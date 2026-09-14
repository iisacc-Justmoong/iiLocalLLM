# MCP 연결 설정과 대화별 도구 검색 (0.9.0)

`agent::McpConnections`는 호스트가 지정한 설정 파일의 stdio 또는 Streamable HTTP 서버를 기존 C++ MCP 클라이언트로 연결한다. `agent::Engine`은 `deferred` 도구의 전체 스키마를 첫 모델 요청에서 제외하고, 내장 `ToolSearch`의 성공 결과에 따라 다음 모델 턴에 공개한다. 구현은 C++이며 Qt Core/Network, 기존 MCP 클라이언트와 jsoncons 검증기를 재사용한다. 추가 생산 런타임 의존성은 없다.

## 설정과 실행

설정의 루트는 `mcpServers` 객체이다. 다음은 사용 형식을 설명하는 예시이며 Society 또는 Dreamscapes의 실제 배포된 주소·명령을 뜻하지 않는다.

```json
{
  "mcpServers": {
    "documents": {
      "command": "/absolute/path/to/document-mcp",
      "args": ["--workspace", "${DOCUMENT_ROOT}"],
      "env": {"APP_PROFILE": "${PROFILE:-default}"},
      "appId": "com.example.documents"
    },
    "images": {
      "type": "http",
      "url": "http://127.0.0.1:51234/mcp",
      "headers": {"Authorization": "Bearer ${IMAGE_MCP_TOKEN}"},
      "appId": "com.example.images"
    }
  }
}
```

```sh
./build/iiLocalLLMD --socket /absolute/private/service.sock \
  --agent-workspace /absolute/workspace --agent-state /absolute/private/agent \
  --agent-credentials /absolute/private/clients.json \
  --agent-mcp-config /absolute/private/mcp.json --agent-allow 'mcp__documents__*'
./build/iillm --socket /absolute/private/service.sock \
  --auth-file /absolute/private/app-token agent mcp
./build/iillm-mcp --workspace /absolute/workspace \
  --mcp-config /absolute/private/mcp.json --allow 'mcp__documents__*'
```

`--agent-mcp-config` 및 `--mcp-config`는 반복할 수 있다. CLI 상대 경로는 실행 당시 cwd를 기준으로 절대 경로화한다. C++ `configFiles`의 상대 경로는 `workingDirectory`를 기준으로 한다. 뒤 파일의 같은 이름 서버 정의가 앞 정의 전체를 대체하며 필드 단위 병합은 하지 않는다. `--agent-mcp-project` / `--mcp-project`를 지정하면 선택한 workspace의 `.mcp.json`을 마지막 우선순위로 읽는다. 옵션 없이 프로젝트 파일을 찾아 프로세스를 실행하지 않는다.

| 필드 | 계약 |
|---|---|
| `type` | 생략하면 stdio. `http`, 별칭 `streamable-http` 지원. legacy `sse`, WebSocket은 미지원 |
| `command`, `args`, `env` | stdio 전용 문자열·문자열 배열·문자열 값 객체. shell을 거치지 않는 `QProcess` 실행 |
| `cwd` | 기본값은 호스트 workspace. 상대 경로도 workspace 기준이며 실제 디렉터리가 있어야 함 |
| `url`, `headers` | HTTP 전용. URL 사용자 정보·fragment 거부. 기본적으로 HTTPS, loopback HTTP 허용 |
| `appId` | 호스트가 부여한 검색용 앱 식별자. 설치·서명·계정 소유권을 검증한 결과가 아님 |
| `disabled` | true면 연결과 도구 등록을 생략. `{ "disabled": true }`만으로 상위 정의를 비활성화 가능 |
| `alwaysLoad` | 해당 서버 도구를 처음부터 공개. 실행 권한을 부여하지 않음 |

`command`, `args`, `env` 값, `cwd`, `url`, 헤더 값의 `${VAR}` 및 `${VAR:-default}`를 호스트가 제공한 `QProcessEnvironment`로 한 번 치환한다. 변수는 설정 파일의 다른 `env` 항목을 참조하지 않는다. 변수가 존재하면 빈 문자열도 그 값으로 사용한다. 변수와 기본값이 모두 없으면 설정을 거부한다. `$()`, backtick, `$VAR`는 실행·치환하지 않는다. 호스트가 명시적으로 shell을 command로 설정하면 그 shell의 실행 계약이 적용된다.

파일당 1 MiB, 최대 64개 파일, 기본 32개 서버(호스트 설정 상한 256), 문자열당 65,536자, args/env 각 256개, headers 128개를 제한한다. 실제 HTTP 전송은 기존 클라이언트의 더 작은 헤더 크기·예약 헤더 제한도 적용한다. 모든 파일의 JSON 구조와 지원 필드를 파싱한 뒤 연결을 시작한다. 구문·타입·미정의 변수 오류는 기존 등록 상태를 유지하면서 실패한다. 연결·협상·스키마 오류는 서버별 `failed`로 격리한다. 기본 초기화 기한은 서버당 10초이며 초기 연결은 순차적으로 진행한다.

## C++ 호스트와 수명

```cpp
#include <agent/McpConnections.h>
#include <agent/Engine.h>

auto registry = std::make_shared<iiLocalLLM::agent::ToolRegistry>();
iiLocalLLM::agent::McpConnectionOptions options;
options.workingDirectory = workspace;
options.configFiles = {configPath};
auto connections = std::make_shared<iiLocalLLM::agent::McpConnections>(registry, options);
// 같은 registry를 Engine 또는 agent::Api에 전달한다.
// ApiOptions::mcp = connections이면 인증된 agent.mcp.status에서 상태를 조회한다.
```

관리자는 설정된 도구 이름을 독점 소유한다. 같은 registry의 해당 이름을 다른 호스트 코드가 제거·교체하지 않아야 한다. `ToolRegistry::replace`는 새 스키마 전체를 검증한 후 기존 관리 도구 집합을 원자적으로 교체한다. 다른 호스트 도구와 충돌하면 기존 registry를 유지하고 실패한다. 변경 없는 주기 검사에서는 스키마를 다시 컴파일하지 않는다.

기본 250ms 주기의 `refresh()`는 이미 읽은 서버 설정으로 도구 목록 변경 알림과 연결 세대를 확인한다. 종료된 연결·실패한 목록 요청은 기본 1초 간격으로 재시도한다. HTTP 세션이 재초기화 중이면 같은 클라이언트를 기다린다. `Client::isClosed()`는 최종 종료 여부이며, `isConnected()==false`만으로 최종 종료를 판정하지 않는다. 도구 호출 POST는 복구 과정에서 재실행하지 않는다.

`reload()`만 파일을 다시 읽는다. 동일한 유효 설정은 연결을 유지하고 목록을 갱신한다. 교체·삭제·비활성화한 연결은 닫는다. 이전 턴의 registry snapshot은 정의와 핸들러를 보존하지만 닫힌 연결의 실행은 실패한다. 같은 연결의 목록 변경 시에는 다음 턴에 새 목록을 사용하며, 이미 진행 중인 호출 결과를 소급 취소하지 않는다. `refreshIntervalMs=0`이면 호스트가 갱신을 직접 구동한다. 파일 변경은 자동 실행 설정 변경으로 이어지지 않는다.

생성·reload·refresh·close는 블로킹 API이다. 앱 UI 스레드 밖에서 실행하며, close는 관리 스레드를 깨우고 연결을 닫아 진행 중 요청을 중단한 후 스레드를 종료한다. 호스트가 전달한 취소 토큰 자체는 변경하지 않는다. 초기화 중 종료는 클라이언트의 초기화 기한 안에서 완료된다. 인증·역방향 요청 콜백 안에서 관리자의 reload/refresh/close로 재진입하지 않아야 한다. 호스트 콜백은 빠르게 반환하거나 전달된 취소 토큰에 협조해야 한다.

`client(server)`는 연결된 클라이언트 객체에 접근하여 리소스·템플릿·프롬프트·구독을 사용할 수 있게 한다. 일시적인 단절 여부는 클라이언트 상태로 확인한다. `clientOptions(server)` 콜백으로 roots와 sampling/elicitation 핸들러·capabilities를 제공한다. 원격 요청이 임의로 새 호스트 기능을 켜지는 않는다. `takeNotifications()`는 제한된 큐에서 서버 이름과 원격 알림을 전달하며, 그 본문은 지침이 아닌 신뢰하지 않는 데이터이다. 호스트가 관리자의 알림 처리와 별개로 같은 Client의 알림 큐를 직접 비우면 목록 변경 알림을 가져갈 수 있으므로 관리 중에는 관리자 큐를 사용한다.

설정의 `Authorization: Bearer …`는 기존 HTTP 인증 콜백으로 변환한다. C++ `bearerToken(server)`이 있으면 설정의 토큰보다 우선하여 매 HTTP 교환에서 현재 토큰을 조회한다. 인증 값·헤더·명령·인수·환경·URL·원격 instructions·예외 본문은 상태 응답에 포함하지 않는다. 다른 인증 scheme은 미지원이며 OAuth 발견·로그인·갱신은 호스트 책임이다. JSON 필드로 `trustAnnotations`나 원격 HTTP 허용을 켤 수 없다.

## ToolSearch와 실행

기본적으로 관리 MCP 도구는 `deferred=true`이다. 미공개 도구가 있으면 모델은 다음 형태의 내장 도구를 받는다.

```json
{"name":"ToolSearch","arguments":{"query":"+documents files","max_results":5}}
```

정확한 이름, `select:name1,name2`, `mcp__server` 접두사, 일반 키워드 및 `+필수단어`를 지원한다. 이름의 단어·부분 일치, 앱/서버 식별자와 `search_hint`, 설명 순으로 점수를 더하고 동점은 이름으로 정렬한다. 대소문자를 접고 CamelCase·Unicode 단어를 처리한다. 키워드 검색은 deferred 카탈로그를 대상으로 하며 직접 선택은 이미 공개된 도구에도 사용할 수 있다.

반환 `data`에는 `matches`, 전체 정의인 `tools`, 직접 선택에서 찾지 못한 `missing`, `total_deferred_tools`가 들어간다. `metadata.iilocal.tool_search`는 선택된 이름과 정의 SHA-256을 저장한다. 모델에 주는 `text`에는 검색된 이름과 아직 작업이 실행되지 않았다는 안내를 담는다. 전체 스키마는 다음 모델 요청의 tools에도 실리므로 본문에 중복하지 않는다. 쿼리는 1..4096자, 순수 `searchTools` 결과 수는 1..100개, Engine 기본 결과 수 상한은 20개, 실제 검색 기본값은 5개이다. 결과 JSON은 4 MiB 이하여야 한다.

선택 상태는 전역 registry를 바꾸지 않고 대화 원본 JSONL에서 복구한다. 기본 최대 64개 deferred 도구를 최근 선택·사용 순서로 유지한다. 새 대화와 앞부분만 fork한 대화에 뒤쪽 검색 결과가 유입되지 않는다. 요약으로 모델에 보이는 이전 검색 결과가 줄어도 원본에서 선택을 복구한다. 이름·스키마·정책 메타데이터·관리 연결 식별자·연결 세대가 바뀌면 이전 선택은 무효가 된다.

한 모델 응답에 ToolSearch와 아직 공개되지 않은 도구 호출을 함께 넣어도 두 번째 호출은 실패한다. 도구 정의와 실행 핸들러는 모델 턴 시작 시 함께 고정되며 검색 결과는 다음 턴부터 반영된다. 선택은 기존 정책·스키마 검증·훅을 통과해야 실행할 수 있다. 원격 `readOnlyHint`는 기본적으로 실행 허가 근거가 아니다. 호스트의 `trustAnnotations=true` 또는 명시적 도구 허용 규칙을 별도로 설정해야 한다.

`EngineOptions::toolSearch.enabled=false`는 모든 등록 도구를 처음부터 공개한다. CLI의 `--agent-mcp-eager` / `--mcp-eager`는 관리 MCP 도구에만 eager 표시를 적용한다. deferred 검색이 활성화된 registry의 `ToolSearch` 이름은 내장 도구용으로 예약한다.

## 현재 경계

고정 Qwen2.5 0.5B Q4_K_M 검사에서 ToolSearch 호출과 선택은 성공했지만, 이어서 실제 도구를 호출하지 않고 검색 안내를 최종 답으로 끝내는 실패를 관측했다. 검색 지원의 C++ 계약과 개별 모델의 연속 도구 사용 능력은 다르다. 해당 모델에서 이 실패가 발생하면 `--agent-mcp-eager` / `--mcp-eager` 또는 `deferTools=false`로 도구를 처음부터 제공할 수 있다. 자동 연속 호출을 성공으로 간주하거나 모델 대신 답을 만들어 넣지 않는다. 실제 검증 결과는 [Verification.md](Verification.md)에 기록한다.

MCP/API 설정 연결·협상·도구 검색까지 구현했으며 앱 manifest 탐색, 설치 앱·프로세스·LAN 자동 발견, 사용자/managed/plugin 설정 우선순위 전체, 임베딩 검색, 모델별 자동 토큰 임계값, 공급자 고유 `tool_reference`, legacy SSE, OAuth, MCP tasks는 남아 있다. `iillm-mcp`에서 프록시 도구 목록을 다시 요청하면 현재 registry를 받지만 관리 서버의 목록 변경 알림을 외부 MCP 클라이언트에 자동 중계하지는 않는다. 실제 Society/Dreamscapes 앱의 MCP endpoint와 API 호출까지 완료한 상태는 아니다.

참조 동작은 고정된 Claude Code 2.1.88 분석 자료의 ToolSearch/config 구조이다. 현재 공식 문서의 설정 형식과 환경변수 표기는 [Claude Code MCP 문서](https://code.claude.com/docs/en/mcp), SDK 연결·검색 개념은 [Agent SDK MCP 문서](https://code.claude.com/docs/en/agent-sdk/mcp)와 대조했다. 현재 문서의 미정의 변수 처리, 자동 검색 임계값 및 제품 전체 범위와 동일한 구현이라고 주장하지 않는다. 코드 복제 없이 기존 iiLocalLLM 인터페이스에 맞춰 구현했다.
