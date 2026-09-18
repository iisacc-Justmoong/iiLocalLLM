#pragma once
#include "Types.h"
#include <QtCore/QSet>
#include <algorithm>

namespace iiLocalLLM::agent::detail {
// Incremental transcript validation; appending a result does not scan the full history.
class ProtocolState {
public:
    void check(const Message& m) const {
        auto require = [](bool ok, const char* message) { if (!ok) throw Error(ErrorCode::ProtocolError, QString::fromUtf8(message)); };
        require(!m.id.isEmpty() && !messageIds.contains(m.id), "Missing or duplicate message ID");
        if (m.role == MessageRole::Tool) {
            require(m.toolCalls.isEmpty() && !m.toolCallId.isEmpty(), "Invalid tool result");
            require(std::any_of(pending.begin(), pending.end(), [&](const auto& c) { return c.id == m.toolCallId; }), "Unmatched or duplicate tool result");
        } else {
            require(pending.isEmpty(), "Tool results must precede the next message");
            require(m.toolCallId.isEmpty(), "Non-tool message has a tool result ID");
            require(m.role == MessageRole::Assistant || m.toolCalls.isEmpty(), "Only assistants may request tools");
            require(!m.text.trimmed().isEmpty() || !m.toolCalls.isEmpty(), "Empty transcript message");
            QSet<QString> inMessage;
            for (const auto& c : m.toolCalls) {
                require(!c.id.isEmpty() && !c.name.isEmpty() && !callIds.contains(c.id) && !inMessage.contains(c.id), "Missing or reused tool call ID");
                inMessage.insert(c.id);
            }
        }
    }
    void accept(const Message& m) {
        check(m); messageIds.insert(m.id);
        if (m.role == MessageRole::Tool) {
            pending.erase(std::find_if(pending.begin(), pending.end(), [&](const auto& c) { return c.id == m.toolCallId; }));
        } else for (const auto& c : m.toolCalls) { callIds.insert(c.id); pending.append(c); }
    }
    QList<ToolCall> pending;
private:
    QSet<QString> messageIds, callIds;
};
}
