# 로컬 IPC 계약 v1

전송은 QLocalSocket(Unix domain socket / Windows named pipe)이고 UTF-8 JSON 객체 + LF가 한 프레임이다. HTTP/SSE/JSON-RPC 2.0과 별개의 로컬 프로토콜이다.

`setRpcHandler`로 확장 메서드를 연결할 수 있다. daemon의 인증된 `agent.*` 메서드는 최상위 `auth` 키와 accepted/rpc 이벤트를 사용한다. 같은 앱의 HTTP 세션·취소와 공유하는 계약은 [AgentAPI.md](AgentAPI.md)에 있다. 기존 메서드의 인증·응답 계약은 유지된다.

동일 Service에 localhost HTTP도 함께 연결할 수 있다. `--socket <endpoint> --http-port <port>`로 두 계층을 시작하며 모델·scheduler·자원 상한을 공유한다. Native IPC는 장기 세션과 모델 관리를, [HTTP](HTTP.md)는 외부 프로그램용 텍스트 Chat Completions JSON/SSE를 제공한다.

요청은 `{"id":"client-id","method":"...","params":{...}}`이다. id는 1~128자 문자열이며 연결 내 미완료 요청 사이 중복은 거부한다. 일반 응답은 `{"id":"client-id","result":...}` 또는 `{"id":"client-id","error":{"code":"...","message":"..."}}`이다.

| method | params | result |
| --- | --- | --- |
| hardware.get | {} | cpu_architecture, ram_bytes, apple_silicon, gpus, metal_available, cuda_available, vulkan_available, diagnostics |
| models.install | package_directory | 설치한 model URI, manifest, loaded=false |
| models.pull | model(URI 또는 등록 별칭) | accepted/progress 이후 설치한 ModelRecord 또는 error |
| models.remove | model(URI/등록 별칭) | {}; 로드 중이면 model_in_use |
| models.list | {} | {models: 설치한 model/manifest/loaded 배열, issues: 잘못된 패키지 진단 배열} |
| models.resolve | model(URI/등록 별칭) | model, manifest, loaded; 가중치를 해싱하지 않는다 |
| models.verify | model(URI/등록 별칭) | model, valid, checked_files, checked_bytes, issues |
| models.loaded | {} | 상주 모델 배열: model/manifest/loaded/context_tokens/options/execution/memory/keep_alive_ms/expires_in_ms/active_requests |
| models.load | model(URI/등록 별칭), 선택 context_tokens(0), options, keep_alive | 로드한 모델과 서비스가 정한 execution |
| models.unload | model(URI/등록 별칭) | {}; 세션이 있으면 model_in_use |
| sessions.create | model(URI/등록 별칭), 선택 system | session_id; 설치된 모델에 chat capability가 필요하며 첫 chat에서 자동 로드한다 |
| sessions.get | session_id | session_id, model(정규 URI), messages(role/content 배열) |
| sessions.reset | session_id | {}; system 유지, 대화 및 KV 삭제 |
| sessions.close | session_id | {}; 세션 및 KV 삭제 |
| chat | session_id, prompt, 선택 options, keep_alive | 스트리밍 이벤트 |
| cancel | request_id | cancel_requested; 이 연결의 생성 또는 pull만 취소 |
| stats | {} | loaded_models, sessions, cached_contexts, reserved_context_tokens, cache_evictions, resident_estimated_bytes, memory_budget_bytes, available_ram_bytes(null 가능), default_keep_alive_ms, model_loads, model_evictions |

상태를 다루는 제어 메서드도 FIFO에 들어가므로 긴 추론 뒤에서 대기할 수 있다. hardware.get은 부팅 시 스냅샷, models.loaded는 worker가 게시한 상주 모델 스냅샷을 사용하므로 긴 추론 중에도 응답한다. parameters.list/get/validate는 모델 실행과 무관한 카탈로그 조회·검증으로 Qt 이벤트 루프에서 처리한다. stats와 나머지 제어 API는 FIFO에 들어간다. cancel은 worker 큐를 거치지 않으며 연결의 미완료 요청 상한에도 허용한다. 성공한 cancel 응답은 취소 요청 접수이며 이미 끝난 결과를 소급 취소하지 않는다.

