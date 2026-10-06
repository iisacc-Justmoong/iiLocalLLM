# iiLocalLLM

A MVP module that provides local LLM conversations and C++ agent execution. The completed scope of 0.52.0 covers model loading, conversations, streaming, sessions, cancellation, tool execution, C++/native IPC/HTTP/MCP integration, and local plugin execution. For execution instructions, follow the CLI examples below; for the execution layers, follow [AgentHarness.md](docs/AgentHarness.md); for final verification with real models and a fresh installation, follow [Verification.md](docs/Verification.md).

[HarnessParity.md](docs/HarnessParity.md)is reference data for comparison with the entire harness. Marketplace, running plugin updates, remote harness, and equivalence with all apps and platforms are not included in this MVP completion scope. The incomplete items in the version-specific records below are the scope at the time of that version.

0.54.0 adds [quantitative host value admission using iiDecision](docs/DecisionGate.md). The harness evaluates success/failure probabilities and expected gain minus loss and cost before model work and tool execution. Missing or low-value host assessments return `deferred`. Eligible tool proposals are ranked by expected value; `decision_input` returns accept trusted host facts, and `decision` returns expose the actual assessment. This policy is enabled by default. Hosts migrating older workflows may explicitly set `decision.enabled=false`. Consumers must rebuild against the 0.54 headers and library.

0.53.0 adds [procedure return control and child request deduplication](docs/Procedures.md). Hosts observe named execution stages and may continue, replace validated input/model/tool/completion returns, or cancel before the next stage consumes the return. Authenticated IPC/HTTP and connection-bound MCP controls remain available while a run waits. Repeated identical new-child requests within one parent run reuse the existing child; explicit idempotency keys preserve the initial receipt across host restarts. Consumers must rebuild against the 0.53 headers and library.

0.52.0 adds C++ [local plugin repository and running](docs/Plugins.md). Installed revision's skills, commands, agents, hooks, MCP, and LSP are connected to the existing runner, and apps check configuration status via authentication API and MCP. Updates and removals preserve previous cache and data and apply on the next host start.

0.51.0 updates the [local team](docs/Teams.md)tool schema to match role and actual unresolved termination requests. Only the leader can request termination, and only the corresponding team member can send approval or rejection tied to the request ID.

0.50.0 corrects the conditional message input schema and Qwen3.5 native XML tool syntax. It aligns required summaries, string/object distinction, argument order, and dialogue history encoding to the same contract. It also provides automatic preemption of shared tasks in starting and idle states, along with priority-based message processing.

