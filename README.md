# iiLocalLLM

C++ 에이전트 하네스를 확장 중이다. 현재 실행 계층은 [AgentHarness.md](docs/AgentHarness.md), 전체 요구사항과 남은 구현은 [HarnessParity.md](docs/HarnessParity.md)에 기록한다. MCP/API 및 앱 전체 호환 완료와 기존 대화 기능 완료는 별도 상태로 관리한다.

0.32.0은 성공한 MCP 도구의 결과를 명령·HTTP·C++ 훅에서 교체하는 updatedMCPToolOutput을 추가한다. 모델·API·MCP에 변경된 관측을 전달하며 원래 구조화 결과와 일반 도구를 구분한다. 설정, 콘텐츠 형식 및 참조 차이는 [McpOutputHooks.md](docs/McpOutputHooks.md)를 따른다. 전체 하네스는 계속 구현 중이다.

0.31.0은 C++ 에이전트 훅을 추가한다. 별도 대화에서 실제 도구를 사용하고 StructuredOutput으로 판단하며, dontAsk 권한·50개 메시지 한도·취소와 임시 상태 정리를 적용한다. API·CLI·MCP도 같은 경로를 사용한다. 설정과 참조 차이는 [AgentHooks.md](docs/AgentHooks.md)에 기록한다. 전체 하네스는 계속 구현 중이다.

0.29.0은 C++ HTTP/HTTPS 훅을 API·CLI·MCP 실행 경로에 연결한다. JSON 결정·입력/권한 변경, URL/환경 허용 목록, DNS 주소 고정·TLS 검증, 프록시와 취소/시간/응답 한도를 제공한다. 설정과 참조 차이는 [HTTPHooks.md](docs/HTTPHooks.md)를 따른다. 기존 일반/제어 응답 용량은 [ControlCapacity.md](docs/ControlCapacity.md), 앱 권한 요청은 [PermissionRequests.md](docs/PermissionRequests.md)에 기록한다.

0.25.0은 Ask 도구의 PermissionRequest 훅과 구조화된 C++ 호스트 응답을 연결한다. 입력 변경 뒤 정책과 실행 대상을 재검사하고, 지속 권한 갱신은 명시적인 호스트 처리기로 전달한다. [PermissionRequest.md](docs/PermissionRequest.md)에 계약과 남은 범위를 기록한다.

0.24.0은 C++·API·MCP의 대화 초기화와 즉시 SessionStart(clear)를 제공한다. 백그라운드 셸·자식 에이전트·완료 알림은 새 대화로 이어진다. 계약과 오류 복구 한계는 [SessionClear.md](docs/SessionClear.md)에 기록한다.

C++20, Qt 6.8.3 Core/Network 기반 로컬 LLM 서비스 SDK이다. 버전은 0.32.0이다. 앱은 `model://id`로 모델을 사용한다. 서비스는 manifest와 설치 파일을 관리하고 시작 시 검사한 하드웨어에 따라 실행 장치를 자동 선택한다. 모델 실행은 llama.cpp 또는 MLX에 맡기고 세션, 프롬프트 예산, KV 캐시, FIFO 스케줄링, 스트리밍, 로컬 IPC를 관리한다. 기존 `helloWorld()`와 `iiLocalLLM::iiLocalLLM` CMake 타깃은 유지한다.

C++ stdio MCP 클라이언트가 외부 도구·리소스·프롬프트를 인식하고 에이전트 엔진에 연결한다. `iillm-mcp` 서버와 C++ 내장 API로 앱 도구 및 로컬 에이전트 실행을 외부 MCP 클라이언트에 제공한다. 프로토콜·정책·자료 보존 및 현재 지원 경계는 [MCP.md](docs/MCP.md) · [MCP 서버·앱 도구 제공](docs/MCPServer.md)에 설명한다.

`agent::Api`를 같은 daemon의 HTTP `/v1/rpc`와 native IPC에 연결하면 앱별 키 인증·영속 세션·기본 transcript 분기·실행 이벤트·취소를 공유한다. 설치된 `iillm --auth-file FILE rpc METHOD [PARAMS_FILE]`로도 호출한다. 설정·메서드·수명·현재 한계는 [AgentAPI.md](docs/AgentAPI.md)에 설명한다.

0.12.0은 데스크톱 POSIX의 실제 백그라운드 Bash 실행, `TaskOutput`·`TaskStop`·`ShellTaskList`, 실행 기록·출력 보존을 제공한다. 인증된 `agent.shell.*`, 얇은 CLI 및 MCP에서 같은 실행을 제어하며 모델의 다음 턴에는 현재 실행 상태를 전달한다. 대기 취소와 프로세스 종료, 정상 종료와 비정상 종료 복구의 차이는 [BackgroundTasks.md](docs/BackgroundTasks.md)에 명시한다. 소스와 설치 헤더·라이브러리는 함께 다시 빌드한다.

