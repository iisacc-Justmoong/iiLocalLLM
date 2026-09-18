#pragma once
#include "Tools.h"

namespace iiLocalLLM::agent {
struct ProjectMemoryOptions {
    QString directory; // Host-owned base, with a separate SHA-256 scope per canonical workspace.
    int maxIndexLines = 200;
    int maxIndexBytes = 25000; // UTF-8 bytes, never split a code point.
    int maxFileBytes = 1024 * 1024;
    int maxFiles = 200;
    int maxScannedEntries = 4096;
    int maxScanBytes = 4 * 1024 * 1024;
    int lockTimeoutMs = 5000;
};
// Persistent project notes. They are untrusted context, not permission grants.
// Read/Write/Edit/Glob/Grep keep their normal workspace behavior and route only
// absolute paths inside this owner's exact project memory directory here.
class IILOCALLLM_EXPORT ProjectMemory {
public:
    explicit ProjectMemory(ProjectMemoryOptions);
    ~ProjectMemory();
    QString directory(const QString& workspace, const CancellationToken& = {}) const;
    // Bounded index state only, without scanning topic files.
    QJsonObject index(const QString& workspace, const CancellationToken& = {}) const;
    Message message(const QString& workspace, const CancellationToken& = {}) const;
    QJsonObject snapshot(const QString& workspace, const QString& query = {}, const CancellationToken& = {}) const;
    // Uses the same read-before-edit cache as the routed native file tools.
    // The hash must match before any read observation is recorded.
    ToolResult readForContext(const QString& path,const QString& sha256,const ToolContext&,int maxLines,int maxBytes) const;
    void bindWorkspaceTools(ToolRegistry&) const;
    Tool forgetTool(bool deferred = true) const;
private:
    class Impl;
    std::shared_ptr<Impl> d;
};
}