0.47.0 adds session branching](docs/SessionFork.md), including [archived files and checkpoints. It connects the dialogue path to the new owner and creates a parent backup and independent copy. It applies to context branching for C++ · API · CLI · MCP and child agents.

0.46.0 adds C++   [file checkpoints and restore](docs/FileCheckpoints.md). It provides original records per user message, new file deletion restore, session locking, work tree boundary, and permissions and authentication API · MCP ·CLI.

0.45.0 adds C++   [Jupyter notebook cell editing](docs/Notebooks.md). It provides NotebookEdit read precedence, permissions, change detection, backup, and authentication API · MCP ·CLI, and follows the work tree and compression boundary of the same session.

0.43.0 provides C++   [LSP  code exploration](docs/Lsp.md). It connects 9 operations of the host-specified language server, versioned document synchronization, per-owner session processes and diagnostics, and read permissions and API · MCP ·CLI.

0.42.0 provides anonymous retrieval, HTML conversion, caching, and local-model extraction through [WebFetch](docs/WebFetch.md).

0.41.0 adds [project memory cleanup](docs/MemoryDream.md). It provides 24 time and recent dialogue count conditions, process locking, and C++ cleanup work that maintains parent context and permissions, along with success timestamp, progress summary, and authentication API · MCP ·CLI. Automatic execution is OFF by default.

0.40.0 provides [stored conversation search](docs/SessionHistory.md). C++ `SessionSearch` divides large JSONL records for search and authenticates scope, original text location, change detection, and permissions API, MCP, and connects to CLI. [also maintains conversation end memory extraction](docs/MemoryExtraction.md).

0.38.0 provides [memory recall](docs/MemoryRecall.md), where the local model selects relevant topic notes. It connects asynchronous advance execution, context attachment, duplicate/outdated-information/size management, and authenticated API, MCP, and CLI. Storage, index, and file-tool contracts in 0.37 follow [project memory](docs/ProjectMemory.md). The C++ QuestionInbox and LVRS question-screen contracts in 0.36 follow [question UI](docs/QuestionUI.md).

0.35.0 connects options, free input, partial response, preview, and annotations to host responses with C++ AskUserQuestion. API, IPC, MCP, and input limits follow [user question](docs/UserQuestions.md).

0.34.0 provides session-based plan writing, review, and execution switching in C++. Review of actual plan files, host modification, and change conflict detection, along with API, MCP integration, follow [plan mode](docs/PlanMode.md).

0.32.0 adds a updatedMCPToolOutput that replaces the result of the successful MCP tool in the command· HTTP · C++ hook. It passes changed observations to the model· API ·MCP and distinguishes the original structured result from general tools. Settings, content format, and reference differences follow [McpOutputHooks .md](docs/McpOutputHooks.md). The entire harness is still under implementation.

0.31.0 adds a C++ agent hook. It uses actual tools in a separate conversation, judges with StructuredOutput , and applies dontAsk permissions· 50 message limits·cancellation and temporary state cleanup. API · CLI ·MCP also uses the same path. Settings and reference differences are recorded in [AgentHooks .md](docs/AgentHooks.md). The entire harness is still under implementation.

0.29.0 connects C++ HTTP/HTTPS hooks to the API, CLI, and MCP execution paths. It provides JSON decisions and input/permission changes, URL/environment allowlists, DNS address pinning and TLS validation, proxies, and cancellation/time/response limits. For configuration and differences from the reference, follow [HTTPHooks.md](docs/HTTPHooks.md). Existing normal/control response capacities are recorded in [ControlCapacity.md](docs/ControlCapacity.md), and app permission requests are recorded in [PermissionRequests.md](docs/PermissionRequests.md).

0.25.0 connects the PermissionRequest hook of the Ask tool with the structured C++ host response. After input changes, it rechecks policies and execution targets, and explicit host processing is used for persistent permission renewal. Contract and remaining scope are recorded in [PermissionRequest .md](docs/PermissionRequest.md).

0.24.0 provides initialization of C++ · API ·MCP conversations and immediate SessionStart(clear) . Background shell·child agent·completion notifications lead to new conversations. Contract and error recovery limits are recorded in [SessionClear .md](docs/SessionClear.md).

C++23, Qt 6.8.3 Core/Network-based local LLM service SDK. The current version is 0.54.0. The app uses `model://id` for models. The service manages manifests and installation files, and automatically selects the execution device based on inspected hardware at startup. Model execution is delegated to llama.cpp or MLX, and it manages sessions, prompt budget, KV cache, FIFO scheduling, streaming, and local IPC. Existing `helloWorld()` and `iiLocalLLM::iiLocalLLM` CMake targets are maintained.

The C++ stdio MCP client discovers external tools, resources, and prompts and connects them to the agent engine. The `iillm-mcp` server and C++ built-in API expose app tools and local agent execution to external MCP clients. Protocols, policies, material preservation, and current support boundaries are described in [MCP.md](docs/MCP.md) and [MCP server and app tool exposure](docs/MCPServer.md).

Connecting `agent::Api` to the same daemon's HTTP `/v1/rpc` and native IPC allows sharing per-app key authentication, persistent sessions, default transcript branching, execution events, and cancellation. It is also invoked via installed `iillm --auth-file FILE rpc METHOD [PARAMS_FILE]`. Settings, methods, lifecycles, and current limits are described in [AgentAPI .md](docs/AgentAPI.md).

0.12.0 provides actual background Bash execution on desktop POSIX, `TaskOutput` · `TaskStop` · `ShellTaskList`, execution logs, and output preservation. Authenticated `agent.shell.*`, thin CLI, and MCP control the same execution and pass the current execution state to the model's next turn. The difference between waiting cancellation and process termination, and normal termination and abnormal termination recovery is specified in [BackgroundTasks .md](docs/BackgroundTasks.md). Sources and installation headers and libraries are rebuilt together.

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

|Configuration|Operation|
| --- | --- |
| Model Manager |install/remove/list/resolve/verify/load/unload, manifest·URI·file integrity management, engine lifecycle adjustment|
| ModelCatalog / RuntimeManager |Engine-independent disk catalog / model format, runtime selection based on device, and memory lifecycle|
| Hardware / Policy |GPU vendor·VRAM·integrated memory·RAM·CPU·acceleration API check, Metal → CUDA → Vulkan → CPU policy|
| Session Manager |Per-model system/user/assistant history, initialization, termination|
| Prompt / Chat Engine |Actual tokenizer, output token reservation, removal of old completed turns|
| Context / KV Cache Manager |Per-session runtime context, LRU, count and reserved token limit|
| Scheduler |Dedicated worker threads, bounded FIFO, cancellation, future completion on termination|
| Streaming |started → delta → finished,  Unicode  processing, stop crossing chunks|
| Local API / IPC / HTTP |C++  future/handle  API , user-only Local Socket  NDJSON , localhost Chat Completions  JSON / SSE|
| Runtime Abstraction |Runtime /  RuntimeModel  /  RuntimeContext  three interfaces|

ONNX  etc. implement and register the above interfaces. Current built-in adapters are llama.cpp and MLX.  HTTP  Chat Completions implements part of the text and function tool call contract. Persistent agent sessions are provided via a separate agent API and multimodal input is not yet complete.

<a id="상세-제어-객체"></a>

## Detailed control object

Handles 391 configuration groups, from inference to training and fine-tuning, through `ParameterCatalog`, `ParameterObject`, and `ControlParameters`. It queries original types, defaults, choices, constraints, descriptions, inheritance, and source commits/hashes and exports the original JSON. The 16 common generation options are connected to actual llama.cpp/MLX requests. Training objects provide configuration validation and export. For the distinction between the investigated scope and execution support, and API/CLI examples, see [Parameters.md](docs/Parameters.md); for official sources and licenses, see [ParameterSources.md](docs/ParameterSources.md).

```sh
./build/iillm parameters trl.GRPOConfig
./build/iillm parameters peft.LoraConfig docs/examples/lora.json
./build/iillm run qwen2.5:0.5b "Hello" --options docs/examples/generation.json
```

0.10.0  automatically discovers the authenticated  MCP  address of the running local app and forwards the call to the actual  QObject  controller.  Society ·Dreamscapes' desktop tools and cancel/lifetime contracts are recorded at  [local app connection](docs/LocalApplications.md).  0.10  ABI at the time was  0.10 . Current version consumers rebuild together with new headers and libraries.

0.9.0  provides  MCP  server connection and recovery and per-conversation  `ToolSearch` . Status is recovered even after resume/branch/compress and re-searches on schema/connection changes.  `agent.mcp.status`  and  `iillm agent mcp`  are used to query status.  [tool search and  MCP  settings](docs/ToolDiscovery.md)record usage and limits.

Fixed Qwen2.5   0.5B model failed continuous call verification after search. In this model,  MCP  tool path providing tools from the start  `--agent-mcp-eager`  /  `--mcp-eager`  can be used. Detailed observation results are recorded at  [Verification.md](docs/Verification.md).

0.8.0  provides authenticated  C++   MCP   HTTP  server and  `iillm-mcp --http-port` . Supports per-app sessions,  SSE  resume of original request stream, cancel/reverse request, and private auth/status folders.  [MCP   HTTP  server](docs/MCPHTTPServer.md)is referenced.

0.7.0 maintains the common C++   MCP client introduced there, along with Streamable HTTP connection·authentication headers· SSE recovery·session reinitialization. It references [MCP   HTTP](docs/MCPHTTP.md).

0.6.0 provides actual native token measurement, automatic tool result reduction·conversation summaries of multiple bundles, compression checkpoints preserving the original, and manual C++ / API / MCP calls. ABI is 0.6 due to public structures and Model virtual interface changes and is rebuilt together with new headers·libraries. It references [conversation compression](docs/Compaction.md)and [project guidelines](docs/ProjectContext.md). The entire harness and product app integration are still tracked in the correspondence table.

<a id="빌드"></a>

## Build

CMake 3.24 or later, a C++23 compiler, and **Qt 6.8.3** Core/Network and an installed **iiDecision 0.0.3 or later** CMake package are required. The probability/value core uses standard C++23 without Qt. CMake consumers also discover the dependencies enabled in their iiDecision installation. Tests also use Qt Test and Python 3. Headers and implementations are placed together without a separate source include directory.

The default build includes llama.cpp, so GGUF models can be run immediately. The archive of fixed commits is verified and statically linked to the shared SDK as SHA-256. A configuration that uses only the service contract without an engine is selected as `-DIILOCALLLM_WITH_LLAMA=OFF`. Model weights are not downloaded during build. All build outputs are placed under build/.

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

Specify the existing llama.cpp source through `IILOCALLLM_LLAMA_SOURCE_DIR`. The verified commit is `5202104b59ada9005db079eea43882a2b7bf5802`. C API compatibility with other commits requires separate verification. The single-call limit on JSON tool calls and the Qwen3.5 XML tool arguments are [patched at build time](docs/AgentHarness.md). The original checkout is not modified; an external source whose patch locations have changed is reported as a configuration error.

A new build configuration detects the installed CUDA Toolkit and Vulkan SDK, including glslc and SPIRV-Headers, to determine whether the corresponding llama.cpp module builds by default. Metal follows the default build on Apple platforms. CMake values such as GGML_CUDA/GGML_VULKAN determine the modules included in the distribution package; they are not an API for choosing the app's execution device. Existing CMake cache values are preserved.

## C++ API

Install the model package first. `manifest.json` has the following minimum fields. It generates the size of the entire file and SHA-256 list during installation and re-verification before loading.

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

The default entry file for GGUF is `model.gguf`. If another name is used, `entry_point` is specified. MLX uses `format: "mlx"` and default `entry_point: "."`. capabilities is a package declaration and does not add tool call APIs. Catalog API, schema, and validation scope are in [docs/ModelManagement.md](docs/ModelManagement.md).

The following is an example of C++ for a daemon or embedding service host. When multiple apps share a model, they access a single iiLocalLLMD via Native IPC /HTTP. The modelsDirectory specified by the host is the repository location, and there is no file path in client inference requests.

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
    // Cancellation through generation.cancel() is also possible from another thread.
    auto result = generation.result.get();
    service.closeSession(session).get();
    service.unloadModel("model://qwen3-8b-q4").get();
    return result.errorCode == iiLocalLLM::ErrorCode::None ? 0 : 1;
}
```

Failure of the control method is passed to `Error` in the future. chat also returns errors and cancellations as GenerationResult and passes terminal events to one time. For queue rejection or cancellation before execution, started/delta may be absent.

The callback executes in the worker thread and immediately passes the queue rejection to the calling thread. Qt UI Renewal is passed as queued invoke. Do not wait for the service future or destroy the service inside the callback. The callback must return quickly. Service destruction cancels in-progress and pending tasks and waits for worker termination. Do not perform destruction and new method calls simultaneously.

<a id="cli와-공유-daemon"></a>

## CLI and shared daemon

`iillm` is a Native IPC client that links only Qt Core/Network. It does not link the inference engine or run the model on behalf of the daemon.

```sh
export IILLM_SOCKET="$PWD/build/llm.sock"
./build/iiLocalLLMD --models-root "$PWD/build/chat/Models" --socket "$IILLM_SOCKET" --http-port 8080
# In another terminal, also change to the repository directory and configure this.
export IILLM_SOCKET="$PWD/build/llm.sock"
./build/iillm pull qwen2.5:0.5b
./build/iillm models
./build/iillm run qwen2.5:0.5b --system "You are a helpful assistant." --max-tokens 256
./build/iillm ps
```

The startup model `qwen2.5:0.5b` is a Q4_K_M GGUF of the official Qwen2.5-0.5B-Instruct, with a download size of 491,400,032 bytes. It resolves to `model://qwen2.5-0.5b-instruct-q4`. The existing `qwen3:8b` is also retained. Pull makes the service download the pinned official GGUF and install it after SHA-256 verification. After download, conversation runs without the internet or an external inference service.