```json
{"id":"hardware","method":"hardware.get","params":{}}
{"id":"install","method":"models.install","params":{"package_directory":"/absolute/path/qwen-package"}}
{"id":"list","method":"models.list","params":{}}
{"id":"resolve","method":"models.resolve","params":{"model":"model://qwen3-8b-q4"}}
{"id":"verify","method":"models.verify","params":{"model":"model://qwen3-8b-q4"}}
{"id":"load","method":"models.load","params":{"model":"model://qwen3-8b-q4","context_tokens":2048}}
{"id":"new","method":"sessions.create","params":{"model":"model://qwen3-8b-q4","system":"You are helpful."}}
{"id":"turn","method":"chat","params":{"session_id":"SESSION_ID","prompt":"Hello","options":{"max_tokens":128,"temperature":0.7,"top_p":0.9,"top_k":40,"seed":0,"stop":["<END>"]}}}
```

세션 생성 응답의 session_id를 사용한다. stop은 최대 16개, 각 1~1024자이다. max_tokens는 1 이상이며 모델 컨텍스트보다 작아야 한다. 타입·범위를 벗어난 수치는 거부한다.

models.install은 관리 도구가 지정한 로컬 패키지를 서비스 저장소에 복사한다. 모델을 사용하는 요청은 model://id 또는 서비스 registry에 등록한 별칭을 받는다. manifest의 파일 목록은 IPC 응답에서 생략하며 절대 가중치 경로를 반환하지 않는다. 상대 entry_point는 manifest 메타데이터이다. 설치 스키마·원자적 게시·소유권 잠금·오류 계약은 [ModelManagement.md](ModelManagement.md)에 있다.

models.load의 id/path/runtime/backend/device/device_id/gpu_layers 등 미지원 필드는 invalid_argument이다. 이전 path/model_id 계약은 허용하지 않는다. options 안의 실행 장치 지정도 거부한다. llama.cpp 모델 options는 threads 및 chat_template만 지원하며 MLX는 모델 options를 받지 않는다. 실행 선택은 서비스의 응답으로만 전달한다. context_tokens는 0~1,048,576 정수이며 1은 거부한다. 0 또는 생략 시 manifest 상한·서비스 기본값(2048)·캐시 토큰 예산 중 최솟값을 적용한다. 명시한 값이 manifest 상한을 넘으면 context_overflow이다.

```json
{"id":"load","result":{"model":"model://qwen3-8b-q4","manifest":{"schema_version":1,"id":"qwen3-8b-q4","architecture":"qwen3","format":"gguf","quantization":"Q4_K_M","context_length":32768,"capabilities":["text-generation","chat","tool-calling"],"entry_point":"model.gguf"},"loaded":true,"context_tokens":2048,"options":{},"execution":{"runtime":"llama.cpp","backend":"metal","device_id":"llama.cpp/MTL/MTL0","reason":"Apple Silicon with usable Metal","fallback_reasons":[]}}}
```

위 JSON은 메모리 필드가 생략된 실행 선택 예시이다. models.load/models.loaded에는 `memory`(estimated_bytes/weights_bytes/context_bytes/overhead_bytes/basis), `keep_alive_ms`, `expires_in_ms`, `active_requests`도 포함한다. 활성 요청 중 expires_in_ms=-1이며 ps 조회는 만료 시간을 연장하지 않는다. keep_alive는 숫자 초 또는 ms/s/m/h 문자열이고 생략 시 현재/기본 정책을 쓴다. 0은 생성 종료 후 즉시 해제한다. 상세 예산은 [Residency.md](Residency.md)를 따른다.

위 device_id는 예시이며 실제 값은 불투명한 로컬 id이다. backend는 metal/cuda/vulkan/cpu이다. CPU의 device_id는 빈 문자열이다. fallback_reasons는 GPU 모델 초기화에 실패한 경우의 원인을 담는다. GPU별 vendor/vram_bytes/unified_memory/recommended_working_set_bytes/available_backends의 정확한 의미는 [HardwarePolicy.md](HardwarePolicy.md)에 있다.

verify가 valid=false를 반환하면 issues에 파일의 누락·추가·크기/해시 불일치 원인이 있다. 상주하지 않은 모델의 load는 전체 검증을 수행하므로 손상된 파일은 integrity_failure로 거부한다. 이미 상주한 인스턴스의 재사용은 파일을 다시 해싱하지 않는다. list와 resolve는 메타데이터 조회이다. 파일을 처음 설치할 때 생성한 해시는 배포자 서명이나 모델 품질 인증을 의미하지 않는다.

