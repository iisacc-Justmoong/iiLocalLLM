# WebFetch (0.42.0)

C++ `WebFetch`는 익명 HTTP(S) 페이지를 조회하고 HTML을 Markdown으로 변환한 뒤 기존 `Model`로 요청한 정보를 추출한다. Engine의 로컬 모델, 인증 HTTP/native IPC, MCP와 thin CLI에서 같은 구현을 사용한다. 학습 프레임워크나 원격 LLM 계정은 필요하지 않다. `WebSearch`는 별도 미구현 항목이다.

## 등록과 호출

임베디드 호스트는 `EngineOptions::webFetchEnabled=true`로 활성화한다. `webFetch` 옵션의 model을 생략하면 해당 대화의 모델을 사용한다. 도구 입력은 `{"url":"https://example.com/","prompt":"Summarize the product information"}` 두 필드뿐이다. 헤더·인증·프록시·모델·사설망 허용 설정을 도구 입력으로 받을 수 없다. 도구는 읽기 전용이며 기본 지연 공개 대상이다. `WebFetch::tool()`을 기존 registry에 직접 등록할 수도 있다. C++ `fetch()`는 신뢰된 호스트용이며, 모델 요청은 반드시 ToolRunner를 거쳐 정책과 훅을 적용한다.

daemon은 기본 활성화하며 `--agent-no-web-fetch`로 끈다. `--agent-web-model MODEL`로 별도 로컬 추출 모델을 선택할 수 있다. `--agent-web-private-origin ORIGIN`은 호스트가 신뢰하는 정확한 scheme/host/port 조합에만 사설망 접속과 HTTP를 허용한다. 반복할 수 있고 경로·쿼리·fragment는 허용하지 않는다. 모델별 지원이나 권한이 다른 앱은 기존 API 호스트 분리 계약을 따른다.

`agent.info.web_fetch_enabled`로 가용성을 확인한다. `agent.web.fetch`는 `session_id`, `url`, `prompt`를 받으며 `text`, `result`, `is_error`를 반환한다. 인증된 앱의 세션만 사용한다. 부모 transcript에 메모를 추가하지 않으며 기존 취소·기한·종료 수명을 따른다. C++ `Engine::runWebFetch`도 같은 경로다.

```sh
iillm --auth-file private/token agent web fetch SESSION request.json
```

에이전트 MCP는 `WebFetch`와 `iisacc/webFetch` capability (`iisacc.web-fetch/1`)를 노출한다. 연결별 호스트 세션을 사용하고 호출자가 세션을 지정하지 못한다. CLI 옵션은 `--no-web-fetch`, `--web-model`, `--web-private-origin`이다. 로컬 추출 모델이 필요한 기능이므로 `--model` 없는 MCP에는 등록하지 않는다. API와 MCP의 도구 오류도 확인해야 하며 전송 성공을 조회·추출 성공으로 간주하지 않는다.

WebFetch는 네트워크/모델을 기다리는 동안 MCP의 일반 앱 실행 잠금을 점유하지 않는다. 자체 동시 실행 한도와 캐시 잠금으로 수명을 관리하므로 독립된 앱 변경 도구와 취소 제어를 계속 처리할 수 있다.

## 권한과 네트워크

읽기 전용 도구의 일반 기본 허용에서 WebFetch를 제외한다. `WebFetch(domain:example.com)`은 정규화한 정확한 DNS 호스트만 허용한다. 대소문자와 IDNA는 정규화하지만 서브도메인·접미사·와일드카드로 권한을 넓히지 않는다. 명시적 deny, ask, allow 순서, DontAsk와 Bypass, 파일 기반 설정 계층, 입력 변경 훅과 호스트 재검증은 기존 정책을 따른다. 내장 사전 승인 도메인은 없다.

```sh
iiLocalLLMD ... --agent-allow 'WebFetch(domain:example.com)'
```

공개 HTTP URL은 HTTPS로 올린다. 익명 GET만 수행하고 쿠키·Authorization·환경 프록시를 사용하지 않는다. URL 자격증명과 HTTP(S) 이외 scheme은 거절한다. DNS 응답의 모든 주소를 검사한 후 검증한 IP에 접속을 고정한다. Host와 TLS 검증 이름은 원래 호스트를 유지한다. 사설·루프백·링크 로컬·멀티캐스트 등 비공개 주소는 정확한 호스트 설정 없이는 거절한다. 인증서 검증은 항상 켜져 있고 C++ 호스트가 CA 설정을 지정할 수 있다.

같은 origin의 301/302/303/307/308만 자동 추적한다. 다른 호스트뿐 아니라 포트·scheme·www 변경도 `redirect_url`을 돌려주고 새 호출의 권한 판정을 요구한다. 다른 origin에 실제 요청을 먼저 보내지 않는다. 전체 리디렉션 묶음에 단일 네트워크 기한을 적용한다. HTTP 오류·연결 중단·인증서 오류·크기 초과는 도구 실패다.

## 내용·캐시·모델

HTML5 토큰화·복구·DOM과 문자 인코딩은 해시 고정된 Lexbor 3.0.0을 사용한다. BOM, HTTP charset, HTML의 처음 1,024바이트 메타 인코딩 순으로 판정하며 나머지는 UTF-8이다. 헤딩·문단·목록·강조·링크·이미지 대체 텍스트·코드 블록·표의 내용을 Markdown으로 만든다. 상대 링크를 최종 URL 기준으로 해석한다. script/style/head/template/noscript와 hidden 요소를 제외한다. CSS 레이아웃이나 JavaScript를 실행하는 브라우저는 아니다. 복잡한 중첩 목록/표의 원래 모양을 완벽하게 재현한다고 주장하지 않는다.