Send a turn by pressing Enter in the terminal and initialize the conversation history and KV while maintaining the system prompt at `/clear`. `/bye`, `/exit`, and EOF are termination signals, while Ctrl+C cancels generation and terminates after session cleanup. `--temperature 0` selects greedy generation. `--max-tokens` is the answer length upper limit, so long answers may be truncated. The answer quality of small models depends on the model capacity.

Requests from CLI · GUI · Python · and the agent reuse the same resident model. If the model is not in memory, the service checks the budget and available RAM, unloads the idle LRU model, and then loads it. The default keep_alive is 5 minutes, and it is 0 for RAM 8 GiB or less. It can be released immediately upon request via `iillm run qwen2.5:0.5b --keep-alive 0`.

```sh
./build/iillm run qwen2.5:0.5b "What is 2 + 2?" --temperature 0
curl -N http://127.0.0.1:8080/v1/chat/completions \
  -H 'Content-Type: application/json' \
  -d '{"model":"qwen2.5:0.5b","messages":[{"role":"user","content":"Hello!"}],"stream":true,"max_tokens":128}'
```

[CLI usage](docs/CLI.md)and [residency/memory policy](docs/Residency.md)describe the installation sources, configuration, and failure/cancellation contracts. The existing `iilocal-llm-service` executable is also retained.

