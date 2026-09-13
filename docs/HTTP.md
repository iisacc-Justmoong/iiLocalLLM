# Native IPC와 localhost HTTP

LocalIpcServer와 HttpApiServer는 같은 Service를 참조한다. 모델 카탈로그·로드 상태·하드웨어 선택·세션 제한·컨텍스트 예산·FIFO는 서비스에 하나만 존재한다. Native IPC는 macOS/Linux에서 Unix Domain Socket, Windows에서 Named Pipe를 사용한다. HTTP는 127.0.0.1의 TCP 포트에서 외부 프로그램·Python·외부 CLI에 텍스트 및 함수 도구 호출 Chat Completions를 제공한다.

```text
생태계 앱/iillm → Native IPC ─┐
                       ├→ Service → ModelManager / SessionManager / Scheduler → Runtime
Python/외부 도구 → HTTP API ──┘
```

## 실행

`models.json`에는 설치된 모델 URI와 컨텍스트·모델 옵션을 넣는다. 패키지 설치와 manifest는 [ModelManagement.md](ModelManagement.md)를 따른다.

```json
{"models":[{"model":"model://qwen3-8b-q4","context_tokens":2048}]}
```

```sh
./build/iilocal-llm-service --models-root "$PWD/build/chat/Models" \
  --config models.json --socket "$PWD/build/llm.sock" --http-port 8080
```

`--http-port 0`은 OS가 선택한 빈 포트를 사용하고 stdout에 `iiLocalLLM HTTP: http://127.0.0.1:<port>`를 출력한다. `--socket`을 생략하면 HTTP만, `--http-port`를 생략하면 Native IPC만 제공한다. Windows의 `--socket`에는 파이프 이름을 지정한다. 두 계층이 동시에 활성화된 경우에도 모델을 두 번 로드하지 않는다. SIGINT/SIGTERM은 두 리스너와 해당 요청을 정리한다.

임베딩 호스트에서도 같은 서비스 객체를 전달한다.

```cpp
iiLocalLLM::Service service(options);
iiLocalLLM::LocalIpcServer ipc(service);
iiLocalLLM::HttpApiServer http(service);
if (!ipc.listen(socketName) || !http.listen(8080)) return 1;
return app.exec();
```

HttpApiServer의 listen은 전송 스레드를 시작하고 반환한다. 해당 HTTP 서버 자체에는 Qt 이벤트 루프가 필요하지 않지만 Native IPC에는 QObject 스레드의 이벤트 루프가 필요하다. listen/close/port/errorString은 직렬로 호출하고 Service가 두 서버보다 오래 살아 있어야 한다. close는 진행 중 HTTP 생성을 취소하고 전송 스레드를 join한다.

## HTTP 계약

| 메서드·경로 | 동작 |
| --- | --- |
| GET /health | 리스너 상태인 {"status":"ok"}. 모델 추론 준비를 보장하지 않는다 |
| GET /v1/models | 현재 로드한 모델을 {"object":"list","data":[...]}로 반환한다. 각 id는 model:// URI이다 |
| POST /v1/chat/completions | messages로 일반 JSON 또는 SSE 응답을 생성한다 |

모델 설치·검증·로드·언로드와 장기 세션 관리는 기존 [Native IPC](IPC.md) 및 C++ API/daemon 설정을 사용한다. HTTP 생성은 설치된 모델 URI 또는 등록 별칭을 받고, 같은 서비스의 Residency Manager가 상주 인스턴스 재사용 또는 자동 로드·LRU 해제를 수행한다. 설치는 models.pull/models.install로 수행한다. `/v1/models`는 IPC의 models.loaded와 같은 로드 상태를 조회한다. IPC에서 언로드하면 HTTP 목록에서도 빠진다.

```sh
curl http://127.0.0.1:8080/v1/chat/completions \
  -H 'Content-Type: application/json' \
  -d '{"model":"model://qwen3-8b-q4","messages":[{"role":"user","content":"Explain KV caching."}],"max_tokens":128}'
```

지원 필드는 model, messages, stream, stream_options.include_usage, max_tokens 또는 max_completion_tokens, temperature, top_p, top_k, seed, stop, n, keep_alive, min_p, typical_p, min_keep, repetition_penalty, repetition_context_size, presence_penalty, frequency_penalty, xtc_probability, xtc_threshold, logit_bias, tools, tool_choice, parallel_tool_calls이다. model은 정규 model://id 또는 서비스 registry에 등록된 별칭을 허용한다. 일반 텍스트 messages는 1~4096개의 role/content 객체이며 content는 비어 있지 않은 문자열이다. 선택적인 첫 system 이후 user/assistant가 번갈아 나오고 마지막은 user여야 한다. 과거 assistant 응답은 재생성하지 않고 입력 이력으로 전달한다. 전체 입력은 ServiceOptions.maxInputCharacters 제한을 따른다.

