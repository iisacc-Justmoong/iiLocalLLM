#include "Skills.h"
#include "SkillsInternal.h"
#include "ContextFile.h"
#include <QtCore/QCryptographicHash>
#include <QtCore/QDirIterator>
#include <QtCore/QJsonDocument>
#include <QtCore/QRegularExpression>
#include <QtCore/QSet>
#include <QtCore/QStringDecoder>
#include <QtCore/QUuid>
#include <yaml.h>
#include <algorithm>

namespace iiLocalLLM::agent {
namespace {
void require(bool ok, const QString& text, ErrorCode code = ErrorCode::InvalidArgument) { if (!ok) throw Error(code, text); }
bool inside(const QString& path, const QString& root) { return path == root || path.startsWith(root + '/'); }
QString decode(const QByteArray& bytes) {
    QStringDecoder decoder(QStringDecoder::Utf8); const QString text = decoder(bytes);
    require(!decoder.hasError() && !text.contains(QChar::Null), "Skill must contain valid UTF-8 without NUL"); return text;
}
QString normalizeName(QString name) {
    name = name.trimmed(); if (name.startsWith('/')) name.remove(0, 1);
    static const QRegularExpression valid("^[A-Za-z0-9_][A-Za-z0-9_.-]{0,127}$");
    require(valid.match(name).hasMatch() && name != "." && name != "..", "Invalid skill name"); return name;
}
struct Loaded { SkillInfo info; QString body; };
struct YamlDocument {
    yaml_document_t doc{}; bool loaded = false;
    ~YamlDocument() { if (loaded) yaml_document_delete(&doc); }
};
void parseYaml(const QByteArray& bytes, Loaded& value, const CancellationToken& token) {
    yaml_parser_t parser{}; require(yaml_parser_initialize(&parser), "Cannot initialize skill YAML parser", ErrorCode::RuntimeFailure);
    struct Cleanup { yaml_parser_t* parser; ~Cleanup() { yaml_parser_delete(parser); } } cleanup{&parser};
    auto input = [&] { yaml_parser_set_input_string(&parser, reinterpret_cast<const unsigned char*>(bytes.constData()), size_t(bytes.size())); };
    input(); int depth = 0, events = 0, documents = 0;
    while (true) {
        token.throwIfCancelled(); yaml_event_t event{};
        require(yaml_parser_parse(&parser, &event), "Invalid skill YAML");
        const auto type = event.type;
        const bool alias = type == YAML_ALIAS_EVENT;
        if (type == YAML_MAPPING_START_EVENT || type == YAML_SEQUENCE_START_EVENT) ++depth;
        if (type == YAML_MAPPING_END_EVENT || type == YAML_SEQUENCE_END_EVENT) --depth;
        if (type == YAML_DOCUMENT_START_EVENT) ++documents;
        yaml_event_delete(&event);
        require(!alias && depth <= 16 && ++events <= 4096 && documents <= 1, "Skill YAML aliases, depth or document limit exceeded");
        if (type == YAML_STREAM_END_EVENT) break;
    }
    yaml_parser_delete(&parser); require(yaml_parser_initialize(&parser), "Cannot initialize skill YAML parser", ErrorCode::RuntimeFailure); input();
    YamlDocument document; require(yaml_parser_load(&parser, &document.doc), "Invalid skill frontmatter"); document.loaded = true;
    auto* root = yaml_document_get_root_node(&document.doc);
    if (!root) return;
    require(root->type == YAML_MAPPING_NODE, "Skill frontmatter must be a mapping");
    auto node = [&](int id) { return yaml_document_get_node(&document.doc, id); };
    auto scalar = [&](yaml_node_t* n) {
        require(n && n->type == YAML_SCALAR_NODE, "Skill metadata field must be a scalar");
        return decode(QByteArray(reinterpret_cast<const char*>(n->data.scalar.value), qsizetype(n->data.scalar.length)));
    };
    auto list = [&](yaml_node_t* n) {
        QStringList result;
        if (n->type == YAML_SCALAR_NODE) result = scalar(n).split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
        else {
            require(n->type == YAML_SEQUENCE_NODE, "Skill arguments must be a string or string list");
            for (auto* p = n->data.sequence.items.start; p != n->data.sequence.items.top; ++p) result.append(scalar(node(*p)));
        }
        require(result.size() <= 64, "Too many skill argument names");
        static const QRegularExpression valid("^[A-Za-z_][A-Za-z0-9_]{0,63}$"); QSet<QString> seen;
        for (const auto& name : result) {
            require(valid.match(name).hasMatch() && name != "ARGUMENTS" && !seen.contains(name), "Invalid or duplicate skill argument name"); seen.insert(name);
        }
        return result;
    };
    QSet<QString> keys;
    for (auto* p = root->data.mapping.pairs.start; p != root->data.mapping.pairs.top; ++p) {
        const auto key = scalar(node(p->key)); auto* n = node(p->value);
        require(!key.isEmpty() && key.size() <= 128 && !keys.contains(key), "Empty, duplicate or excessive skill metadata key"); keys.insert(key);
        auto text = [&] { const auto s = scalar(n); require(s.size() <= 4096, "Skill metadata field exceeds limit", ErrorCode::ResourceLimit); return s; };
        if (key == "name") value.info.displayName = text();
        else if (key == "description") value.info.description = text();
        else if (key == "argument-hint") value.info.argumentHint = text();
        else if (key == "when_to_use") value.info.whenToUse = text();
        else if (key == "version") value.info.version = text();
        else if (key == "arguments") value.info.argumentNames = list(n);
        else if (key == "disable-model-invocation" || key == "user-invocable") {
            const auto s = text().toLower(); require(s == "true" || s == "false", "Skill invocation flag must be true or false");
            if (key == "user-invocable") value.info.userInvocable = s == "true"; else value.info.disableModelInvocation = s == "true";
        } else if (key == "license" || key == "compatibility" || key == "metadata") {
            // Descriptive Agent Skills metadata has no runtime effect.
        } else if (key == "model") { value.info.model = text();
        } else if (key == "agent") { value.info.agent = text();
        } else if (key == "context") { value.info.executionContext = text();
        } else if (key == "allowed-tools" && n->type == YAML_SEQUENCE_NODE && n->data.sequence.items.start == n->data.sequence.items.top) {
        } else value.info.unsupportedFeatures.append(key);
    }
    if (value.info.executionContext != "inline" && value.info.executionContext != "fork") value.info.unsupportedFeatures.append("context");
    if (value.info.executionContext != "fork") {
        if (!value.info.agent.isEmpty()) value.info.unsupportedFeatures.append("agent");
        if (!value.info.model.isEmpty() && value.info.model != "inherit") value.info.unsupportedFeatures.append("model");
    }
    require(value.info.agent.size() <= 128 && value.info.model.size() <= 256, "Skill agent or model exceeds limit");
}
Loaded parse(const QString& name, const QString& path, const QByteArray& raw, const CancellationToken& token) {
    Loaded value; value.info.name = name; value.info.path = path; value.info.directory = QFileInfo(path).absolutePath();
    value.info.sha256 = QString::fromLatin1(QCryptographicHash::hash(raw, QCryptographicHash::Sha256).toHex());
    const auto bytes = raw.startsWith("\xEF\xBB\xBF") ? raw.mid(3) : raw;
    value.body = decode(bytes); value.body.replace("\r\n", "\n");
    const auto opening = QRegularExpression("^---[ \\t]*\\n").match(value.body);
    if (opening.hasMatch()) {
        const auto start = opening.capturedEnd();
        const auto end = value.body.indexOf(QRegularExpression("(?m)^---[ \\t]*(?:\\n|$)"), start);
        require(end >= 0, "Unclosed skill frontmatter");
        const auto yaml = value.body.mid(start, end - start).toUtf8();
        require(yaml.size() <= 16 * 1024, "Skill frontmatter exceeds limit", ErrorCode::ResourceLimit);
        const auto newline = value.body.indexOf('\n', end);
        value.body = newline < 0 ? QString() : value.body.mid(newline + 1);
        parseYaml(yaml, value, token);
    }
    require(!value.body.trimmed().isEmpty(), "Skill body is empty");
    if (value.info.description.trimmed().isEmpty()) {
        for (const auto& line : value.body.split('\n')) if (!line.trimmed().isEmpty()) {
            value.info.description = line.trimmed().remove(QRegularExpression("^#{1,6}\\s+")).left(512); break;
        }
    }
    if (value.body.contains("!`") || value.body.contains(QRegularExpression("(?m)^\\s*(?:```|~~~)!")))
        value.info.unsupportedFeatures.append("shell-substitution");
    return value;
}
QList<Loaded> scan(const QString& workspace, const SkillOptions& options, const CancellationToken& token, QJsonArray& shadowed) {
    token.throwIfCancelled(); if (!options.enabled) return {};
    require(options.maxFileBytes > 0 && options.maxFileBytes <= 1024 * 1024 && options.maxTotalBytes > 0 && options.maxTotalBytes <= 16 * 1024 * 1024
        && options.maxSkills > 0 && options.maxSkills <= 1024 && options.maxScannedEntries > 0 && options.maxScannedEntries <= 65536
        && options.directories.size() <= 64, "Invalid skill discovery limits");
    const auto cwd = QFileInfo(workspace).canonicalFilePath();
    require(!cwd.isEmpty() && QFileInfo(cwd).isDir() && !QDir(cwd).isRoot(), "Invalid skill workspace");
    auto directories = options.directories; directories.append(QDir(cwd).filePath(".claude/skills"));
    QList<Loaded> result; QSet<QString> seenRoots, seenFiles, seenNames; qint64 total = 0; int scanned = 0, files = 0;
    for (qsizetype index = 0; index < directories.size(); ++index) {
        token.throwIfCancelled(); const auto input = directories[index];
        require(!input.isEmpty() && input.size() <= 4096 && !input.contains(QChar::Null) && !input.startsWith('~') && !input.contains("://"), "Invalid skills directory");
        QFileInfo rootInfo(QDir::isAbsolutePath(input) ? input : QDir(cwd).filePath(input));
        if (!rootInfo.exists() && !rootInfo.isSymLink()) continue;
        const auto root = rootInfo.canonicalFilePath();
        require(!root.isEmpty() && rootInfo.isDir() && !QDir(root).isRoot(), "Skills path must be a directory");
        if (index + 1 == directories.size()) require(inside(root, cwd), "Workspace skills directory escapes the workspace");
        if (seenRoots.contains(root)) continue; seenRoots.insert(root);
        QDirIterator iterator(root, QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot); QStringList entries;
        while (iterator.hasNext()) {
            token.throwIfCancelled(); require(++scanned <= options.maxScannedEntries, "Skill scan exceeds entry limit", ErrorCode::ResourceLimit);
            entries.append(iterator.next());
        }
        entries.sort(Qt::CaseSensitive);
        for (const auto& entry : entries) {
            token.throwIfCancelled(); const QFileInfo directory(entry);
            if (!directory.isDir() && !directory.isSymLink()) continue;
            const auto candidate = QDir(entry).filePath("SKILL.md"); const QFileInfo file(candidate);
            if (!file.exists() && !file.isSymLink()) continue;
            const auto path = file.canonicalFilePath();
            require(!path.isEmpty() && inside(path, root), "Skill symlink escapes its directory");
            const auto name = normalizeName(directory.fileName());
            if (seenFiles.contains(path) || seenNames.contains(name)) {
                shadowed.append(QJsonObject{{"name", name}, {"path", path}, {"reason", seenFiles.contains(path) ? "duplicate_file" : "duplicate_name"}}); continue;
            }
            require(++files <= options.maxSkills, "Too many skills", ErrorCode::ResourceLimit);
            const auto raw = detail::readContextFile(root, path, options.maxFileBytes, token); total += raw.size();
            require(total <= options.maxTotalBytes, "Skill input exceeds total byte limit", ErrorCode::ResourceLimit);
            result.append(parse(name, path, raw, token)); seenFiles.insert(path); seenNames.insert(name);
        }
    }
    return result;
}
QStringList arguments(const QString& text) {
    QStringList result; QString part; QChar quote; bool escape = false, started = false;
    for (const auto c : text) {
        if (escape) { part += c; escape = false; started = true; }
        else if (c == '\\' && quote != '\'') { escape = true; started = true; }
        else if (!quote.isNull()) { if (c == quote) quote = {}; else part += c; }
        else if (c == '\'' || c == '"') { quote = c; started = true; }
        else if (c.isSpace()) { if (started) { result.append(part); part.clear(); started = false; } }
        else { part += c; started = true; }
    }
    require(quote.isNull() && !escape, "Unclosed quote or escape in skill arguments");
    if (started) result.append(part); return result;
}
QString expand(const Loaded& skill, const QString& raw, const QString& session) {
    const auto args = arguments(raw);
    static const QRegularExpression placeholder(R"(\$\{CLAUDE_SKILL_DIR\}|\$\{CLAUDE_SESSION_ID\}|\$ARGUMENTS\[(\d+)\]|\$(\d+)(?!\w)|\$([A-Za-z_][A-Za-z0-9_]*)(?![\w\[]))");
    auto matches = placeholder.globalMatch(skill.body); QString output; qsizetype pos = 0; bool substitutedArgument = false;
    while (matches.hasNext()) {
        const auto m = matches.next(); output += skill.body.mid(pos, m.capturedStart() - pos); QString replacement = m.captured();
        if (replacement == "${CLAUDE_SKILL_DIR}") replacement = QDir::fromNativeSeparators(skill.info.directory);
        else if (replacement == "${CLAUDE_SESSION_ID}") replacement = session;
        else {
            int index = -1;
            if (!m.captured(1).isEmpty() || !m.captured(2).isEmpty()) {
                bool ok = false; index = (m.captured(1).isEmpty() ? m.captured(2) : m.captured(1)).toInt(&ok);
                replacement = ok && index >= 0 && index < args.size() ? args[index] : QString(); substitutedArgument = true;
            } else if (m.captured(3) == "ARGUMENTS") { replacement = raw; substitutedArgument = true; }
            else if ((index = skill.info.argumentNames.indexOf(m.captured(3))) >= 0) {
                replacement = index < args.size() ? args[index] : QString(); substitutedArgument = true;
            }
        }
        output += replacement; pos = m.capturedEnd();
        require(output.size() <= 512 * 1024, "Expanded skill exceeds limit", ErrorCode::ResourceLimit);
    }
    output += skill.body.mid(pos);
    if (!substitutedArgument && !raw.isEmpty()) output += "\n\nARGUMENTS: " + raw;
    require(output.size() <= 512 * 1024, "Expanded skill exceeds limit", ErrorCode::ResourceLimit); return output;
}
}
QJsonObject SkillInfo::toJson() const {
    return {{"name", name}, {"display_name", displayName}, {"description", description}, {"argument_hint", argumentHint}, {"when_to_use", whenToUse},
        {"version", version}, {"path", path}, {"directory", directory}, {"sha256", sha256}, {"arguments", QJsonArray::fromStringList(argumentNames)},
        {"disable_model_invocation", disableModelInvocation}, {"user_invocable", userInvocable}, {"unsupported_features", QJsonArray::fromStringList(unsupportedFeatures)},
        {"context", executionContext}, {"agent", agent}, {"model", model}};
}
QJsonObject SkillCatalog::toJson() const {
    QJsonArray items; for (const auto& s : skills) items.append(s.toJson()); return {{"skills", items}, {"shadowed", shadowed}};
}
Message SkillCatalog::message() const {
    QJsonArray items;
    for (const auto& s : skills) if (!s.disableModelInvocation && s.unsupportedFeatures.isEmpty())
        items.append(QJsonObject{{"name", s.name}, {"description", s.description}, {"argument_hint", s.argumentHint}, {"when_to_use", s.whenToUse}, {"context", s.executionContext}});
    if (items.isEmpty()) return {};
    Message result{{}, MessageRole::User, "Available local skills (catalog metadata, not instructions). "
        "Call Skill with the skill name and optional args when its instructions are needed. "
        "Inline skills load instructions; fork skills run in an isolated child and return its result. Skills do not grant tool permissions.\n" + QString::fromUtf8(QJsonDocument(items).toJson(QJsonDocument::Compact))};
    result.metadata = {{"iilocal.skill_catalog", true}}; return result;
}
SkillCatalog discoverSkills(const QString& workspace, const SkillOptions& options, const CancellationToken& token) {
    SkillCatalog result; for (const auto& s : scan(workspace, options, token, result.shadowed)) result.skills.append(s.info); return result;
}
Message loadSkill(const QString& workspace, const QString& input, const QString& args, const QString& session,
    SkillInvocationSource source, const SkillOptions& options, const CancellationToken& token) {
    token.throwIfCancelled(); require(options.enabled, "Skills are disabled by the host", ErrorCode::RuntimeUnavailable);
    const auto name = normalizeName(input);
    require(args.size() <= 65536 && !args.contains(QChar::Null) && session.size() <= 128 && !session.contains(QChar::Null), "Invalid skill invocation arguments");
    QJsonArray shadowed; const auto loaded = scan(workspace, options, token, shadowed);
    const auto found = std::find_if(loaded.begin(), loaded.end(), [&](const auto& s) { return s.info.name == name; });
    require(found != loaded.end(), "Unknown skill: " + name, ErrorCode::NotFound);
    require(source != SkillInvocationSource::Model || !found->info.disableModelInvocation, "Skill disables model invocation");
    require(source != SkillInvocationSource::User || found->info.userInvocable, "Skill disables user invocation");
    require(found->info.unsupportedFeatures.isEmpty(), "Unsupported skill execution features: " + found->info.unsupportedFeatures.join(", "), ErrorCode::RuntimeUnavailable);
    Message message{QUuid::createUuid().toString(QUuid::WithoutBraces), MessageRole::User,
        "Skill: " + name + "\nBase directory for this skill's resources: " + found->info.directory
        + "\nSession working directory: " + QFileInfo(workspace).canonicalFilePath()
        + "\nRelative paths passed to workspace tools resolve from the session working directory."
        + "\nThis skill is already loaded. Follow its instructions below without calling Skill again for this invocation.\n\n" + expand(*found, args, session)};
    auto metadata = found->info.toJson(); metadata["format_version"] = 1; metadata["args"] = args;
    metadata["invocation"] = source == SkillInvocationSource::Model ? "model" : "user";
    message.metadata = {{"iilocal.skill", metadata}}; return message;
}
namespace detail {
SkillCatalog executableSkills(const QString& workspace, const SkillOptions& options, bool canFork, const CancellationToken& token) {
    auto catalog = discoverSkills(workspace, options, token);
    if (!canFork) for (auto& skill : catalog.skills) if (skill.executionContext == "fork") skill.unsupportedFeatures.append("fork-executor-unavailable");
    return catalog;
}
Tool skillTool(const QString& workspace, const SkillOptions& options, const SkillForkExecutor& executor, const SkillForkRequest& execution) {
    Tool tool; tool.definition.name = "Skill"; tool.definition.readOnly = true; tool.definition.concurrencySafe = false;
    tool.definition.description = "Invoke a local skill from the available catalog. Inline skills load instructions into this conversation; "
        "fork skills execute in an isolated child and return the final result. Pass a skill name and optional literal arguments. "
        "When a skill is already loaded in the current turn, follow its instructions directly instead of loading it again.";
    tool.definition.metadata = {{"source", "builtin.skill"}};
    tool.definition.inputSchema = {{"type", "object"}, {"additionalProperties", false}, {"required", QJsonArray{"skill"}}, {"properties", QJsonObject{
        {"skill", QJsonObject{{"type", "string"}, {"minLength", 1}, {"maxLength", 129}}},
        {"args", QJsonObject{{"type", "string"}, {"maxLength", 65536}}}}}};
    tool.execute = [workspace, options, executor, execution](const QJsonObject& args, const ToolContext& context) {
        auto message = loadSkill(workspace, args["skill"].toString(), args["args"].toString(), context.sessionId, SkillInvocationSource::Model, options, context.cancellation);
        const auto metadata = message.metadata["iilocal.skill"].toObject();
        if (metadata["context"] == "fork") {
            require(bool(executor), "Skill fork executor is unavailable", ErrorCode::RuntimeUnavailable);
            auto request = execution; request.prompt = std::move(message);
            const auto outcome = executor(request, context);
            const bool success = outcome.result.status == RunStatus::Completed;
            return ToolResult{success ? outcome.result.text : "Forked skill ended with status " + enumName(outcome.result.status) + ": " + outcome.result.errorMessage,
                {{"success", success}, {"status", "forked"}, {"commandName", metadata["name"]}, {"agentId", outcome.execution["agentId"]},
                    {"result", outcome.result.text}, {"execution", outcome.execution}}, !success, {}, {{"iilocal.skill_fork", outcome.execution}}};
        }
        return ToolResult{"Loaded skill " + message.metadata["iilocal.skill"].toObject()["name"].toString() + ". Follow the injected skill instructions on the next turn.",
            {{"success", true}, {"status", "inline"}, {"commandName", message.metadata["iilocal.skill"].toObject()["name"]}}, false, {},
            {{"iilocal.skill_result", QJsonObject{{"version", 1}, {"message", toJson(message)}}}}};
    };
    return tool;
}
QList<Message> pendingSkillMessages(const QList<Message>& messages) {
    QSet<QString> injected; QSet<QString> calls; QList<Message> pending;
    for (const auto& message : messages) if (message.role == MessageRole::User)
        injected.insert(message.metadata["iilocal.skill_parent"].toString());
    for (const auto& message : messages) {
        if (message.role == MessageRole::Assistant) {
            calls.clear(); for (const auto& call : message.toolCalls) if (call.name == "Skill") calls.insert(call.id);
        } else if (message.role == MessageRole::Tool && calls.remove(message.toolCallId) && !message.isError && !injected.contains(message.id)) {
            const auto result = message.metadata["iilocal.skill_result"].toObject(); if (result["version"] != 1) continue;
            auto prompt = messageFromJson(result["message"].toObject());
            require(prompt.role == MessageRole::User && prompt.toolCalls.isEmpty() && prompt.toolCallId.isEmpty()
                && prompt.metadata["iilocal.skill"].toObject()["format_version"] == 1, "Invalid persisted skill prompt", ErrorCode::ProtocolError);
            prompt.metadata["iilocal.skill_parent"] = message.id; pending.append(std::move(prompt));
        }
    }
    return pending;
}
}
}