<a id="ipc-서비스"></a>

## IPC service

Boot using models.json containing only the URI after installing the local package. The default for `--models-root` is Models in the working directory. For stable service deployment, always specify the repository path.

```json
{"models":[{"model":"model://qwen3-8b-q4","context_tokens":2048,"options":{"threads":4}}]}
```

```sh
./build/iilocal-llm-service --models-root "$PWD/build/chat/Models" --install /absolute/path/qwen-package
./build/iilocal-llm-service --models-root "$PWD/build/chat/Models" \
  --config models.json --socket "$PWD/build/llm.sock"
```

You can start without a configuration file and install via models.install or models.pull. models.install is a local copy, while models.pull uses a verified remote source from the service registry. Installed models are automatically loaded on the first generation request. Since each repository maintains an ownership lock on one service, install via IPC for running services. Installation files are retained after restart, while the load status and sessions are recreated. Existing sockets are not automatically deleted. SIGINT /SIGTERM cleans up connections and inference. Due to the path length limit of macOS Unix sockets, short paths are used. Protocol and client examples are in [docs/IPC.md](docs/IPC.md).

## localhost HTTP

Native IPC and HTTP are enabled simultaneously in the same service. IPC uses Unix Domain Sockets on macOS/Linux and Windows Named Pipes. HTTP binds only to 127.0.0.1.