```text
C++ Local API / Native IPC / localhost HTTP
                  │
           iiLocalLLM Service
     Hardware Detection / Device Policy
    Model Manager ── ModelCatalog → Models/manifest.json
    Session Manager / RuntimeManager
    Prompt Engine / Context Cache Manager
        FIFO Scheduler / Streaming
                  │
        Runtime → RuntimeModel → RuntimeContext
             ┌────┴─────┐
     llama.cpp/GGUF   MLX/Python
```

| 구성 | 동작 |
| --- | --- |
| Model Manager | install/remove/list/resolve/verify/load/unload, manifest·URI·파일 무결성 관리, 엔진 수명 조정 |
| ModelCatalog / RuntimeManager | 엔진과 독립된 디스크 카탈로그 / 모델 형식·장치에 따른 런타임 선택과 메모리 수명 |
| Hardware / Policy | GPU vendor·VRAM·통합 메모리·RAM·CPU·가속 API 검사, Metal → CUDA → Vulkan → CPU 정책 |
| Session Manager | 모델별 system/user/assistant 이력, 초기화·종료 |
| Prompt / Chat Engine | 실제 tokenizer, 출력 토큰 예약, 오래된 완결 턴 제거 |
| Context / KV Cache Manager | 세션별 runtime context, LRU, 개수·예약 토큰 상한 |
| Scheduler | 전용 작업 스레드, bounded FIFO, 취소, 종료 시 future 완료 |
| Streaming | started → delta → finished, Unicode 처리, 청크를 가로지르는 stop |
| Local API / IPC / HTTP | C++ future/handle API, 사용자 전용 Local Socket의 NDJSON, localhost Chat Completions JSON/SSE |
| Runtime Abstraction | Runtime / RuntimeModel / RuntimeContext의 세 인터페이스 |

ONNX 등은 위 인터페이스를 구현하여 등록한다. 현재 내장 어댑터는 llama.cpp와 MLX이다. HTTP Chat Completions는 텍스트와 함수 도구 호출 일부 계약을 구현한다. 영속 에이전트 세션은 별도 agent API로 제공하며 멀티모달 입력은 미완료이다.

## 상세 제어 객체

추론부터 학습·파인튜닝까지 391개 설정 그룹을 `ParameterCatalog`, `ParameterObject`, `ControlParameters`로 다룬다. 원본 타입·기본값·선택값·제약·설명·상속·소스 커밋/해시를 조회하고 원본 JSON으로 내보낸다. 공통 생성 옵션 16개는 실제 llama.cpp/MLX 요청에 연결된다. 학습 객체는 설정 검증·내보내기를 제공한다. 조사 범위와 실행 지원의 구분, API·CLI 예제는 [Parameters.md](docs/Parameters.md), 공식 소스와 라이선스는 [ParameterSources.md](docs/ParameterSources.md)를 참고한다.

```sh
./build/iillm parameters trl.GRPOConfig
./build/iillm parameters peft.LoraConfig docs/examples/lora.json
./build/iillm run qwen2.5:0.5b "안녕하세요" --options docs/examples/generation.json
```

0.10.0은 실행 중인 로컬 앱의 인증된 MCP 주소를 자동 발견하고 실제 QObject 컨트롤러에 호출을 전달한다. Society·Dreamscapes의 데스크톱 도구와 취소·수명 계약은 [로컬 앱 연결](docs/LocalApplications.md)에 기록한다. 0.10 당시 ABI는 0.10이었다. 현재 버전의 소비자는 새 헤더·라이브러리로 함께 다시 빌드한다.

0.9.0은 설정 파일의 MCP 서버 연결·복구와 대화별 `ToolSearch`를 제공한다. 선택 상태는 재개·분기·압축 후에도 복구하고 스키마·연결 변경 시 다시 검색한다. `agent.mcp.status`와 `iillm agent mcp`로 상태를 조회한다. [도구 검색과 MCP 설정](docs/ToolDiscovery.md)에 사용법과 한계를 기록한다.

고정 Qwen2.5 0.5B 모델은 검색 후 연속 호출 검증을 통과하지 못했다. 이 모델에서는 MCP 도구를 처음부터 제공하는 `--agent-mcp-eager` / `--mcp-eager` 경로를 사용할 수 있다. 자세한 관측 결과는 [Verification.md](docs/Verification.md)에 기록한다.

0.8.0은 인증된 C++ MCP HTTP 서버와 `iillm-mcp --http-port`를 제공한다. 앱별 세션, 원래 요청 스트림의 SSE 재개, 취소·역방향 요청, 비공개 인증·상태 폴더를 지원한다. [MCP HTTP 서버](docs/MCPHTTPServer.md)를 참조한다.

0.7.0에서 도입한 공통 C++ MCP 클라이언트와 Streamable HTTP 연결·인증 헤더·SSE 복원·세션 재초기화도 유지한다. [MCP HTTP](docs/MCPHTTP.md)를 참조한다.

