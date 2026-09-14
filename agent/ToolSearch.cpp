#include "ToolSearch.h"
#include <QtCore/QCryptographicHash>
#include <QtCore/QJsonDocument>
#include <QtCore/QRegularExpression>
#include <QtCore/QMap>
#include <QtCore/QSet>
#include <algorithm>

namespace iiLocalLLM::agent {
namespace {
QString fingerprint(const ToolDefinition& tool) {
    auto value = toJson(tool);
    value["deferred"] = tool.deferred; value["editsFiles"] = tool.editsFiles;
    return QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(value).toJson(QJsonDocument::Compact),
        QCryptographicHash::Sha256).toHex());
}
QString words(QString value) {
    value.replace(QRegularExpression("([a-z])([A-Z])"), "\\1 \\2");
    return value.toCaseFolded();
}
}
ToolResult searchTools(const QList<ToolDefinition>& definitions, const QString& input, int limit) {
    const auto query = input.trimmed();
    if (query.isEmpty() || query.size() > 4096 || limit < 1 || limit > 100)
        throw Error(ErrorCode::InvalidArgument, "ToolSearch requires a nonempty query of at most 4096 characters and 1..100 results");
    QList<ToolDefinition> selected; QStringList missing; QSet<QString> seen;
    const auto lower = query.toCaseFolded();
    auto select = [&](const QString& name) {
        const auto it = std::find_if(definitions.begin(), definitions.end(), [&](const auto& d) {
            return d.name.compare(name, Qt::CaseInsensitive) == 0;
        });
        if (it == definitions.end()) return false;
        if (!seen.contains(it->name) && selected.size() < limit) { selected.append(*it); seen.insert(it->name); }
        return true;
    };
    if (lower.startsWith("select:")) {
        const auto names = query.mid(7).split(',', Qt::SkipEmptyParts);
        if (names.isEmpty()) throw Error(ErrorCode::InvalidArgument, "ToolSearch select requires a tool name");
        for (const auto& name : names) if (!select(name.trimmed())) missing.append(name.trimmed());
    } else if (!select(query)) {
        if (lower.startsWith("mcp__") && lower.size() > 5) {
            for (const auto& d : definitions)
                if (d.deferred && d.name.toCaseFolded().startsWith(lower)) select(d.name);
        }
        if (selected.isEmpty()) {
            const auto terms = lower.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
            QList<std::pair<int, ToolDefinition>> scored;
            for (const auto& d : definitions) {
                if (!d.deferred) continue;
                const auto name = words(d.name);
                const auto parts = name.split(QRegularExpression("[^\\p{L}\\p{N}]+"), Qt::SkipEmptyParts);
                const auto description = words(d.description);
                const auto hints = words(d.metadata["app_id"].toString() + ' ' + d.metadata["server_name"].toString()
                    + ' ' + d.metadata["search_hint"].toString());
                int score = 0; bool eligible = true;
                for (auto term : terms) {
                    const bool required = term.startsWith('+') && term.size() > 1;
                    if (required) term.remove(0, 1);
                    const bool inName = name.contains(term), inDescription = description.contains(term), inHints = hints.contains(term);
                    if (required && !inName && !inDescription && !inHints) { eligible = false; break; }
                    if (parts.contains(term)) score += 16;
                    else if (inName) score += 8;
                    if (inHints) score += 4;
                    if (inDescription) score += 2;
                }
                if (eligible && score) scored.append({score, d});
            }
            std::sort(scored.begin(), scored.end(), [](const auto& a, const auto& b) {
                return a.first != b.first ? a.first > b.first : a.second.name < b.second.name;
            });
            for (const auto& item : scored) { if (selected.size() >= limit) break; select(item.second.name); }
        }
    }
    QJsonArray matches, tools, entries;
    for (const auto& d : selected) {
        matches.append(d.name); tools.append(toJson(d));
        if (d.deferred) entries.append(QJsonObject{{"name", d.name}, {"fingerprint", fingerprint(d)}});
    }
    const auto total = std::count_if(definitions.begin(), definitions.end(), [](const auto& d) { return d.deferred; });
    ToolResult result;
    result.data = {{"query", query}, {"matches", matches}, {"tools", tools}, {"missing", QJsonArray::fromStringList(missing)},
        {"total_deferred_tools", int(total)}};
    result.metadata = {{"iilocal.tool_search", QJsonObject{{"version", 1}, {"entries", entries}}}};
    const auto bytes = QJsonDocument(result.data).toJson(QJsonDocument::Compact);
    if (bytes.size() > 4 * 1024 * 1024) throw Error(ErrorCode::ResourceLimit, "ToolSearch result is too large; select fewer tools");
    // Full schemas travel in data and in the next ModelRequest::tools. Avoid
    // presenting duplicated metadata as an observation of the user's task.
    QStringList names; for (const auto& d : selected) names.append(d.name);
    result.text = names.isEmpty() ? "No matching tool was found. Try different keywords or an exact tool name."
        : "Tool definitions found: " + names.join(", ")
            + ". No operation has been executed. Call the selected tool on the next turn to perform the requested action.";
    if (!missing.isEmpty()) result.text += " Names not found: " + missing.join(", ") + '.';
    return result;
}
namespace detail {
void prepareToolDiscovery(ToolRegistry& registry, const Session& session, const ToolSearchOptions& options) {
    const auto catalog = registry.definitions();
    if (!options.enabled || std::none_of(catalog.begin(), catalog.end(), [](const auto& t) { return t.deferred; })) return;
    if (std::any_of(catalog.begin(), catalog.end(), [](const auto& t) { return t.name == "ToolSearch"; }))
        throw Error(ErrorCode::AlreadyExists, "ToolSearch is reserved while deferred discovery is enabled");
    QMap<QString, QString> fingerprints;
    for (const auto& t : catalog) if (t.deferred) fingerprints[t.name] = fingerprint(t);
    QMap<QString, QString> pending;
    QStringList active;
    auto touch = [&](const QString& name) {
        active.removeAll(name); active.append(name);
        while (active.size() > options.maxActiveTools) active.removeFirst();
    };
    for (const auto& message : session.messages) {
        if (message.role == MessageRole::Assistant) {
            pending.clear();
            for (const auto& call : message.toolCalls) pending[call.id] = call.name;
        } else if (message.role == MessageRole::Tool) {
            const auto name = pending.take(message.toolCallId);
            if (message.isError) continue;
            if (name == "ToolSearch") {
                const auto search = message.metadata["iilocal.tool_search"].toObject();
                if (search["version"] != 1) continue;
                for (const auto& entry : search["entries"].toArray()) {
                    const auto value = entry.toObject(); const auto selected = value["name"].toString();
                    if (fingerprints.contains(selected) && fingerprints[selected] == value["fingerprint"].toString()) touch(selected);
                }
            } else if (active.contains(name)) touch(name);
        }
    }
    for (const auto& t : catalog) if (t.deferred && !active.contains(t.name)) registry.remove(t.name);
    Tool search;
    search.definition.name = "ToolSearch";
    search.definition.description = "Find additional tools by keywords, +required terms, an MCP server prefix, or select:exact_name1,exact_name2. "
        "Returns full definitions and makes matching tools callable on the next turn. Search before calling a tool that is not currently available. "
        "Use app/server names or describe the action. Selecting a tool does not grant execution permission.";
    search.definition.readOnly = true; search.definition.concurrencySafe = true;
    search.definition.metadata = {{"source", "builtin"}};
    search.definition.inputSchema = {{"type", "object"}, {"properties", QJsonObject{
        {"query", QJsonObject{{"type", "string"}, {"minLength", 1}, {"maxLength", 4096}}},
        {"max_results", QJsonObject{{"type", "integer"}, {"minimum", 1}, {"maximum", options.maxResults}}}}},
        {"required", QJsonArray{"query"}}, {"additionalProperties", false}};
    search.execute = [catalog, options](const QJsonObject& args, const ToolContext& context) {
        context.cancellation.throwIfCancelled();
        return searchTools(catalog, args["query"].toString(), args["max_results"].toInt(std::min(5, options.maxResults)));
    };
    registry.add(std::move(search));
}
}
}
