# HTTP 훅

0.29.0은 호스트가 설정한 HTTP/HTTPS 엔드포인트에 생명주기 입력을 POST하고 JSON 응답을 기존 C++ 훅 결정 경로에 연결한다. `agent::CommandHooks`가 명령과 HTTP 설정을 함께 소유한다. daemon의 인증 API·native IPC, MCP HTTP·stdio, 임베디드 ToolRunner·Engine에서 동일하게 적용한다. Python 서버는 독립 검증용이며 생산 실행기는 C++이다.

기존 Qt 6.8.3 Network의 QNetworkAccessManager·QHostInfo·TLS를 사용한다. 새 생산 라이브러리는 추가하지 않았으며 Qt의 기존 배포·라이선스 조건을 따른다. 참조의 Axios·Node 실행기를 추가하지 않는다.

## 설정과 데이터 흐름

```json
{
  "allowedHttpHookUrls": ["https://hooks.example.com/iilocal/*"],
  "httpHookAllowedEnvVars": ["HOOK_TOKEN"],
  "hooks": {
    "PreToolUse": [{
      "matcher": "Write|Edit",
      "hooks": [{
        "type": "http",
        "url": "https://hooks.example.com/iilocal/check",
        "timeout": 10,
        "headers": {"Authorization": "Bearer ${HOOK_TOKEN}"},
        "allowedEnvVars": ["HOOK_TOKEN"]
      }]
    }]
  }
}
```

예시 도메인은 사용자가 운영하는 실제 엔드포인트로 교체한다. daemon은 `--agent-hooks FILE`, MCP 서버는 `--hooks FILE`을 사용한다. 파일 소유권·위치·크기 검사와 시작 시 고정은 [CommandHooks.md](CommandHooks.md)를 따른다. API·MCP 요청이나 모델 출력으로 URL·헤더·명령을 등록할 수 없다.

HTTP 항목은 type, url, if, timeout, headers, allowedEnvVars, statusMessage, once를 받는다. URL은 유효한 http/https 절대 주소여야 하며 사용자 정보·fragment·NUL을 허용하지 않는다. 매처·if·지원 이벤트와 공통 입력 필드는 명령 훅과 같다. 입력은 UTF-8 JSON 객체이며 기본 Content-Type/Accept는 application/json이다. 도구 입력은 POST 본문에만 들어가고 URL·헤더 템플릿에 치환하지 않는다.

일치한 HTTP 설정의 URL과 if 조건이 같으면 마지막 항목을 실행한다. 헤더·timeout·once가 다르더라도 이 중복 기준은 같다. 조건이 다르면 별도로 실행한다. 스킬·플러그인 출처별 병합과 명령 훅 중복 제거까지 구현한 것은 아니다.

allowedHttpHookUrls가 없으면 URL 패턴 제한을 추가하지 않으며 빈 배열은 모든 HTTP 훅을 막는다. 각 패턴은 전체 URL에 일치해야 하고 `*`만 와일드카드다. URL 허용 실패는 네트워크 요청 전 비차단 오류로 기록한다. httpHookAllowedEnvVars가 있으면 항목의 allowedEnvVars와 교집합만 사용한다. `$NAME`과 `${NAME}`의 이름은 대문자·숫자·밑줄 규칙을 따르며 허용되지 않았거나 없는 변수는 빈 문자열이 된다. CommandHookOptions.environment에 캡처한 환경에서 읽는다. CR·LF·NUL은 제거하고 나머지 ASCII 제어 문자는 탭을 제외하고 거부한다.

헤더는 최대 128개, 이름 128자, 이름과 치환된 값의 합계 64 KiB이다. Host·Content-Length·Transfer-Encoding·Connection·Proxy-Authorization·Proxy-Connection·Upgrade·TE·Trailer는 전송기가 소유하므로 설정에서 거부한다. 참조보다 엄격한 차이이다. 임의 헤더명이나 알 수 없는 설정/응답 필드를 무시하지 않고 거부한다.