0.6.0은 실제 네이티브 토큰 측정, 자동 도구 결과 축소·여러 묶음의 대화 요약, 원본을 보존하는 압축 체크포인트와 수동 C++/API/MCP 호출을 제공한다. 공개 구조체와 Model 가상 인터페이스 변경으로 ABI는 0.6이며 새 헤더·라이브러리로 함께 다시 빌드한다. [대화 압축](docs/Compaction.md)과 [프로젝트 지침](docs/ProjectContext.md)을 참조한다. 전체 하네스 및 제품 앱 통합은 대응표에서 계속 추적한다.

## 빌드

CMake 3.24 이상, C++20 컴파일러, **Qt 6.8.3** Core/Network가 필요하다. 테스트에는 Qt Test와 Python 3도 사용한다. 헤더와 구현을 함께 배치하며 별도 소스 include 디렉터리를 두지 않는다.

기본 빌드는 llama.cpp를 포함하므로 GGUF 모델을 바로 실행할 수 있다. 고정 커밋의 아카이브를 SHA-256으로 검증하고 공유 SDK에 정적으로 링크한다. 엔진 없이 서비스 계약만 사용하는 구성은 `-DIILOCALLLM_WITH_LLAMA=OFF`로 선택한다. 모델 가중치는 빌드 중 다운로드하지 않는다. 모든 빌드 산출물은 build/ 아래에 둔다.

```sh
mkdir -p build/tmp build/ccache
export TMPDIR="$PWD/build/tmp"
export CCACHE_DIR="$PWD/build/ccache"
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON \
  -DCMAKE_PREFIX_PATH=/Volumes/Storage/Qt/6.8.3/macos \
  -DIILOCALLLM_WITH_LLAMA=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

기존 llama.cpp 소스는 `IILOCALLLM_LLAMA_SOURCE_DIR`로 지정한다. 검증 커밋은 `5202104b59ada9005db079eea43882a2b7bf5802`이다. 다른 커밋의 C API 호환성은 별도 확인이 필요하다.

새 빌드 구성은 설치된 CUDA Toolkit 및 Vulkan SDK(glslc, SPIRV-Headers 포함)를 탐지하여 해당 llama.cpp 모듈의 기본 빌드 여부를 정한다. Metal은 Apple 플랫폼의 기본 빌드를 따른다. GGML_CUDA/GGML_VULKAN 등의 CMake 값은 배포 패키지의 포함 모듈을 정하며 앱의 실행 장치 선택 API가 아니다. 기존 CMake 캐시 값은 보존한다.

## C++ API

모델 패키지를 먼저 설치한다. `manifest.json`은 다음 최소 필드를 갖는다. 설치 시 전체 파일의 크기와 SHA-256 목록을 생성하며, 이후 로드 전에 다시 검증한다.

```text
Models/
└─ qwen3-8b-q4/
   ├─ manifest.json
   ├─ model.gguf
   └─ tokenizer.json
