# Inference architecture (0.56)

The public Service schedules model/catalog operations and generation through
bounded native worker queues. ModelManager owns runtime/model loading; residency
and context caches remain below the service. llama.cpp/libmtmd implement native
text and media inference; the explicitly selected MLX adapter retains its Python
worker. Hardware/resource discovery and native media budgets remain here.

IPC and HTTP expose inference and a generic RpcHandler extension interface.
They neither construct an agent nor link iiDecision. Agent ownership, tools,
sessions, MCP, hooks, routing and decision policy belong to iiMultimodalAgent.
ServiceModel in that module adapts this Service without a reverse dependency.

Public contracts remain C++23 and the existing Qt Core/Network API. This module
extraction does not claim Qt removal. File creation retains iiFileProvider's
native/compatibility contracts. Installed inference consumers verify that neither
agent nor iiDecision targets are imported by find_package(iiLocalLLM).

See [module extraction](ModuleExtraction.md), [inference model management](ModelManagement.md),
[HTTP](HTTP.md), [IPC](IPC.md), and the [agent migration](../../iiMultimodalAgent/docs/Migration.md).