```sh
./build/iilocal-llm-service --models-root "$PWD/build/chat/Models" \
  --config models.json --socket "$PWD/build/llm.sock" --http-port 8080
curl http://127.0.0.1:8080/v1/chat/completions \
  -H 'Content-Type: application/json' \
  -d '{"model":"model://qwen3-8b-q4","messages":[{"role":"user","content":"Hello"}],"max_tokens":128}'
```

`--http-port 0` automatically selects a free port and outputs the address. If only `--http-port` is specified, it is dedicated to HTTP, and if both are omitted, it starts with the per-user default Native IPC endpoint. It provides GET /health, GET /v1/models, and POST /v1/chat/completions. `stream: true` returns SSE delta and [DONE].

HTTP uses Service::complete to run existing chat execution paths and the scheduler. It processes past conversations and the latest user input in temporary sessions, and cleans up sessions and KV upon completion, error, or connection cancellation. The model loaded from Native IPC is used directly by HTTP, and the history of long-term IPC sessions is maintained. The supported text Chat Completions scope, Python examples, and error/resource limits are in [docs/HTTP.md](docs/HTTP.md).

## MLX

MLX is an optional dependency for Apple Silicon. A separate Python environment and local model directory are required. The verified version is mlx-lm 0.31.3 / mlx 0.32.2.

```sh
python3 -m venv build/mlx-env
build/mlx-env/bin/python -m pip install -r src/runtimes/mlx-requirements.txt
./build/iilocal-llm-service --socket "$PWD/build/llm.sock" \
  --mlx-python "$PWD/build/mlx-env/bin/python" \
  --mlx-worker "$PWD/runtimes/mlx_worker.py"
```

Install a package including tokenizer/config/safetensors and MLX manifest, and load via URI to select MLX. The app does not specify the runtime. The runtime applies trust_remote_code =False and does not perform network downloads. Weights are prepared separately. One process per model Python is maintained, and KV states are separated per session. Processes are terminated on cancellation, timeout, or consumer error, and the model is reloaded on the next request. At this time, other sessions of the same model also lose their physical KV cache, but conversation history is maintained. MLX default device and generation stream are initialized to the service-specified Metal /CPU. The embedding service's Python /worker deployment path is specified as the second MlxRuntimeOptions argument of the Service creator.

<a id="실행-정책"></a>

## Execution Policy

