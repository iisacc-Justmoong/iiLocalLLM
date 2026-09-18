#pragma once
#include "Tools.h"

namespace iiLocalLLM::agent {
struct MemoryContext {
    QString sessionId, workspace;
    ModelRequest request; // Exact parent request plus the completed assistant response.
    QList<Message> messages; // Model-visible conversation, excluding the host prompt prefix.
    std::shared_ptr<ToolRegistry> registry; // Stable native/MCP identity; definitions alone cannot grant execution.
    ToolContext context; // Only host paths/revision are retained; callbacks and cancellation are not inherited.
};
}