캐시는 네트워크로 받은 내용만 저장한다. 프롬프트별 모델 응답은 저장하지 않으며 캐시 적중에서도 현재 추출 요청으로 모델을 호출한다. 같은 인스턴스 안에서도 소유 세션별로 분리한다. API 앱은 별도 Engine과 캐시를 갖는다. TTL 만료·LRU 퇴출·호스트 `clearCache()`를 제공하며 실패/리디렉션/바이너리는 캐시하지 않는다.

추출 모델에는 페이지와 추출 요청을 분리한 JSON 및 호스트 지시만 전달한다. 부모 system prompt·대화·다른 파일·도구 목록은 전달하지 않는다. thinking과 tool choice를 끄고 도구 호출을 반환하면 실패한다. 페이지 본문이 지시나 권한을 부여하지 않는다는 계약을 명시하지만 모든 모델의 프롬프트 삽입 내성을 보장하지는 않는다. 네이티브 측정이 있으면 출력 예산을 문맥의 최대 1/4로 제한하고 본문을 줄여 입력을 맞춘다. 잘림은 모델 입력과 반환 `truncated`에 함께 표시한다. 측정이 없는 사용자 어댑터에는 설정된 문자 상한을 적용한다.

`result`는 모델 응답이며 URL·최종 URL·HTTP 상태·원본 byte 수·content type·원본 SHA-256·소요 시간·cached·truncated·추출 usage를 함께 반환한다. 사용량은 도구 결과에 기록하고 본 대화 모델의 RunUsage와 합산하지 않는다. 원격 문서의 지시를 실행할 도구나 권한을 추출 모델에 주지 않는다.

PDF·이미지·음성 등 바이너리는 현재 소유자의 artifactsDirectory에 UUID 이름, 형식별 확장자, 소유자 읽기/쓰기 권한으로 원자 저장한다. 경로와 크기·SHA-256을 반환한다. 바이너리를 UTF-8 본문으로 오인해 모델에 요약시키지 않는다. 내용 분석은 해당 파일 형식을 지원하는 읽기 기능의 별도 구현 대상이다. 저장 경로는 호스트가 공급하며 모델 입력으로 바꿀 수 없다.

## 기본 상한과 수명

| 항목 | 기본값 |
|---|---|
| URL / 추출 요청 | 2,000 / 16,000 UTF-16 코드 단위 |
| 네트워크 / 추출 기한 | 각각 60초 |
| 응답 / Markdown | 10 MiB / 100,000 UTF-16 코드 단위 |
| HTML 토큰 / 순회 노드 / 깊이 | 각각 200,000 / 200,000 / 256 |
| 추출 최대 토큰 / 응답 byte | 2,048 / 400,000 |
| 리디렉션 / 인스턴스 동시 호출 | 10 / 2 |
| 캐시 TTL / byte / 항목 수 | 15분 / 50 MiB / 128 |

문자 잘림은 surrogate pair를 나누지 않는다. 응답 크기는 디코딩된 네트워크 body에도 적용하며 압축 안전 검사도 유지한다. 병렬 호출이 상한에 닿으면 대기열을 추가하지 않고 ResourceLimit을 반환한다. 네트워크 조회, HTML chunk/토큰 처리, 모델 스트림과 종료는 취소를 확인한다. 모델 어댑터는 취소에 협조해야 하며, 이미 저장된 artifact는 취소로 삭제하지 않는다. 브라우저 렌더링·쿠키 로그인·기업 프록시·원격 도메인 평판 서비스·일반 MIME/멀티모달 분석은 이 기능의 완료 증거에 포함하지 않는다.

## 참조와 의존성

참조 고정 커밋 `c8cd253554319f32ff64ff7000636199f720c9bc`의 `source/src/tools/WebFetchTool/{WebFetchTool.ts,utils.ts,prompt.ts}`를 대조했다. url/prompt, 도메인 규칙, 지연 공개, HTML 변환, 15분/50MiB 캐시, HTTP 업그레이드와 별도 모델 추출에 대응한다. 전용 Anthropic 도메인 조회와 Haiku는 각각 로컬 주소 정책과 `Model` 인터페이스로 교체했다. 참조의 www 자동 허용보다 origin을 엄격히 구분하며, 같은 호스트의 반복 요청에 개별 기한 대신 묶음 기한을 적용한다. 참조처럼 바이너리를 문자로 잘못 해석하는 경로는 제공하지 않는다. 공급자별 사전 승인 문서 원문 반환과 완전한 터미널 UI는 남아 있다.

외부 라이브러리 검토: Qt Core/Network는 기존 의존성을 유지한다. HTML 정규식 구현이나 GUI QTextDocument 대신 [Lexbor 3.0.0](https://github.com/lexbor/lexbor/releases/tag/v3.0.0)의 유지보수 중인 HTML5 파서를 사용한다. 원본 5,777,325바이트 archive SHA-256은 `eafaa79ef9871f0bbb1978eda8677d184f7ecdcaa203d7cd25b3f86e32c014c2`이다. core/dom/ns/tag/html/encoding과 플랫폼 메모리/파일 모듈만 비공개 object로 빌드한다. 추가 실행 라이브러리나 파서 공개 ABI는 없다. Apache-2.0의 LICENSE와 NOTICE를 설치한다. 선언·변환만 Qt 타입을 공개하므로 파서 교체는 C++ 소비자 인터페이스와 분리된다.

검증은 `tests/web_fetch_tests.cpp`, `web_fetch_tls.py`, 실제 전송 및 모델 fixture와 설치 소비자로 수행하며 최종 수치는 [Verification.md](Verification.md)에 기록한다. 전체 하네스·WebSearch·모든 앱과 플랫폼의 완료 선언은 아니다.
