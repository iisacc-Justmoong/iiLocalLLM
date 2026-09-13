#pragma once
#include "Engine.h"
#include "../mcp/Server.h"

namespace iiLocalLLM::agent {
struct McpServerOptions {
    QString workingDirectory;
    QString artifactsDirectory;
    QString appId;
    ToolRunnerOptions tools;
    // Optional local agent tools. Each MCP connection receives its own conversation.
    std::shared_ptr<Engine> engine;
    QString model;
    QString systemPrompt;
    GenerationOptions generation;
    int maxAgentTurns = 32;
    int maxAgentSessions = 64;
};
// Exports a live registry through schema validation, policy and hooks. The export
// coordinates concurrent read-safe calls and exclusive calls across connections.
// The Engine must use its own registry, without the exported agent.run tool.
IILOCALLLM_EXPORT mcp::ServerOptions mcpServerOptions(std::shared_ptr<ToolRegistry>,
    std::shared_ptr<const PermissionPolicy>, McpServerOptions);
}