```

```json
{
  "id": "qwen3-8b-q4",
  "architecture": "qwen3",
  "format": "gguf",
  "quantization": "Q4_K_M",
  "context_length": 32768,
  "capabilities": ["text-generation", "chat", "tool-calling"]
}
```

GGUF의 기본 진입 파일은 `model.gguf`이다. 다른 이름이면 `entry_point`를 지정한다. MLX는 `format: "mlx"`와 기본 `entry_point: "."`를 사용한다. capabilities는 패키지의 선언이며 도구 호출 API를 추가하지 않는다. 카탈로그 API, 스키마, 검증 범위는 [docs/ModelManagement.md](docs/ModelManagement.md)에 있다.

아래는 daemon 또는 임베딩 서비스 호스트의 C++ 예시이다. 여러 앱이 모델을 공유할 때에는 하나의 iiLocalLLMD에 Native IPC/HTTP로 접속한다. 호스트가 지정하는 modelsDirectory는 저장소 위치이며 클라이언트 추론 요청에는 파일 경로가 없다.

```cpp
#include <iiLocalLLM.h>
#include <QCoreApplication>
#include <iostream>

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    iiLocalLLM::ServiceOptions options;
    options.modelsDirectory = "Models";
    iiLocalLLM::Service service(options);
    auto hardware = service.hardware();
    auto model = service.loadModel({"model://qwen3-8b-q4", 2048}).get();
    std::cout << iiLocalLLM::enumName(model.execution.device.backend).toStdString() << '\n';
    auto session = service.createSession("model://qwen3-8b-q4", "You are a concise assistant.").get();
    iiLocalLLM::ChatRequest request;
    request.sessionId = session;
    request.prompt = "Explain KV caching.";
    request.options.maxTokens = 128;
    auto generation = service.chat(request, [](const iiLocalLLM::StreamEvent& event) {
        if (event.kind == iiLocalLLM::StreamEventKind::Delta)
            std::cout << event.text.toStdString() << std::flush;
    });
    // 다른 스레드에서도 generation.cancel()로 취소할 수 있다.
    auto result = generation.result.get();
    service.closeSession(session).get();
    service.unloadModel("model://qwen3-8b-q4").get();
    return result.errorCode == iiLocalLLM::ErrorCode::None ? 0 : 1;
}
```

제어 메서드의 실패는 future에서 `Error`로 전달한다. chat은 오류·취소도 GenerationResult로 반환하고 terminal 이벤트를 한 번 전달한다. 큐 거절 또는 실행 전 취소에는 started/delta가 없을 수 있다.

콜백은 작업 스레드에서 실행하며 즉시 큐 거절은 호출 스레드에서 전달한다. Qt UI 갱신은 queued invoke로 넘긴다. 콜백 안에서 서비스 future를 기다리거나 서비스를 파괴하면 안 된다. 콜백은 빨리 반환해야 한다. 서비스 파괴는 진행 중·대기 작업을 취소하고 worker 종료를 기다린다. 파괴와 새 메서드 호출을 동시에 수행하지 않는다.

## CLI와 공유 daemon

`iillm`은 Qt Core/Network만 링크하는 Native IPC 클라이언트이다. 추론 엔진을 링크하거나 daemon을 대신해 모델을 실행하지 않는다.

```sh
export IILLM_SOCKET="$PWD/build/llm.sock"
./build/iiLocalLLMD --models-root "$PWD/build/chat/Models" --socket "$IILLM_SOCKET" --http-port 8080
# 다른 터미널에서도 저장소 디렉터리로 이동하고 설정한다.
export IILLM_SOCKET="$PWD/build/llm.sock"
./build/iillm pull qwen2.5:0.5b
./build/iillm models
./build/iillm run qwen2.5:0.5b --system "You are a helpful assistant." --max-tokens 256
./build/iillm ps
```

시작 모델 `qwen2.5:0.5b`는 공식 Qwen2.5-0.5B-Instruct의 Q4_K_M GGUF이며 다운로드 크기는 491,400,032 bytes이다. `model://qwen2.5-0.5b-instruct-q4`로 해석한다. 기존 `qwen3:8b`도 유지한다. pull은 서비스가 고정된 공식 GGUF를 다운로드하고 SHA-256 검증 후 설치한다. 다운로드 후 대화는 인터넷이나 외부 추론 서비스 없이 실행된다.

터미널에서 Enter로 한 턴을 보내고 `/clear`로 system prompt를 유지한 채 대화 이력·KV를 초기화한다. `/bye`, `/exit`, EOF는 종료이며 Ctrl+C는 생성 취소·세션 정리 후 종료한다. `--temperature 0`은 greedy 생성을 선택한다. `--max-tokens`는 답변 길이 상한으로 긴 답변은 잘릴 수 있다. 작은 모델의 답변 품질은 모델 용량에 따른다.

CLI·GUI·Python·에이전트의 요청은 같은 상주 모델을 재사용한다. 모델이 메모리에 없으면 서비스가 예산과 가용 RAM을 검사하고 유휴 LRU 모델을 내린 뒤 로드한다. 기본 keep_alive는 5분이며 RAM 8 GiB 이하에서는 0이다. `iillm run qwen2.5:0.5b --keep-alive 0`으로 요청 직후 해제할 수 있다.

```sh
./build/iillm run qwen2.5:0.5b "What is 2 + 2?" --temperature 0
curl -N http://127.0.0.1:8080/v1/chat/completions \
  -H 'Content-Type: application/json' \
  -d '{"model":"qwen2.5:0.5b","messages":[{"role":"user","content":"Hello!"}],"stream":true,"max_tokens":128}'
```

[CLI 사용법](docs/CLI.md), [상주·메모리 정책](docs/Residency.md)에 설치 원본, 설정, 실패·취소 계약을 설명한다. 기존 `iilocal-llm-service` 실행 파일도 유지한다.

## IPC 서비스

로컬 패키지를 설치한 뒤 URI만 담은 models.json으로 부팅한다. `--models-root`의 기본값은 작업 디렉터리의 Models이다. 안정적인 서비스 배포에는 절대 저장소 경로를 지정한다.

```json
{"models":[{"model":"model://qwen3-8b-q4","context_tokens":2048,"options":{"threads":4}}]}
```

```sh
./build/iilocal-llm-service --models-root "$PWD/build/chat/Models" --install /absolute/path/qwen-package
./build/iilocal-llm-service --models-root "$PWD/build/chat/Models" \
  --config models.json --socket "$PWD/build/llm.sock"
```

