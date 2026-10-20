# Inference and agent module boundary

iiLocalLLM 0.56.0 provides model/runtime services independently of iiDecision and
iiMultimodalAgent. Agent sources, MCP transports, decision adapters, private
schema/context/web/permission parsers, question QML, agent executables, fixtures,
feature docs and regression tests move to
[iiMultimodalAgent](../../iiMultimodalAgent/README.md).

Its installed consumer asserts that `find_package(iiLocalLLM)` imports neither
iiDecision nor iiMultimodalAgent. ServiceModel adapter tests move with the agent;
the service suite keeps model/context/conversation/transport tests independently.

Native media/projectors, modality masks, llama.cpp and MLX remain in inference.
The agent owns model selection, iiDecision evaluation, tools and transcript
semantics. Windows metadata and SDK install discovery follow this direction.

Rebuild consumers using [the migration table](../../iiMultimodalAgent/docs/Migration.md).
Persisted session/wire schemas stay stable. Existing uncommitted history and
Windows fixes move with their implementations. This change does not remove the
inherited Qt Core/Network API or the optional MLX Python worker.

On Apple hosts, CTest and `install.sh` clear process-local `DYLD_LIBRARY_PATH`,
framework, fallback and versioned overrides. This prevents login-shell SDK paths
from replacing the packages selected by CMake. User shell profiles are unchanged.
The agent package additionally tests actual symbol-owner paths.
