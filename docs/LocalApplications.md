# 실행 중인 로컬 앱과 MCP 연결

0.10.0은 실행 중인 데스크톱 앱이 인증된 MCP HTTP 주소를 게시하고 iiLocalLLM이 자동 발견하는 C++ 경로를 제공한다. 같은 기기의 Society·Dreamscapes 컨트롤러에 연결한다. 서버 구현은 기존 Qt와 cpp-httplib를 재사용하며 새 런타임 의존성을 추가하지 않는다.

## 발견과 실행 권한

앱은 `mcp::LocalApplicationServer`로 `127.0.0.1`의 임시 포트에서 `/mcp`를 열고, 초기화·도구 목록·호출에 기존 MCP 서버를 사용한다. `listen()` 성공 후에만 등록 파일이 보인다. 호스트는 `agent::McpConnections`의 `localApplicationsDirectory`를 설정한다. C++ 옵션의 기본값은 비활성이고, `iiLocalLLMD`의 agent API 및 `iillm-mcp`에서는 데스크톱 POSIX일 때 기본 활성이다.

등록 위치의 우선순위는 `--agent-apps-dir` / `--apps-dir`, 환경변수 `IILOCALLLM_APP_ENDPOINTS`, Qt `GenericDataLocation/iisacc/AgentEndpoints`이다. 환경변수와 C++ 경로는 절대 경로를 요구한다. CLI의 상대 경로는 실행 디렉터리에서 해석한다. macOS의 일반 기본 경로는 `~/Library/Application Support/iisacc/AgentEndpoints`이다. 종료하려면 데몬의 `--agent-no-apps`, MCP 실행기의 `--no-apps`를 사용한다. 명시 경로와 비활성 옵션은 함께 사용할 수 없다. 제품 앱의 서버는 `IILOCALLLM_DISABLE_APP_MCP=1`로 끈다.

`McpConnections`는 기본 250ms마다 등록 파일과 연결을 갱신한다. 설정 파일은 생성과 명시적 `reload()`에서만 다시 읽는다. 자동 발견 파일로 명령·인자·환경변수를 공급하거나 프로세스를 실행할 수 없다. 명시 설정 서버와 앱을 합쳐 기본 32개로 제한하고, 명시 설정이 우선한다. 앱 종료·레코드 변경 때 이전 연결을 닫고 도구를 제거하여 이전 스냅샷의 호출도 실패시킨다.

자동 갱신은 새 도구 목록을 게시한 뒤 레지스트리·상태 잠금 밖에서 이전 연결을 닫는다. 완료 검증은 목록 제거와 이전 연결 종료를 각각 기다린다. 이 순서는 네트워크 종료 중 레지스트리 잠금을 잡지 않도록 한다.

자동 발견은 도구 실행 허가가 아니다. 가져온 도구의 서버 annotation은 기본 신뢰하지 않으며, 호스트의 일반 권한 정책을 통과해야 한다. 검색은 기본 지연 공개이고 `ToolSearch`로 선택한다. 작은 Qwen2.5 0.5B 모델의 검색 후 연속 실행 검증은 아직 실패하므로 이 모델의 검증된 경로는 `--agent-mcp-eager` / `--mcp-eager`이다. [도구 검색 계약](ToolDiscovery.md)을 함께 적용한다.

호스트의 상태 API `agent.mcp.status` 및 `iillm agent mcp`에는 `source: "local_application"`, `app_id`, 서버 이름, 연결 상태, 도구 수만 추가한다. 인증 토큰·주소·원격 오류 원문은 상태에 포함하지 않는다. 가져온 도구 메타데이터에는 `app_id`, `application_instance`, `discovery_source: "local_application"`, `remote_name`이 있다. 일반 이름은 `mcp__app.com.iisacc.society.<instance>__status` 형태이며 긴 이름은 기존 이름 길이 규칙에 따라 해시로 줄인다. 문자열을 직접 조합하기보다 레지스트리의 실제 이름과 메타데이터를 사용한다.

## 등록 파일과 신뢰 경계

레코드는 `iisacc.mcp-app/1` 스키마의 JSON이며 아래 8개 필드만 허용한다.

| 필드 | 값 |
|---|---|
| `schema` | `iisacc.mcp-app/1` |
| `app_id` | 영문자로 시작하는 1~80자의 영문·숫자·점·하이픈 |
| `name`, `version` | 각각 최대 128자·64자, 제어 문자 제외 |
| `instance_id` | 정규 소문자 UUID, 파일 이름은 `<instance_id>.json` |
| `pid` | 살아 있는 프로세스의 양의 정수 PID |
| `endpoint` | `http://127.0.0.1:<port>/mcp`, 사용자 정보·query·fragment 제외 |
| `bearer_token` | 시스템 난수 32바이트를 URL-safe Base64로 인코딩한 43자 비밀값 |

디렉터리는 현재 effective UID 소유이고 다른 사용자 권한이 없어야 한다. 생성 모드는 0700이며 기존의 공개 디렉터리를 임의로 chmod하지 않고 거절한다. 파일은 같은 UID의 비공개 일반 파일, 링크 수 1개, 최대 64KiB여야 한다. 최종 디렉터리·파일 심볼릭 링크와 FIFO를 거절한다. 디렉터리 descriptor와 `openat/O_NOFOLLOW`로 파일을 읽고 한 번에 최대 4096개 디렉터리 항목을 검사한다. 결과의 `rejectedRecords`, `staleRecords`, `truncated`, `error`로 직접 호출자가 제한·거절을 확인할 수 있다.

