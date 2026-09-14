#pragma once
#include "Tools.h"
#include <QtCore/QObject>

namespace iiLocalLLM::agent {
using ObjectToolHandler = std::function<ToolResult(QObject&, const QJsonObject&, const ToolContext&)>;
// Create on QCoreApplication's thread with an owner on that same thread.
// QCoreApplication must outlive all callers. Stop tool servers before destroying it.
// The callback runs on that thread and should only read state or enqueue work.
// A cancelled/timed-out queued callback is skipped. A callback already executing
// cannot be undone; a timeout reports whether dispatch started. Never auto-retry it.
IILOCALLLM_EXPORT Tool objectTool(QObject* owner, ToolDefinition, ObjectToolHandler,
                                 int timeoutMs = 10000);
}