## 응답과 오류

HTTP 2xx 응답만 결정으로 처리한다. 빈 본문은 `{}`이며 나머지는 올바른 UTF-8 JSON 객체여야 한다. 명령 stdout과 달리 일반 텍스트를 문맥으로 승격하지 않는다. 정상 객체의 입력 변경·추가 문맥·PreToolUse 권한·PermissionRequest 결정·Stop/세션 동작은 기존 공통 응답 계약을 따른다. 입력과 지속 권한을 바꾸는 승인도 [PermissionRequest.md](PermissionRequest.md), [PermissionUpdates.md](PermissionUpdates.md)의 검사를 통과해야 한다.

```json
{"hookSpecificOutput":{"hookEventName":"PermissionRequest","decision":{
  "behavior":"allow",
  "updatedInput":{"path":"approved.txt","content":"Approved content"}
}}}
```

리다이렉트를 따르지 않는다. 비 2xx·DNS/TLS/연결 오류·시간 초과·응답 초과·잘못된 JSON은 결정 없이 비차단 진단으로 처리한다. 반드시 거부해야 하는 정책은 정상 응답의 명시적인 거부 결정을 반환해야 한다. 외부 취소는 Cancelled로 전파한다. 실패한 요청을 애플리케이션에서 재시도하지 않는다. 순차 업로드 장치를 사용해 Qt가 이미 소비한 POST 본문을 다시 전송하지 못하도록 한다. 서버가 이미 수행한 효과는 취소로 되돌릴 수 없다.

`{"async":true,"asyncTimeout":1000}` 응답은 비동기 접수 확인으로만 성공 처리한다. asyncTimeout은 선택 숫자이며 SDK가 후속 작업을 예약하거나 이 값만큼 기다리는 기능이 아니다. 로컬 백그라운드 훅·결과 재수신·asyncRewake는 별도 미구현이다.

HTTP와 명령은 동일 입력으로 병렬 실행하고 공통 결정 병합을 사용한다. PermissionRequest는 먼저 완료한 결정의 behavior·updatedInput·updatedPermissions를 함께 선택한다. HTTP 응답이 명령 응답보다 먼저 완료하면 그 결정 전체가 적용된다. 일반 훅의 거부·중단과 마지막 완료 입력 변경 규칙은 CommandHooks.md를 따른다.

HTTP 진단은 hook_type, url_sha256, http_status, response_bytes, duration_ms, outcome과 오류 코드/설명을 제공한다. 네트워크 실패처럼 응답을 얻지 못하면 상태·크기는 없다. URL·인증 헤더·원시 응답은 기록하지 않는다. 응답이 명시한 systemMessage·추가 문맥·결정 이유는 기존 훅 결과로 전달된다. API/MCP의 활성 표시·hook 이벤트·progress도 그대로 사용한다.

## DNS·TLS·프록시