The app passes the model URI / contextTokens /options of ModelLoadRequest. ModelManager interprets the URI and validates the manifest, full file hash, and format structure, then passes the actual path and format to the internal ModelSpec. GGUF is processed by llama.cpp, and the package is processed by MLX. Built-in runtime registration is also handled by the service. Since the model format is separate from device selection, GGUF runs on Apple Silicon via llama.cpp /Metal.

|Hardware and availability conditions|Hardware and Availability Conditions|
| --- | --- |
|Apple Silicon + a working Metal| Metal |
|NVIDIA GPU + CUDA operating| CUDA |
|AMD /Intel GPU + Vulkan operating| Vulkan |
|No GPU path available in the model runtime| CPU |

GPU Acceleration must also be compiled and supported for that engine. Within the same priority, sort by confirmed memory size and device id. If model load or llama.cpp GPU context pre-allocation fails, retry on CPU. Do not retry for invalid input or cancellation. If put into runtime/ backend/device/gpu_layers model request, it is invalid_argument . Execution results are queried via ModelInfo .execution and models.loaded, and information checked at boot via hardware.get or `build/iilocal-llm-service --hardware` . Field meanings and failure ranges are in [docs/HardwarePolicy.md](docs/HardwarePolicy.md).

contextTokens = 0 or omitted uses manifest. context_length , ServiceOptions . defaultContextTokens (default: 2048) , and uses the minimum among cache token budgets. If the specified context exceeds the manifest upper limit, it is context_overflow . Explicit unload/remove calls after closing all sessions of that model. Automatic LRU /expiration releases KV and weights while preserving session history.

Default limits are 64 queued jobs, 4 models, 128 sessions, 4 cached contexts, and 16,384 total reserved context tokens. Change them through ServiceOptions. Control and generation run FIFO in one worker. A subsequent request in the same session reads the latest history after the preceding successful response is stored. GPU usage across multiple Service instances is managed separately.

ModelResidencyManager checks weight reservation, KV , runtime slack bytes, and latest available RAM. Upon reaching count/byte upper limits, release idle LRU models, and upon expiration, the worker releases without new requests. Active requests are protected and idle session history is preserved. Estimates are not hard upper limits of process RSS. Detailed formulas and limits are in [Residency.md](docs/Residency.md). Batch inference is not provided.

Check context with the actual prompt token count and maxTokens total. If exceeded, remove old user/assistant pairs but preserve system and the latest user. If still exceeded, it is context_overflow . Save compressed history and assistant only on success. On error or cancellation, retain existing history and discard KV. Partial text already sent remains in the result but is not stored in history. Token usage for intermediate tokens on error or cancellation may be incomplete.

Templates not supported by llama.cpp's built-in chat template formatter are errors. For GGUF without a template, specify options. chat_template (e.g., chatml). Do not automatically apply ChatML to arbitrary models. MLX uses the model tokenizer's chat template. Models where partial KV removal is impossible reconfigure the cache.

0.12.1 passes a `enable_thinking` boolean for model loading to the native Jinja template. Explicit settings apply together to both general chat and structured tool chat. Inference-only returned responses are distinguished as separate errors and are not promoted to executable tools or final answers. [records the control and verification scope in native inference mode](docs/NativeThinking.md).

llama.cpp provides model loading option `tool_grammar` (boolean, default true) for structured tool generation. false disables syntax constraints at generation time while maintaining schema and permission checks before execution. Multi-field arguments and per-model operational conditions are described in [Tasks.md](docs/Tasks.md).

Stop strings retain partial matches between chunks and are excluded from output upon completion. KV cut by stop may differ from history and is discarded. Generation options are maxTokens, temperature, topP, topK, seed, and stop. Identical seed does not guarantee identical output across different runtimes.

Structure and extension contracts are in [docs/Architecture.md](docs/Architecture.md).

<a id="설치와-검증"></a>

## Installation and verification

The default installation path is $ HOME /.local/ SDK / iiLocalLLM. install.sh also verifies separate projects that consume installed packages after build and CTest installation. The workspace staging path can be specified.

```sh
IILOCALLLM_WITH_LLAMA=ON INSTALL_PREFIX="$PWD/build/stage" ./install.sh
```

