#pragma once
#include "Tools.h"

namespace iiLocalLLM::agent {
struct CompactionOptions {
    bool automatic = true;
    bool clearOldToolResults = true;
    double triggerFraction = 0.85; // Fraction of context remaining after reserving the requested output.
    int keepRecentGroups = 2; // One assistant plus all its tool results is indivisible.
    int summaryMaxTokens = 512;
    int maxSummaryPasses = 16;
};
namespace detail {
void validateCompaction(const Session&, const Compaction&);
void addTranscriptTool(ToolRegistry&, const Session&);
bool needsCompaction(Model&, const ModelRequest&, const CompactionOptions&, const CancellationToken&);
// base.messages contains only ephemeral prefix instructions, not transcript messages.
// Returns a proposed checkpoint only. The caller commits it after successful hooks and cancellation checks.
Compaction prepareCompaction(Model&, const Session&, const ModelRequest& base,
    const CompactionOptions&, bool manual, const QString& instructions, const CancellationToken&,
    RunUsage&, const std::function<void(const QJsonObject&)>& progress);
}
}
