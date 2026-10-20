# iiLocalLLM

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

## License

SPDX-License-Identifier: MIT

The original code and documentation in this repository are licensed under the
[MIT License](LICENSE). Third-party code, libraries, fonts, tools, and model
weights retain their respective licenses.
