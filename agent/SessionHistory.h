#pragma once
#include "Tools.h"

namespace iiLocalLLM::agent {
struct SessionHistoryOptions {
    int maxSessions = 1024; // Bounds directory entries as well as matching sessions.
    int maxScanBytes = 16 * 1024 * 1024; // Per page, including immutable header reads.
    int maxRecordBytes = 4 * 1024 * 1024;
    int maxRecordsPerPage = 20000;
    int maxSnippetCharacters = 1024; // UTF-16 units, without splitting surrogate pairs.
    int maxCursors = 16;
    int cursorLifetimeMs = 300000;
};
// Read-only access to one host-owned SessionStore. It never acquires a writer
// lease or repairs an interrupted tail. Search is literal, not semantic/regex.
// Cursors are short-lived, single-use and bound to this instance + owner session.
class IILOCALLLM_EXPORT SessionHistory {
public:
    explicit SessionHistory(QString sessionsDirectory, SessionHistoryOptions = {});
    QJsonObject search(const QString& ownerSessionId,const QString& workspace,
        const QJsonObject& arguments,const CancellationToken& = {}) const;
    Tool tool(bool deferred = false) const;
private:
    class Impl;
    std::shared_ptr<Impl> d;
};
}
