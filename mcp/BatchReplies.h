#pragma once
#include "Protocol.h"
#include <map>
#include <optional>

namespace iiLocalLLM::mcp::detail {
// JSON-RPC 2025-03 batch reply grouping. The connection serializes all access.
// Notifications/reverse requests flow independently while responses wait for
// their batch peers. Both the number of pending IDs and retained bytes are bounded.
class BatchReplies {
    struct Batch { int pending = 0; QJsonArray results; qsizetype bytes = 2; };
    std::map<QString, std::shared_ptr<Batch>> requests_;
    qsizetype bytes_ = 0;
    int maxEntries_, maxBytes_, maxFrame_;
    void append(const std::shared_ptr<Batch>& batch, const QJsonObject& response) {
        const auto size = encode(response, maxFrame_).size();
        require(batch->bytes + size <= maxFrame_ && bytes_ + size <= maxBytes_, "MCP batch reply exceeds byte limit", ErrorCode::ResourceLimit);
        batch->results.append(response); batch->bytes += size; bytes_ += size;
    }
    std::optional<QJsonValue> ready(const std::shared_ptr<Batch>& batch) {
        if (batch->pending) return {};
        bytes_ -= batch->bytes;
        if (batch->results.isEmpty()) return {};
        return batch->results;
    }
public:
    struct Prepared { QList<QJsonObject> messages; std::optional<QJsonValue> immediate; };
    BatchReplies(int entries, int bytes, int frame) : maxEntries_(entries), maxBytes_(bytes), maxFrame_(frame) {}
    Prepared prepare(const QJsonArray& input) {
        require(!input.isEmpty(), "Empty JSON-RPC batch");
        auto batch = std::make_shared<Batch>(); bytes_ += 2; Prepared output;
        for (const auto& value : input) {
            const auto message = value.toObject();
            const bool method = message.contains("method");
            bool valid = value.isObject() && message["jsonrpc"] == "2.0"
                && (method ? message["method"].isString() && !message["method"].toString().isEmpty() && !message.contains("result") && !message.contains("error")
                           : message.contains("id") && (message.contains("result") != message.contains("error")));
            QString id;
            if (valid && message.contains("id")) { try { id = key(message["id"]); } catch (const Error&) { valid = false; } }
            if (!valid) { append(batch, rpcError(QJsonValue::Null, -32600, "Invalid JSON-RPC batch entry")); continue; }
            if (method && message.contains("id")) {
                require(requests_.size() < size_t(maxEntries_) && !requests_.contains(id), "MCP batch ID collision or capacity exceeded", ErrorCode::ResourceLimit);
                requests_.emplace(id, batch); ++batch->pending;
            }
            output.messages.append(message);
        }
        output.immediate = ready(batch); return output;
    }
    std::optional<QJsonValue> response(const QJsonObject& message) {
        if (!message.contains("id") || (!message.contains("result") && !message.contains("error")) || message["id"].isNull()) return message;
        const auto found = requests_.find(key(message["id"]));
        if (found == requests_.end()) return message;
        auto batch = found->second;
        // Keep membership if encoding fails: the caller can replace an oversized
        // result with an error in the same batch without losing the other replies.
        append(batch, message);
        requests_.erase(found); --batch->pending; return ready(batch);
    }
    std::optional<QJsonValue> cancel(const QString& id) {
        const auto found = requests_.find(id); if (found == requests_.end()) return {};
        auto batch = found->second; requests_.erase(found); --batch->pending; return ready(batch);
    }
    void clear() { requests_.clear(); bytes_ = 0; }
};
}