keep_alive는 숫자 초 또는 ms/s/m/h 문자열이며 생략하면 현재/서비스 정책이다. 0은 이 요청 종료 후 모델을 해제하고, 5m은 마지막 사용 후 5분 유지한다. 요청의 성공·취소·오류 후 임시 세션/KV를 정리하고 모델의 lease를 반환한다. [Residency.md](Residency.md)의 메모리 예산과 LRU 정책을 공유한다.

기본 생성 값은 max_tokens=256, temperature=0.7, top_p=0.9, top_k=40, seed=0, n=1, stream=false이다. HTTP temperature는 0~2, top_p는 0보다 크고 1 이하, top_k는 0~1,000,000, seed는 0~2^32-1 정수이다. 토큰 제한은 1~1,048,576 정수이며 실제 로드 컨텍스트보다 작아야 한다. max_tokens와 max_completion_tokens는 함께 지정할 수 없다. n은 1만 지원한다. stop은 문자열 또는 최대 16개의 문자열 배열이며 각 값은 1~1024자이다.

이 API는 OpenAI Chat Completions의 텍스트·함수 호출 요청/응답 형식 중 문서화된 범위를 구현한다. 멀티모달 content, developer 역할, 함수명을 지정하는 tool_choice 객체, strict, JSON schema response_format, logprobs, reasoning 제어 설정, 여러 choice, 저장·조회 API 등은 지원하지 않는다. 알 수 없는 필드와 runtime/backend/device/path 등은 400으로 거부한다. 이름이 같은 엔드포인트가 전체 OpenAI API를 구현한다는 뜻은 아니다.

일반 응답은 다음 형태이다. 토큰 수는 실제 런타임 결과를 사용하며 아래 수치는 형식 예시이다.

```json
{
  "id": "chatcmpl-REQUEST_ID",
  "object": "chat.completion",
  "created": 1788768000,
  "model": "model://qwen3-8b-q4",
  "choices": [{"index": 0, "message": {"role": "assistant", "content": "..."}, "finish_reason": "stop"}],
  "usage": {"prompt_tokens": 20, "completion_tokens": 10, "total_tokens": 30, "prompt_tokens_details": {"cached_tokens": 0}}
}
```

## SSE와 취소

`stream: true`는 `text/event-stream`과 HTTP chunked encoding을 사용한다. 각 이벤트는 `data: <JSON>` 뒤에 빈 줄을 붙인다. 처음에는 choices[0].delta.role=assistant, 이후에는 delta.content, 마지막에는 빈 delta와 finish_reason(stop, length 또는 tool_calls)을 보낸다. object는 chat.completion.chunk이며 id/model/created는 응답 내에서 같다. 마지막 마커는 `data: [DONE]`이다. UTF-8 토큰 경계를 HTTP 패킷 경계와 동일하다고 가정하지 않는다.

```sh
curl -N http://127.0.0.1:8080/v1/chat/completions \
  -H 'Content-Type: application/json' \
  -d '{"model":"model://qwen3-8b-q4","messages":[{"role":"user","content":"Hello"}],"stream":true,"stream_options":{"include_usage":true},"max_tokens":128}'
```

include_usage=true이면 일반 청크에는 usage=null을 넣고 종료 마커 전에 choices=[]와 실제 usage를 가진 추가 청크를 보낸다. 헤더 전의 검증·큐 오류는 HTTP 오류 JSON이다. SSE 시작 후 추론 오류·시간 제한·출력 상한은 error 객체와 [DONE]으로 종료한다. 연결이 끊기거나 서버가 닫히면 생성 핸들을 취소하며 더 이상 최종 이벤트 전달을 보장할 수 없다.

## 함수 도구 호출

`tools`의 각 항목은 `{"type":"function","function":{"name":"...","description":"...","parameters":{...}}}` 형식이다. `tool_choice`는 `auto`(기본), `required`, `none`을 지원하며, `parallel_tool_calls`는 bool(기본 true)이다. `required`에는 하나 이상의 도구가 필요하다. 호출할 도구가 있는 요청 또는 과거 도구 호출/결과가 있는 메시지는 `Service::converse`를 사용한다. 현재 네이티브 llama.cpp에서 지원하며 미지원 런타임은 503을 반환한다.

응답의 `message.tool_calls`에는 `id`, `type:function`, `function.name`, JSON 객체를 문자열로 인코딩한 `function.arguments`가 담긴다. 텍스트가 없으면 content는 null이고 finish_reason은 tool_calls이다. API는 클라이언트의 도구를 자동 실행하지 않는다. 클라이언트가 도구를 실행하고 원래 assistant 메시지와 `{"role":"tool","tool_call_id":"받은 ID","content":"관측 결과"}`를 전체 이력에 붙여 다음 요청을 보낸다. 도구 결과가 모두 오기 전 다음 user/assistant 메시지를 끼워 넣거나 ID를 재사용하면 400으로 거부한다.

