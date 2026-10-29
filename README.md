# iiLocalLLM

The service exposes a model-neutral text embedding interface for explicitly
declared embedding models, including a native GGUF runtime implementation.
See [text embeddings](docs/Embeddings.md) for model capability, pooling,
resource and verification boundaries.

C++23 local model management and inference. Version **0.56.0** separates the
agent/MCP harness into [iiMultimodalAgent](../iiMultimodalAgent/README.md).
iiLocalLLM no longer discovers, links or exports iiDecision or agent code.

It retains model manifests/catalogs, pulling/verification, hardware and residency
policy, service scheduling, context caching, native llama.cpp/libmtmd
text/image/audio/timestamped-frame inference, optional MLX, parameters, IPC, HTTP
and generic RpcHandler extension interfaces. Existing Qt 6.8.3 Core/Network public
contracts remain. Native llama.cpp runs in-process; the separately selected
existing MLX worker retains its Python requirement.

## Build and consume

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="<iiFileProvider and Qt prefixes>" \
  -DCMAKE_INSTALL_PREFIX="$HOME/.local/SDK/iiLocalLLM"
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
cmake --install build
```

`./install.sh` also verifies an independent installed consumer. Backend settings
and inference fixtures retain their IILOCALLLM_* options. No iiDecision accelerator
packages are required by an inference-only consumer.

```cmake
find_package(iiLocalLLM 0.56 CONFIG REQUIRED)
target_link_libraries(MyApp PRIVATE iiLocalLLM::iiLocalLLM)
```

Use `<iiLocalLLM.h>` and `iiLocalLLM::Service`. Inference-only iiLocalLLMD,
iilocal-llm-service and iillm remain here. Agent startup and CLI commands move to
iiMultimodalAgentD, iimagent-service, iimagent, iimagent-mcp and iimagent-plugins.

Agent users must rebuild against iiMultimodalAgent::iiMultimodalAgent, include
`<iiMultimodalAgent.h>` or `<iiMultimodalAgent/agent/Engine.h>`, and use
`iiMultimodalAgent::agent` / `iiMultimodalAgent::mcp`. Persisted schemas and wire
methods stay stable. The minor version advances because the combined ABI changes.

See [module migration](docs/ModuleExtraction.md), [architecture](docs/Architecture.md),
[model management](docs/ModelManagement.md), [hardware](docs/HardwarePolicy.md),
[parameters](docs/Parameters.md), [HTTP](docs/HTTP.md), [IPC](docs/IPC.md), and
[Windows](docs/WindowsBuildAndRun.md). Old harness documentation links now point
to their owning package; historical test counts are not new verification claims.

Pruned Nemotron Bolt GGUFs use a guarded upstream backport for per-layer expert
dimensions. [Nemotron Bolt](docs/NemotronBolt.md) documents the native CPU
regression fixture and the separate service residency-budget boundary.

Hosts can use `createLlamaRuntime(LlamaRuntimeOptions)` to copy weights into owned
memory and cap GPU layer offload without exposing device selection to model
manifests or clients. The native Metal regression covers batched top-9 expert
routing. See [Nemotron Bolt](docs/NemotronBolt.md) for the verified agent greeting,
memory limits, and residency settings.

## License

SPDX-License-Identifier: MIT

The original code and documentation in this repository are licensed under the
[MIT License](LICENSE). Third-party code, libraries, fonts, tools, and model
weights retain their respective licenses.

Related implementation contracts: [Native tool-turn contracts](docs/NativeToolTemplates.md).

Managed MLX environments build a validated Transformers import index to avoid repeated model-source scans during worker startup. See [MLX import metadata](docs/MlxImportIndex.md) for preparation, invalidation and unchanged inference deadlines.

Model integrity still verifies every byte; the macOS streaming SHA-256 path uses the system implementation. See [model integrity](docs/ModelIntegrity.md).

<!-- workspace-docs:readme:start -->
## Documentation entry points

- Human overview: this English `README.md`.
- Agent instructions: [AGENT.md](AGENT.md).
- Human documentation: [documentation index](docs/README.md).
- Structured agent documentation: [agent index](docs/agent-index.json).
<!-- workspace-docs:readme:end -->
