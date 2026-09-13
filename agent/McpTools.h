#pragma once
#include "Tools.h"
#include "../mcp/Client.h"

namespace iiLocalLLM::agent {
struct McpToolOptions {
    QString serverName;
    QString appId;
    // MCP annotations are hints from the remote server. The host must explicitly
    // trust that server before they influence automatic permission or scheduling.
    bool trustAnnotations = false;
};
// Discover a full list before exposing tools. Returned handlers retain the client.
// Add these to a fresh registry (or at a host-controlled registry update boundary).
IILOCALLLM_EXPORT QList<Tool> mcpTools(std::shared_ptr<mcp::StdioClient>, const McpToolOptions&,
    CancellationToken = {});
}