설정 파일 없이 시작하여 models.install 또는 models.pull로 설치할 수 있다. models.install은 로컬 복사이며 models.pull은 서비스 registry의 검증된 원격 원본을 사용한다. 설치된 모델은 첫 생성 요청에서 자동 로드한다. 저장소마다 서비스 하나가 소유권 잠금을 유지하므로 실행 중인 서비스에는 IPC로 설치한다. 설치 파일은 재시작 후에도 유지하고 로드 상태와 세션은 다시 만든다. 기존 소켓을 자동 삭제하지 않는다. SIGINT/SIGTERM에서 연결과 추론을 정리한다. macOS Unix 소켓의 경로 길이 제한 때문에 짧은 경로를 사용한다. 프로토콜과 클라이언트 예시는 [docs/IPC.md](docs/IPC.md)에 있다.

## localhost HTTP

Native IPC와 HTTP를 같은 서비스에서 동시에 활성화한다. IPC는 macOS/Linux의 Unix Domain Socket과 Windows Named Pipe를 사용한다. HTTP는 127.0.0.1에만 바인딩한다.

```sh
./build/iilocal-llm-service --models-root "$PWD/build/chat/Models" \
  --config models.json --socket "$PWD/build/llm.sock" --http-port 8080
curl http://127.0.0.1:8080/v1/chat/completions \
  -H 'Content-Type: application/json' \
  -d '{"model":"model://qwen3-8b-q4","messages":[{"role":"user","content":"Hello"}],"max_tokens":128}'
```

`--http-port 0`은 빈 포트를 자동 선택하고 주소를 출력한다. `--http-port`만 지정하면 HTTP 전용이고, 둘 다 생략하면 사용자별 기본 Native IPC endpoint로 시작한다. GET /health, GET /v1/models, POST /v1/chat/completions를 제공한다. `stream: true`는 SSE delta와 [DONE]을 반환한다.

HTTP는 Service::complete를 통해 기존 채팅 실행 경로와 scheduler를 사용한다. messages의 과거 대화와 최신 user 입력을 임시 세션으로 처리하고 완료·오류·연결 취소 시 세션과 KV를 정리한다. Native IPC에서 로드한 모델을 HTTP가 그대로 사용하며, 장기 IPC 세션의 이력은 유지한다. 지원하는 텍스트 Chat Completions 범위, Python 예제, 오류·자원 제한은 [docs/HTTP.md](docs/HTTP.md)에 있다.

## MLX

MLX는 Apple Silicon용 선택 의존성이다. 별도 Python 환경과 로컬 모델 디렉터리가 필요하다. 검증 버전은 mlx-lm 0.31.3 / mlx 0.32.2이다.

```sh
python3 -m venv build/mlx-env
build/mlx-env/bin/python -m pip install -r runtimes/mlx-requirements.txt
./build/iilocal-llm-service --socket "$PWD/build/llm.sock" \
  --mlx-python "$PWD/build/mlx-env/bin/python" \
  --mlx-worker "$PWD/runtimes/mlx_worker.py"
```

tokenizer/config/safetensors와 MLX manifest를 포함한 패키지를 설치하고 URI로 로드하면 서비스가 MLX를 선택한다. 앱은 runtime을 지정하지 않는다. 런타임은 네트워크 다운로드를 하지 않고 trust_remote_code=False를 적용한다. 가중치는 별도로 준비한다. 모델당 Python 프로세스 하나를 유지하고 세션별 KV 상태를 분리한다. 취소·타임아웃·소비자 오류 시 프로세스를 종료하고 다음 요청에서 모델을 다시 로드한다. 이때 같은 모델의 다른 세션도 물리 KV 캐시가 사라지지만 대화 이력은 유지된다. MLX 기본 장치와 생성 stream은 서비스가 지정한 Metal/CPU로 초기화한다. 임베딩 서비스의 Python/worker 배포 경로는 Service 생성자의 두 번째 MlxRuntimeOptions 인수로 지정한다.

## 실행 정책

앱은 ModelLoadRequest의 model URI/contextTokens/options를 전달한다. ModelManager가 URI를 해석하고 manifest·전체 파일 해시·형식 구조를 검증한 뒤 내부 ModelSpec에 실제 경로와 형식을 전달한다. GGUF는 llama.cpp, MLX 패키지는 MLX가 처리한다. 내장 런타임 등록도 서비스가 처리한다. 모델 형식은 장치 선택과 별개이므로 Apple Silicon에서도 GGUF는 llama.cpp/Metal로 실행한다.

| 하드웨어와 가용 조건 | 선택 |
| --- | --- |
| Apple Silicon + 동작하는 Metal | Metal |
| NVIDIA GPU + 동작하는 CUDA | CUDA |
| AMD/Intel GPU + 동작하는 Vulkan | Vulkan |
| 모델 런타임에서 사용할 수 있는 위 GPU 경로가 없음 | CPU |