models.pull의 스트림은 accepted → 0개 이상의 progress → 일반 result/error 순서이다. progress는 model, file, received_bytes, total_bytes를 포함한다. cancel에는 accepted.request_id를 사용한다. 네트워크 I/O·다운로드 파일은 모두 서비스 소유이며 클라이언트는 원본 URL을 지정하지 않는다.

```json
{"id":"pull","method":"models.pull","params":{"model":"qwen3:8b"}}
```

```json
{"id":"turn","event":"accepted","request_id":"GENERATION_ID"}
{"id":"turn","event":"started","request_id":"GENERATION_ID"}
{"id":"turn","event":"delta","request_id":"GENERATION_ID","text":"Hello"}
{"id":"turn","event":"done","request_id":"GENERATION_ID","result":{"request_id":"GENERATION_ID","session_id":"SESSION_ID","text":"Hello","finish_reason":"stop","usage":{"prompt_tokens":18,"generated_tokens":2,"cached_tokens":0,"dropped_messages":0}}}
```

accepted는 핸들 발급이며 성공 보장이 아니다. 큐 거절·입력 오류·실행 전 취소에는 started가 없을 수 있으나 done은 하나이다. finish_reason은 stop/length/cancelled/error이다. 실패 result에는 error(code/message)가 추가된다. delta를 결합하고 done으로 완료 여부를 확인한다.

```json
{"id":"cancel-turn","method":"cancel","params":{"request_id":"GENERATION_ID"}}
```

생성이 끝나고 모델을 제거할 때에는 해당 모델의 모든 세션을 닫고 언로드한다. unload만 수행하면 설치 파일은 재시작 후에도 남는다.

```json
{"id":"close","method":"sessions.close","params":{"session_id":"SESSION_ID"}}
{"id":"unload","method":"models.unload","params":{"model":"model://qwen3-8b-q4"}}
{"id":"remove","method":"models.remove","params":{"model":"model://qwen3-8b-q4"}}
```

현재 사용자만 접근하도록 소켓 권한을 지정한다. 같은 OS 사용자의 여러 클라이언트는 모델과 세션을 공유한다. 별도 테넌트 인증이나 세션 소유권 분리는 제공하지 않는다. 다른 연결의 생성 핸들 취소는 거부한다.

기본 상한은 연결 16개, 연결별 미완료 요청 64개, 입력 프레임 1 MiB, 스트리밍 inbox 4 MiB, 소켓 쓰기 버퍼 4 MiB이다. IpcOptions로 변경한다. 입력 프레임 또는 출력 버퍼 상한 초과 시 연결을 닫고 생성을 취소한다. 끊어진 연결로 완료 이벤트를 전달할 수는 없다. 세션은 남으므로 재접속 후 조회·종료할 수 있다.

## Python 클라이언트 (Unix)

```python
import json
import socket

with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
    client.connect("/absolute/workspace/path/build/llm.sock")
    stream = client.makefile("rwb")

    def send(value):
        stream.write((json.dumps(value) + "\n").encode())
        stream.flush()

    # 서비스에 해당 모델이 이미 설치·로드되어 있다고 가정한다.
    send({"id": "new", "method": "sessions.create", "params": {"model": "model://qwen3-8b-q4"}})
    session = json.loads(stream.readline())["result"]["session_id"]
    send({"id": "turn", "method": "chat", "params": {"session_id": session, "prompt": "Hello"}})
    for line in stream:
        event = json.loads(line)
        if event.get("event") == "delta":
            print(event["text"], end="", flush=True)
        if event.get("event") == "done":
            print("\n", event["result"]["finish_reason"])
            break
```

## 제어 파라미터

- `parameters.list {}`: id/description/phase/fields 그룹 요약 배열.
- `parameters.get {"group":"trl.GRPOConfig"}`: 타입·기본값·설명·소스·바인딩을 포함한 상세 정의.
- `parameters.validate {"group":"peft.LoraConfig","values":{"r":16},"defaults":false,"redact":false}`: 필수 필드를 포함해 검증한 원본 JSON. 잘못된 필드·타입·제약은 invalid_argument.

`chat.options`는 `iiLocalLLM.GenerationOptions` 객체의 16개 필드를 받으며 알 수 없는 옵션을 거부한다. [Parameters.md](Parameters.md)의 필드표와 예제를 따른다. 학습·파인튜닝 객체를 조회·내보내는 작업은 모델이나 학습 패키지를 로드하지 않는다.