Omitting IILOCALLLM_WITH_LLAMA maintains the existing CMake selection, and the default for the new configuration is ON. It activates build/ `IILOCALLLM_WITH_LLAMA=ON ./install.sh` which were previously configured as OFF. The path is set using INSTALL_PREFIX, QT_PREFIX_PATH, CMAKE_PREFIX_PATH. Qt and MLX Python environments are not copied to the package.

```cmake
find_package(iiLocalLLM 0.54.0 CONFIG REQUIRED)
target_link_libraries(your_app PRIVATE iiLocalLLM::iiLocalLLM)
```

Install public headers, shared libraries, CMake package, iiLocalLLMD, iillm, compatible daemon name, MLX worker, default registry, documentation, and external license notice. HTTP internally uses a fixed cpp-httplib single header and requires no separate HTTP runtime installation or network download. The installed daemon finds the worker via relative path. Specify --mlx-worker for custom datadir.

Default CTest verifies legacy API, service and IPC, model hosting and LRU expiration and memory rejection, HTTP JSON / SSE cancellation and limit, persistent catalog, manifest, and integrity, CLI daemon communication, loopback pull, failure, cancellation, and restart, hardware auto-selection and CPU retry, MLX cache and device policy, without requiring internet or actual models. IPC and HTTP tests use loopback sockets. Actual inference tests are registered only if a local model is specified.