GPU 가속은 해당 엔진에도 컴파일·지원되어 있어야 한다. 같은 우선순위에서는 확인된 메모리 크기, 장치 id 순으로 정렬한다. 모델 로드 또는 llama.cpp GPU 컨텍스트 사전 할당 실패 시 CPU로 재시도한다. 잘못된 입력·취소는 재시도하지 않는다. runtime/backend/device/gpu_layers를 모델 요청에 넣으면 invalid_argument이다. 실행 결과는 ModelInfo.execution 및 models.loaded로 조회하며 hardware.get 또는 `build/iilocal-llm-service --hardware`로 부팅 시 검사한 정보를 조회한다. 필드 의미와 실패 범위는 [docs/HardwarePolicy.md](docs/HardwarePolicy.md)에 있다.

contextTokens=0 또는 생략 시 manifest.context_length, ServiceOptions.defaultContextTokens(기본 2048), 캐시 토큰 예산 중 최솟값을 사용한다. 명시한 컨텍스트가 manifest 상한을 넘으면 context_overflow이다. 명시적인 unload/remove는 해당 모델의 세션을 모두 닫은 뒤 호출한다. 자동 LRU/만료는 세션의 이력을 보존하면서 KV와 가중치를 해제한다.

기본 상한은 대기 작업 64개, 모델 4개, 세션 128개, 캐시 컨텍스트 4개, 예약 컨텍스트 토큰 합계 16,384개이다. ServiceOptions로 변경한다. 제어와 생성을 한 worker에서 FIFO로 실행한다. 같은 세션의 후속 요청은 앞선 성공 응답이 저장된 뒤 최신 이력을 읽는다. 여러 Service 인스턴스 사이의 GPU 사용량은 별도 관리 대상이다.

ModelResidencyManager가 가중치·KV·런타임 여유분의 바이트 예약과 최신 가용 RAM을 검사한다. 개수/바이트 상한에 도달하면 유휴 LRU 모델을 해제하고, 만료 시에는 새 요청 없이도 worker가 해제한다. 활성 요청은 보호하며 유휴 세션의 이력은 보존한다. 추정치는 프로세스 RSS의 강제 상한이 아니다. 상세 산식과 한계는 [Residency.md](docs/Residency.md)에 있다. 배치 추론은 제공하지 않는다.

실제 prompt 토큰과 maxTokens 합계로 컨텍스트를 검사한다. 초과 시 오래된 user/assistant 쌍을 제거하되 system과 최신 user는 보존한다. 여전히 초과하면 context_overflow이다. 성공한 경우에만 축소한 이력과 assistant를 저장한다. 오류·취소 시 기존 이력을 유지하고 KV를 폐기한다. 이미 보낸 partial text는 결과에 남아도 이력에는 저장하지 않는다. 오류·취소 시 중간 토큰 사용량은 완전하지 않을 수 있다.

llama.cpp의 내장 chat template formatter가 지원하지 않는 템플릿은 오류이다. 템플릿 없는 GGUF에는 options.chat_template을 명시한다(예: chatml). 임의 모델에 ChatML을 자동 적용하지 않는다. MLX는 모델 tokenizer의 chat template을 사용한다. 부분 KV 제거가 불가능한 모델은 캐시를 새로 구성한다.

0.12.1은 모델 로딩의 `enable_thinking` boolean을 네이티브 Jinja 템플릿에 전달한다. 명시적 설정은 일반 대화와 구조화 도구 대화에 함께 적용한다. 추론만 반환된 응답은 실행 가능한 도구나 최종 답으로 승격하지 않고 별도 오류로 구분한다. [네이티브 추론 모드](docs/NativeThinking.md)에 제어·검증 범위를 기록한다.

llama.cpp의 구조화 도구 생성에는 모델 로딩 옵션 `tool_grammar`(boolean, 기본 true)를 제공한다. false는 생성 시 문법 제약을 끄며 실행 전 스키마·권한 검사는 유지한다. 다중 필드 인수와 모델별 운용 조건은 [Tasks.md](docs/Tasks.md)에 설명한다.

stop 문자열은 청크 사이 부분 일치를 보관하고 완성되면 출력에서 제외한다. stop으로 잘린 KV는 이력과 달라질 수 있어 폐기한다. 생성 옵션은 maxTokens, temperature, topP, topK, seed, stop이다. 동일 seed가 서로 다른 런타임에서 동일 출력을 보장하지는 않는다.

구조와 확장 계약은 [docs/Architecture.md](docs/Architecture.md)에 있다.

## 설치와 검증

기본 설치 경로는 $HOME/.local/SDK/iiLocalLLM이다. install.sh는 빌드·CTest·설치 후 설치된 패키지를 소비하는 별도 프로젝트도 검증한다. Workspace staging 경로를 지정할 수 있다.

```sh
IILOCALLLM_WITH_LLAMA=ON INSTALL_PREFIX="$PWD/build/stage" ./install.sh
```