임시 파일을 0600으로 작성·fsync한 뒤 고유 이름으로 게시한다. 정상 `close()`는 등록한 inode가 그대로일 때만 파일을 제거한다. 비정상 종료의 레코드는 PID 검사로 제외하며, 다시 시작한 앱은 새 UUID와 토큰을 사용한다. 디스크의 오래된 레코드를 자동 삭제하지는 않는다.

이 경계는 같은 OS 사용자에 대한 신뢰이다. 앱 ID와 PID는 코드 서명이나 프로세스 소유권 증명이 아니며 PID가 재사용될 수 있다. 같은 사용자 권한을 가진 프로그램은 등록 파일을 읽거나 다른 앱을 사칭할 수 있다. 따라서 모델이 쓰는 workspace와 등록 폴더를 분리한다. 실제 접속 시 토큰 인증과 MCP 초기화가 다시 수행되고, HTTP 서버의 Host/Origin 검사도 유지된다. [MCP 보안 지침](https://modelcontextprotocol.io/docs/2025-11-25/tutorials/security/security_best_practices)이 로컬 HTTP에도 권고하는 인증을 적용한다. 전체 OAuth·원격 발견·코드 서명 기반 신뢰는 이 구현 범위 밖이다.

## QObject 컨트롤러 호출

`agent::objectTool(owner, definition, handler, timeoutMs)`는 Qt 주 스레드에서 생성하며 owner도 그 스레드에 있어야 한다. HTTP 작업 스레드의 호출은 `QMetaObject::invokeMethod(..., Qt::QueuedConnection)`로 앱 주 스레드에 보낸다. 주 스레드의 직접 호출은 즉시 실행한다. 삭제된 owner는 `RuntimeUnavailable`로 실패한다. [Qt의 queued 호출 계약](https://doc.qt.io/qt-6.8/qmetaobject.html)을 사용한다.

기본 제한은 10초이다. 실행 전에 취소·시간 초과된 대기 요청은 나중에도 실행하지 않는다. 이미 컨트롤러 호출이 시작된 경우 취소가 효과를 되돌리지는 않는다. 시간 초과 결과의 `dispatch_started: true`는 효과가 발생했을 수 있음을 뜻한다. 자동 재시도하지 말고 상태·작업 ID를 조회한다. 직접 주 스레드 호출과 이미 실행 중인 handler를 선점하지 않는다. handler는 상태 조회나 비동기 작업 제출처럼 짧게 수행해야 한다.

서버를 먼저 닫은 뒤 컨트롤러와 `QCoreApplication`을 파괴한다. 서버 종료가 대기 호출의 취소를 전달하므로 GUI 스레드가 HTTP 작업 스레드의 완료를 기다릴 때 교착하지 않는다. [설치 consumer](../tests/consumer/local_applications.cpp)는 공개 헤더만으로 등록·발견·HTTP·QObject 전달·연결 제거를 실행한다.

## 제품 앱 범위와 검증

MCP 도구 결과의 `structuredContent`는 모델이 읽을 실제 데이터이다. 서버 bridge는 이 JSON을 text content에도 넣고, 클라이언트 어댑터는 상대 서버가 설명 문장만 반환해도 구조화된 값을 모델용 텍스트에 보존한다. 이미 동일한 JSON 텍스트가 있으면 중복하지 않는다. host용 `_meta`를 모델 텍스트로 변환하지 않는다. [MCP 구조화 결과 규격](https://modelcontextprotocol.io/specification/2025-11-25/server/tools#structured-content)의 텍스트 호환 권고를 따른다.

| 앱 | 실제 컨트롤러의 도구 |
|---|---|
| Society | `status`, `open_section`, `navigate`, `refresh`, `list_entries` |
| Dreamscapes | `status`, `models`, `jobs`, `select_model`, `refresh_models`, `generate`, `cancel` |

Society는 현재 컨테이너의 탐색과 제한된 디렉터리 목록을 제공한다. Dreamscapes는 로컬 모델과 실제 생성 큐를 조작한다. 동기화는 Society가 소유하며 Dreamscapes에 원격 추론·Sync 서버를 추가하지 않는다. 새 HTTP listener는 데스크톱 POSIX에서만 컴파일한다. iOS·Android 앱과 Windows 서버의 발견·게시 경로는 미구현이다. Congregation·Thinking Space도 아직 연동하지 않았다.

SDK의 `iiLocalLLM.local_applications`는 인증·비공개 파일·원격 주소/명령/FIFO/오래된 PID 거절·자동 갱신·토큰 비노출·컨트롤러 수명·취소·종료를 검증한다. 제품의 `Society.Mcp`와 `Dreamscapes.Mcp`는 실제 실행 파일을 별도 컨테이너에서 띄운다. Dreamscapes 생성 테스트는 기존 PNG 생성 프로토콜 fixture를 사용하므로 앱 제어·큐·결과 파일의 증거이며 실제 모델의 생성 품질 증거가 아니다. 실행 결과와 남은 실패는 [Verification.md](Verification.md)에 기록한다.