```sh
cmake -S . -B build -DIILOCALLLM_WITH_LLAMA=ON \
  -DIILOCALLLM_TEST_GGUF="/absolute/path/test.gguf" \
  -DIILOCALLLM_TEST_CHAT_GGUF="$PWD/build/chat/Models/qwen2.5-0.5b-instruct-q4/model.gguf" \
  -DIILOCALLLM_TEST_MLX_MODEL="/absolute/path/mlx-model" \
  -DIILOCALLLM_MLX_PYTHON="$PWD/build/mlx-env/bin/python"
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

`IILOCALLLM_TEST_CHAT_GGUF` is designated as the official model file installed in the above `pull qwen2.5:0.5b`. UNIX's `iiLocalLLM.chat` hashes and verifies the file in a temporary catalog without a network and actually converses using a built-in template. The terminal's second turn name memory and KV reuse, `/clear` history and KV initialization and system prompt preservation, per-turn JSON immediate output, CLI/HTTP single model sharing, and verification until HTTP/JSON/SSE completion are performed. Weights must be prepared before test execution and CTest is not downloaded.

GGUF smoke explicitly specifies chatml and also uses a lightweight test model. From temporary package installation and verification and URI load to the service's automatic device selection, actual inference, second turn KV reuse, initialization, cancellation, regeneration, and removal are verified. Independent daemon tests check catalog persistence by changing the original package path and restarting the service. A separate adapter suitability test also verifies actual CPU inference. Model answer quality or all architecture compatibility evaluation is not performed. Environment and results are recorded in [/docs/Verification.md/](docs/Verification.md).

<a id="라이선스"></a>

## License

Own code and documentation are **/AGPL-3.0-only**and follow [/LICENSE/](LICENSE). External dependencies and weights maintain their respective licenses. Introduction review and sources are in [/THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

0.11.0 added work and Todo persistent storage, dependency and atomic preemption, and added C++/agent API/MCP/CLI connections. [work management](docs/Tasks.md)records input, storage, permission, and restart contracts. Consumers rebuild together with the current version's header and library.

0.13.0 provides per-conversation persistent input queues and now/next/later delivery, and operation-unit cooperation interruption and tool history recovery. Controlled via C++·authenticated API·MCP·CLI, and idle queues are explicitly invoked via runQueued. Automatic startup and shell completion notification production remain. Refer to [input queue](docs/InputQueue.md), [verification record](docs/Verification.md). CancellationToken layout and Engine/API options have changed, so consumers rebuild together with 0.13 header and library.

0.13.1 delivers text, data, and is_error together to the native agent's tool observation to correct omissions in structured results and partial completion/error states. The same observation is reflected in the token budget while preserving the original text and host metadata boundaries. Refer to [agent contract](docs/AgentHarness.md), [verification record](docs/Verification.md).

0.13.2 preserves the deadline, elapsed time, and whether the request was submitted to the transport layer for MCP requests in C++ errors, and adds diagnostics for each failure stage to the connection manager. Configuration files can specify initialization and request deadlines per server. The default initialization deadline is retained, and reproduced timeouts are not treated as resolved. See the [MCP contract](docs/MCP.md), [tool discovery](docs/ToolDiscovery.md), and [verification records](docs/Verification.md).

0.14.0 adds local SKILL .md search·argument substitution·inline injection and authentication API · MCP · CLI calls. With EngineOptions and RunRequest extensions, C++ consumers must rebuild again. 0.17.0 adds fork execution, 0.18.0 adds allowed-tools and argument permission rules within the call scope. Hook·install and other features remain. It refers to [skill contract](docs/Skills.md).

<a id="c-서브에이전트-0170"></a>

## C++ subagent ( 0.17.0 )

It provides delegated execution of separate conversations, background, parent context branching, resumption, file-based profiles, skill pre-loading, child lifecycle hooks, and authentication API · MCP ·CLI. The profile contract is recorded in [AgentProfiles .md](docs/AgentProfiles.md). The contract and remaining scope are recorded in [Subagents.md](docs/Subagents.md), and actual model results are recorded in [Verification.md](docs/Verification.md).

0.17.0 executes the `context: fork` skill separately in a child with the same API · MCP · CLI calls. Direct calls return child results, and model `Skill` calls pass results to subsequent parent turns. Body separation, permissions, model, usage, queue input, and reference differences follow the separate child execution contract of [Skills.md](docs/Skills.md).

0.18.0 provides the call lifespan of skill permissions, fixed execution snapshots before permission judgment, and permission rules for file, Bash, Skill, Agent, and MCP tools. Supported syntax and reference differences follow [Permissions.md](docs/Permissions.md).

0.19.0 provides the permission setting hierarchy for user, project, local, host, and management files, per-source file rules, real-time reloading, and authentication API · CLI · MCP lookup. `PermissionPolicy` virtual functions and `PermissionRule` layouts have changed, so consumers must rebuild with ABI 0.19. Supported scope and explicit differences are recorded in [permission setting](docs/PermissionSettings.md).

0.20.0 connects the additional work directory to file tools, Bash redirection, and child agents. It provides settings and `--agent-add-dir` / `--add-dir`, real-time revocation, canonical target binding, host private file protection, and API · MCP lookup. The ABI of this stage is 0.20. Usage and remaining reference differences for [additional work directory](docs/WorkingDirectories.md)are recorded.

0.21.0 connects C++ external command hooks to tool·model·exit·compress·action·child lifecycle. `--agent-hooks` / `--hooks` explicit host setting, JSON stdin, input change and one-time permission, block·abort, parallel execution·cancel·diagnosis are provided. 0.21 ABI at that time is 0.21 and entire lifecycle and HTTP ·prompt·agent hooks remain. [command hook](docs/CommandHooks.md)records usage and reference difference.

0.22.0 connects UserPromptSubmit and SessionStart directly to input·skill·queue·session resume·compress. Block verdict is preserved in the original and excluded from general model context, and queue preparation and confirmation are separated to support re-entry·cancel·restore after save. ABI of that version is 0.22 . [input·session lifecycle](docs/InputLifecycle.md)records setting and preservation·failure contract. It is distinguished from complete harness and app deployment.

0.33.0 adds C++ asynchronous command hook, async declaration of first stdout line, session transfer in completion context, and asyncRewake idle execution. It provides session/connection lifespan and cancel, automatic execution count limit, and API / CLI / MCP control. Consumer rebuild is needed due to changed public structures. Child execution followed by restart·environment cache invalidation·entire lifecycle and setting merge remains partial. [asynchronous hook](docs/AsyncHooks.md)is followed.

0.34.0 adds EnterPlanMode · ExitPlanMode , per-session plan file and review hash, host modification, and execution switch concurrency control. Restart·branch·initialization and authentication API / IPC /MCP are connected to the same state. Consumers are rebuilt according to public structures and ABI   0.34 . Team leader review·interview UI ·automatic permission classification·entire app verification remains partial. [plan mode](docs/PlanMode.md)is followed.

The C++ [WebFetch](docs/WebFetch.md)in 0.42 provides URL retrieval, HTML conversion, caching, domain permissions, and local model extraction to Engine/API/MCP/CLI. WebSearch and full platform verification are separately in progress.

## Source layout

Implementation files and their headers live together under `src/`. Existing feature and platform subdirectories retain their responsibilities. Build configuration, tests, documentation, resources, and maintenance scripts remain at the project root. Configure and build using the repository-local `build/` directory.
