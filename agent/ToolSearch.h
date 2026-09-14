#pragma once
#include "Tools.h"

namespace iiLocalLLM::agent {
struct ToolSearchOptions {
    bool enabled = true; // false publishes every tool eagerly.
    int maxResults = 20;
    int maxActiveTools = 64; // Most recently selected/used deferred definitions.
};
// Search names/descriptions/app metadata, require +terms, or select:name1,name2.
// Selecting a definition never grants permission to execute it.
IILOCALLLM_EXPORT ToolResult searchTools(const QList<ToolDefinition>&, const QString& query,
    int maxResults = 5);
namespace detail {
// Called on an isolated turn snapshot. The complete original transcript is used
// for selection recovery even after compaction; schemas are checked afresh.
void prepareToolDiscovery(ToolRegistry&, const Session&, const ToolSearchOptions&);
}
}