SSE에서는 완성·검증된 도구 호출을 `delta.tool_calls` 배열로 보내며 각 호출에 index를 붙인다. 잘린 도구 호출은 반환하지 않는다. 구조화 대화의 텍스트·도구 인자는 아직 토큰 단위로 전달하지 않고 완성된 파싱 결과를 보낸다. 런타임이 분리한 추론 내용은 `reasoning_content`로 반환할 수 있다. 이는 reasoning 예산 등 제어 설정을 구현했다는 뜻이 아니다.

```json
{
  "model": "qwen2.5:0.5b",
  "messages": [{"role":"user","content":"Look up the current document title."}],
  "tools": [{"type":"function","function":{
    "name":"current_document","description":"Read the current document title",
    "parameters":{"type":"object","properties":{},"additionalProperties":false}
  }}],
  "tool_choice":"auto", "max_tokens":256
}
```

일반 텍스트 HTTP 요청은 Service::complete(CompletionRequest)를 호출한다. Service::chat과 같은 작업 함수·PromptEngine·ContextCacheManager·RuntimeContext::generate를 사용한다. 실행할 때 임시 세션을 만들고 성공·실패·취소 후 worker에서 세션과 KV를 제거한다. 다른 Native IPC 세션의 대화 이력은 수정하지 않는다. 후속 HTTP 요청은 필요한 이력 전체를 messages에 전달한다. 요청 사이의 지속적인 KV 재사용이 필요하면 Native IPC의 명시적 세션 API를 사용한다. 공유 LRU 예산으로 인해 HTTP 요청도 다른 세션의 물리 KV를 eviction할 수 있으며 그 세션의 이력은 유지된다.

## 오류와 제한

오류 body는 {"error":{"message":"...","type":"invalid_request_error 또는 server_error","param":null,"code":"서비스 오류 코드"}}이다. 잘못된 입력·컨텍스트 초과는 400, 없는 모델은 404, 충돌은 409, 잘못된 Content-Type은 415, body 상한은 413, 큐/자원 상한은 429, 엔진 오류는 500, 엔진 불가·종료 중은 503, 요청 시간 초과는 504이다. 알려지지 않은 경로·미등록 HTTP 메서드는 404이다. 모든 응답에서 성공 상태 코드만 보고 추론 완료를 판단하지 말고 SSE의 마지막 상태·오류도 확인한다.

HttpOptions 기본값은 전송 worker 8개, 대기 연결 16개, 요청 body 1 MiB, 스트리밍 대기 큐 및 누적 출력 텍스트 각각 4 MiB, socket read/write timeout 각각 5초, 파싱 후 서비스 대기·생성 deadline 300초이다. 전송 큐가 가득 차면 추가 연결을 닫으며 서비스 scheduler 큐가 가득 차면 429를 반환한다. HTTP/1.1 연결당 요청 하나를 처리한다. /v1/models는 상주 모델의 읽기 전용 값 스냅샷을 사용하므로 추론 FIFO가 끝나기를 기다리지 않는다.

127.0.0.1에만 bind하고 Host를 실제 localhost 포트와 대조한다. 다른 Origin이나 Origin:null을 거부하며 교차 출처 CORS를 허용하지 않는다. HTTP에는 OS 사용자 단위 인증이 없으므로 같은 컴퓨터의 프로세스가 접근할 수 있다. 사용자 전용 소켓 권한은 Native IPC에 적용된다. HTTP에 TLS·원격 호스트 바인딩·정적 파일 제공을 추가하지 않는다.

Python 표준 라이브러리로 호출할 수 있다.

```python
import json
from urllib.request import Request, urlopen

request = Request("http://127.0.0.1:8080/v1/chat/completions",
    data=json.dumps({"model": "model://qwen3-8b-q4",
        "messages": [{"role": "user", "content": "Hello"}], "max_tokens": 128}).encode(),
    headers={"Content-Type": "application/json"})
with urlopen(request, timeout=310) as response:
    result = json.load(response)
print(result["choices"][0]["message"]["content"])
```

출처와 의존성은 [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md), 테스트 환경과 실제 결과는 [Verification.md](Verification.md)를 따른다.

생성 제어의 전체 기본값·제약·런타임 지원은 [Parameters.md](Parameters.md)를 따른다. HTTP도 동일한 객체 검증기를 사용하며, 활성 typical_p는 llama.cpp에서만 지원한다. 학습 설정은 대화 요청의 필드가 아니며 별도 ParameterObject로 검증·내보낸다.

도구 요청은 임시 SessionManager 세션을 만들지 않으며 요청별 구조화 컨텍스트를 사용하고 종료 후 해제한다. 앱 내부의 지속 KV 재사용은 C++ ConversationRequest.contextId 또는 agent::Engine 세션을 사용한다.