IILOCALLLM_WITH_LLAMA를 생략하면 기존 CMake 선택을 유지하며 새 구성의 기본값은 ON이다. 과거 OFF로 구성했던 build/는 `IILOCALLLM_WITH_LLAMA=ON ./install.sh`로 활성화한다. INSTALL_PREFIX, QT_PREFIX_PATH, CMAKE_PREFIX_PATH로 경로를 설정한다. Qt와 MLX Python 환경은 패키지에 복사하지 않는다.

```cmake
find_package(iiLocalLLM 0.22.0 CONFIG REQUIRED)
target_link_libraries(your_app PRIVATE iiLocalLLM::iiLocalLLM)
```

공개 헤더, 공유 라이브러리, CMake package, iiLocalLLMD, iillm, 호환 daemon 이름, MLX worker, 기본 registry, 문서와 외부 라이선스 고지를 설치한다. HTTP에는 고정한 cpp-httplib 단일 헤더를 내부적으로 사용하며 별도 HTTP 런타임 설치나 네트워크 다운로드가 필요 없다. 설치된 daemon은 상대 경로로 worker를 찾는다. 커스텀 datadir에는 --mlx-worker를 지정한다.

기본 CTest는 레거시 API, 서비스·IPC, 모델 상주·LRU·만료·메모리 거절, HTTP JSON/SSE·취소·제한, 영속 카탈로그·manifest·무결성, CLI의 daemon 통신·loopback pull·실패·취소·재시작, 하드웨어 자동 선택·CPU 재시도, MLX 캐시·장치 정책을 검증하며 인터넷이나 실제 모델이 필요 없다. IPC와 HTTP 테스트는 loopback 소켓을 사용한다. 실제 추론 테스트는 로컬 모델을 명시한 경우에만 등록한다.

