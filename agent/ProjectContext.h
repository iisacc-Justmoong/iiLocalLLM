#pragma once
#include "Types.h"

namespace iiLocalLLM::agent {
struct ProjectContextOptions {
    bool enabled = true;
    // Empty means the session's working directory. Never discover above this root.
    QString rootDirectory;
    QStringList excludes; // Root-relative globs; applies to instructions and imports.
    int maxFileBytes = 128 * 1024;
    int maxTotalBytes = 512 * 1024;
    int maxFiles = 256;
    int maxScannedEntries = 4096;
    int maxImportDepth = 5; // The entrypoint is depth zero.
    int maxTargetPaths = 128;
};
struct InstructionFile {
    QString path; // Canonical absolute path.
    QString scopeDirectory;
    QString parent; // Importing file, if any.
    QString content;
    QStringList patterns;
    QString sha256; // Unmodified file bytes, including frontmatter/comments.
    bool transformed = false;
};
struct IILOCALLLM_EXPORT ProjectContext {
    QString workingDirectory;
    QList<InstructionFile> files;
    QStringList targetPaths;
    QString fingerprint; // Ordered, rendered snapshot, including target scopes.
    Message message() const; // Ephemeral user context; never a system instruction.
    QJsonObject toJson(bool includeContent = true) const;
};
// A fresh bounded snapshot. Missing entrypoints are normal; malformed rules,
// unsafe imports and resource exhaustion fail explicitly. No home/network access.
IILOCALLLM_EXPORT ProjectContext loadProjectContext(const QString& workingDirectory,
    const QStringList& targetPaths = {}, const ProjectContextOptions& = {}, const CancellationToken& = {});
// Collect explicit run scopes and paths observed by host workspace tools.
IILOCALLLM_EXPORT QStringList projectContextPaths(const QList<Message>&);
}