직접 연결은 DNS의 모든 반환 주소를 검사한 뒤 한 주소로 연결한다. IPv4를 우선하고 없으면 첫 주소를 선택한다. 검사한 IP를 요청 주소에 고정해 연결 시 호스트명을 다시 조회하지 않는다. Host 헤더와 인증서 검증·SNI에는 원래 호스트명을 유지한다. 이 TLS 동작은 실제 독립 HTTPS 서버로 검사한다. Qt의 [setPeerVerifyName](https://doc.qt.io/qt-6.8/qnetworkrequest.html#setPeerVerifyName) 계약을 사용한다.

참조와 같은 차단 범위는 IPv4 0/8, 10/8, 100.64/10, 169.254/16, 172.16/12, 192.168/16과 IPv6 ::, fc00/7, fe80/10이다. IPv4 매핑 IPv6도 같은 IPv4 규칙을 적용한다. 127/8과 ::1은 로컬 훅 서버를 위해 허용한다. 이는 전체 네트워크 격리나 모든 특수 주소 차단을 뜻하지 않는다. 직접 연결에서 선택 주소의 접속이 실패하면 다른 DNS 주소로 자동 순회하지 않는다.

C++ 호스트는 CommandHookOptions.httpSslConfiguration으로 CA·클라이언트 인증서 등 Qt TLS 설정을 제공할 수 있다. VerifyPeer는 전송기가 강제한다. CLI용 추가 CA/mTLS 환경 설정, OS 샌드박스 프록시 자동 연결은 아직 없다. 기본 신뢰 저장소는 Qt 플랫폼 설정을 따른다.

httpProxy를 생략하면 캡처한 환경에서 https_proxy → HTTPS_PROXY → http_proxy → HTTP_PROXY 순서의 첫 값을 사용한다. no_proxy → NO_PROXY는 `*`, 정확한 이름, 선행 점 도메인 접미사, host:port를 지원한다. 대소문자를 구분하지 않으며 쉼표·공백으로 구분한다. 환경 프록시는 `http://` 전송만 지원한다. `https://` 프록시는 RuntimeUnavailable 비차단 진단이며 직접 연결로 우회하지 않는다. HTTPS 대상의 HTTP CONNECT는 Qt에 맡긴다.

C++ 호스트는 httpProxy에 NoProxy·HttpProxy·HttpCachingProxy·Socks5Proxy를 명시할 수 있다. 실제 프록시가 있으면 대상 DNS와 네트워크 접근 판단은 프록시에 맡기고 직접 연결의 주소 차단은 적용하지 않는다. URL 패턴 검사는 계속 적용한다. 로컬 HTTP 프록시와 NO_PROXY는 테스트했으며 CONNECT·SOCKS·mTLS 전체 조합을 인증한 것은 아니다.

## 한도와 검증 경계

timeout은 슬롯 획득 뒤 DNS·연결·POST·응답 읽기를 포함한다. 기본 600000 ms, 최대 3600000 ms이며 설정 JSON은 초 단위다. 슬롯 대기는 별도이고 취소를 확인한다. 기존 maxConcurrentProcesses는 이름을 유지하며 명령과 HTTP를 합친 공유 실행기 한도다. 기본 4, 최대 32이다.

maxInputBytes/maxOutputBytes는 기본 각각 1 MiB이다. HTTP 출력 한도는 읽은 응답 본문에 적용한다. once는 세션·설정 항목별 프로세스 내 기록이다. POST 시작 전에 실패하면 예약을 반환하고 시작 후에는 응답 실패·취소라도 소비한다. 재시작 뒤 복원하지 않는다. 전체 입력/피드백 합계·OS 네트워크 내부 버퍼까지 하나의 예산으로 통합한 기능은 아니다.

이 버전은 macOS Qt 6.8.3에서 검증한다. HTTP 생산 코드는 Qt 경로지만 다른 OS·모바일 실기기 검증은 별도다. CommandHookOptions 공개 구조체가 변경되어 소비자는 0.29 헤더와 라이브러리로 재빌드해야 한다. 이전 바이너리의 ABI 호환을 보장하지 않는다.

참조는 고정 미러 c8cd253554319f32ff64ff7000636199f720c9bc의 schemas/hooks.ts, utils/hooks/execHttpHook.ts, utils/hooks/ssrfGuard.ts, utils/proxy.ts, utils/hooks.ts와 SDK 출력 스키마이다. 미러의 진위나 Claude Code 전체 동작의 독립 인증은 아니다. 프롬프트 훅은 0.30의 [PromptHooks.md](PromptHooks.md)에 별도 기록한다. 전체 생명주기, agent 훅, 비동기 실행기, 설정/스킬/플러그인 병합과 앱 전체 검증은 [HarnessParity.md](HarnessParity.md)의 partial 상태를 유지한다. 실행 증거는 [Verification.md](Verification.md)에 기록한다.