```sh
cmake -S . -B build -DIILOCALLLM_WITH_LLAMA=ON \
  -DIILOCALLLM_TEST_GGUF="/absolute/path/test.gguf" \
  -DIILOCALLLM_TEST_CHAT_GGUF="$PWD/build/chat/Models/qwen2.5-0.5b-instruct-q4/model.gguf" \
  -DIILOCALLLM_TEST_MLX_MODEL="/absolute/path/mlx-model" \
  -DIILOCALLLM_MLX_PYTHON="$PWD/build/mlx-env/bin/python"
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

`IILOCALLLM_TEST_CHAT_GGUF`는 위 `pull qwen2.5:0.5b`로 설치한 공식 모델 파일을 지정한다. UNIX의 `iiLocalLLM.chat`은 네트워크 없이 임시 카탈로그에 해당 파일을 해시 검증하여 설치하고, 내장 template으로 실제 대화한다. 터미널의 두 번째 턴 이름 기억·KV 재사용, `/clear`의 이력·KV 초기화와 system prompt 보존, 매 턴 JSON 즉시 출력, CLI/HTTP의 단일 모델 공유, HTTP JSON/SSE 완료까지 검증한다. 가중치는 테스트 실행 전에 준비해야 하며 CTest는 다운로드하지 않는다.

GGUF smoke는 chatml을 명시하여 경량 테스트 모델도 사용한다. 임시 패키지 설치·검증·URI 로드부터 서비스의 자동 장치 선택, 실제 추론·두 번째 턴 KV 재사용·초기화·취소·재생성·제거까지 검증한다. 독립 daemon 테스트는 원본 패키지 경로를 바꾸고 서비스를 재시작하여 카탈로그의 지속성을 확인한다. 별도 어댑터 적합성 테스트가 실제 CPU 추론도 검증한다. 모델 답변 품질이나 모든 아키텍처 호환성 평가는 아니다. 환경과 결과는 [docs/Verification.md](docs/Verification.md)에 기록한다.

## 라이선스

자체 코드·문서는 **AGPL-3.0-only**이며 [LICENSE](LICENSE)를 따른다. 외부 의존성과 가중치는 각각의 라이선스를 유지한다. 도입 검토와 출처는 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)에 있다.

0.11.0에서 작업·Todo 영속 저장, 의존 관계와 원자적 선점, C++/에이전트 API/MCP/CLI 연결을 추가했다. [작업 관리](docs/Tasks.md)에 입력·저장·권한·재시작 계약을 기록한다. 현재 ABI는 0.20이며 소비자는 새 헤더와 라이브러리로 함께 다시 빌드한다.

0.13.0은 대화별 영속 입력 큐와 now/next/later 전달, 연산 단위 협력 중단·도구 이력 복구를 제공한다. C++·인증된 API·MCP·CLI로 제어하며 유휴 큐는 명시적으로 runQueued를 호출한다. 자동 기동과 셸 완료 알림 생산은 남아 있다. [입력 큐](docs/InputQueue.md), [검증 기록](docs/Verification.md)을 참조한다. CancellationToken 레이아웃과 Engine/API 옵션이 바뀌었으므로 소비자는 0.13 헤더와 라이브러리로 함께 다시 빌드한다.

0.13.1은 네이티브 에이전트의 도구 관측에 text·data·is_error를 함께 전달하여 구조화 결과와 부분 완료·오류 상태의 누락을 수정한다. 같은 관측을 토큰 예산에 반영하며 원문과 호스트 metadata 경계를 보존한다. [에이전트 계약](docs/AgentHarness.md), [검증 기록](docs/Verification.md)을 참조한다.

0.13.2는 MCP 요청의 기한·경과 시간·전송 계층 제출 여부를 C++ 오류에 보존하고 연결 관리자에 실패 단계별 진단을 추가한다. 설정 파일로 서버별 초기화·요청 기한을 지정할 수 있다. 초기화 기본 기한은 유지하며 재현된 시간 초과를 해결 완료로 처리하지 않는다. [MCP 계약](docs/MCP.md), [도구 발견](docs/ToolDiscovery.md), [검증 기록](docs/Verification.md)을 참조한다.

0.14.0은 로컬 SKILL.md 탐색·인자 치환·인라인 주입과 인증 API·MCP·CLI 호출을 추가한다. EngineOptions와 RunRequest 확장으로 C++ 소비자는 다시 빌드해야 한다. 0.17.0은 fork 실행, 0.18.0은 호출 범위의 allowed-tools와 인자 권한 규칙을 추가한다. 훅·설치 등의 기능은 남아 있다. [스킬 계약](docs/Skills.md)을 참조한다.

## C++ 서브에이전트 (0.17.0)

별도 대화의 위임 실행·백그라운드·부모 컨텍스트 분기·재개, 파일 기반 프로파일·스킬 사전 로딩·자식 생명주기 훅과 인증 API·MCP·CLI를 제공한다. 프로파일 계약은 [AgentProfiles.md](docs/AgentProfiles.md)에 기록한다. 계약과 남은 범위는 [Subagents.md](docs/Subagents.md), 실제 모델 결과는 [Verification.md](docs/Verification.md)에 기록한다.

0.17.0에서는 `context: fork` 스킬을 같은 API·MCP·CLI 호출로 별도 자식에서 실행한다. 직접 호출은 자식 결과를 반환하고 모델의 `Skill` 호출은 후속 부모 턴에 결과를 전달한다. 본문 분리·권한·모델·사용량·큐 입력과 참조 차이는 [Skills.md](docs/Skills.md)의 별도 자식 실행 계약을 따른다.

0.18.0은 스킬 권한의 호출 수명, 권한 판정 전에 고정한 실행 스냅샷, 파일·Bash·Skill·Agent·MCP 도구의 권한 규칙을 제공한다. 지원 문법과 참조 차이는 [Permissions.md](docs/Permissions.md)를 따른다.

0.19.0은 사용자·프로젝트·로컬·호스트·관리 파일의 권한 설정 계층과 출처별 파일 규칙, 실시간 재로딩, 인증 API·CLI·MCP 조회를 제공한다. `PermissionPolicy` 가상 함수와 `PermissionRule` 레이아웃이 바뀌어 소비자는 ABI 0.19로 다시 빌드한다. 지원 범위와 명시적인 차이는 [권한 설정](docs/PermissionSettings.md)에 기록한다.

0.20.0은 추가 작업 디렉터리를 파일 도구·Bash 리다이렉션·자식 에이전트에 연결한다. 설정과 `--agent-add-dir`/`--add-dir`, 실시간 철회, canonical 대상 바인딩, 호스트 비공개 파일 보호와 API·MCP 조회를 제공한다. 이 단계의 ABI는 0.20이다. [추가 작업 디렉터리](docs/WorkingDirectories.md)에 사용법과 남은 참조 차이를 기록한다.

0.21.0은 C++ 외부 명령 훅을 도구·모델·종료·압축·작업·자식 생명주기에 연결한다. `--agent-hooks`/`--hooks`의 명시적 호스트 설정, JSON stdin, 입력 변경과 일회 권한, 차단·중단, 병렬 실행·취소·진단을 제공한다. 0.21 당시 ABI는 0.21이며 전체 생명주기 및 HTTP·prompt·agent 훅은 남아 있다. [명령 훅](docs/CommandHooks.md)에 사용법과 참조 차이를 기록한다.

0.22.0은 UserPromptSubmit과 SessionStart를 직접 입력·스킬·큐·세션 재개·압축에 연결한다. 차단 판정을 원본에 보존하고 일반 모델 문맥에서 제외하며, 큐의 준비와 확인을 분리해 재진입·취소·저장 후 복구를 지원한다. 현재 ABI는 0.22이다. [입력·세션 생명주기](docs/InputLifecycle.md)에 설정과 보존·실패 계약을 기록한다. 전체 하네스 및 앱 배포 완료와는 구분한다.
