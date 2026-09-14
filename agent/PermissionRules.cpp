#include "PermissionRulesInternal.h"
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QRegularExpression>
#include <tree_sitter/api.h>
#include <algorithm>
#include <chrono>
#include <memory>

extern "C" const TSLanguage* tree_sitter_bash();
namespace iiLocalLLM::agent {
namespace {
void require(bool ok, const QString& message) { if (!ok) throw Error(ErrorCode::InvalidArgument, message); }
struct Rule { QString tool, content; bool whole = true; };
Rule parse(const QString& value) {
    Rule r; bool escaped = false; int start = -1, end = -1;
    for (int i = 0; i < value.size(); ++i) {
        const auto c = value[i];
        if (escaped) { escaped = false; continue; }
        if (c == '\\') { escaped = true; continue; }
        if (c == '(') { require(start < 0, "Nested permission rule parentheses must be escaped"); start = i; }
        if (c == ')') { require(start >= 0 && end < 0, "Unmatched permission rule parenthesis"); end = i; }
    }
    require(!escaped && (start < 0 || end == value.size() - 1), "Unclosed permission rule or trailing content");
    r.tool = start < 0 ? value : value.left(start);
    static const QRegularExpression valid("\\A[A-Za-z0-9_.:*?-]{1,256}\\z");
    require(valid.match(r.tool).hasMatch(), "Invalid permission tool pattern");
    if (start >= 0) {
        r.content = value.mid(start + 1, end - start - 1);
        r.content.replace("\\(", "(").replace("\\)", ")").replace("\\\\", "\\");
        r.whole = r.content.isEmpty() || r.content == "*";
    }
    return r;
}
bool glob(const QString& pattern, const QString& value) {
    return QRegularExpression(QRegularExpression::wildcardToRegularExpression(pattern,
        QRegularExpression::NonPathWildcardConversion)).match(value).hasMatch();
}
bool toolMatch(const QString& pattern, const QString& name) {
    return glob(pattern, name) || (pattern.startsWith("mcp__") && !pattern.mid(5).contains("__") && name.startsWith(pattern + "__"));
}
bool inside(const QString& path, const QString& root) { return !root.isEmpty() && (path == root || path.startsWith(root + '/')); }
QString canonicalTarget(QString path) {
    QStringList suffix;
    while (!QFileInfo::exists(path)) {
        const QFileInfo info(path);
        if (info.isSymLink() || QDir(path).isRoot()) return {};
        suffix.prepend(info.fileName()); const auto parent = info.absolutePath(); if (parent == path) return {}; path = parent;
    }
    const auto resolved = QFileInfo(path).canonicalFilePath();
    return resolved.isEmpty() ? QString() : QDir::cleanPath(resolved + '/' + suffix.join('/'));
}
bool pathGlob(const QString& pattern, const QString& path) {
    QString expression = "\\A";
    for (qsizetype i = 0; i < pattern.size(); ++i) {
        if (pattern[i] == '*') {
            if (i + 1 < pattern.size() && pattern[i + 1] == '*') {
                ++i;
                if (i + 1 < pattern.size() && pattern[i + 1] == '/') { ++i; expression += "(?:.*/)?"; }
                else expression += ".*";
            } else expression += "[^/]*";
        } else if (pattern[i] == '?') expression += "[^/]";
        else expression += QRegularExpression::escape(pattern.mid(i, 1));
    }
    return QRegularExpression(expression + "\\z").match(path).hasMatch();
}
bool fileMatch(const QString& content, const QString& path, const ToolContext& context, bool allow) {
    if (path.isEmpty() || context.workingDirectory.isEmpty()) return false;
    const auto root = QFileInfo(context.workingDirectory).canonicalFilePath();
    const auto lexical = QDir::cleanPath(QDir::isAbsolutePath(path) ? path : QDir(root).filePath(path));
    const auto canonical = canonicalTarget(lexical);
    auto pattern = QDir::isAbsolutePath(content) ? content : QDir(root).filePath(content);
    pattern = QDir::cleanPath(pattern);
    if (allow) return inside(lexical, root) && inside(canonical, root) && pathGlob(pattern, lexical) && pathGlob(pattern, canonical);
    return pathGlob(pattern, lexical) || (!canonical.isEmpty() && pathGlob(pattern, canonical));
}
bool valueMatch(const Rule& rule, const QString& name, const QJsonObject& args, const ToolContext& context, bool allow) {
    if (!toolMatch(rule.tool, name)) return false;
    if (rule.whole) return true;
    if (name == "Read" || name == "Write" || name == "Edit") return fileMatch(rule.content, args["path"].toString(), context, allow);
    if (name == "Skill" || name == "Agent") {
        auto value = args[name == "Skill" ? "skill" : "subagent_type"].toString();
        if (name == "Skill" && value.startsWith('/')) value.remove(0, 1);
        return rule.content.endsWith(":*") ? value.startsWith(rule.content.chopped(2)) : value == rule.content;
    }
    return false; // Unknown tools never treat argument rules as whole-tool grants.
}
QString unquote(const QString& text, bool& literal) {
    QString result; QChar quote; bool escape = false;
    for (const auto c : text) {
        if (escape) {
            if (c != '\n') {
                if (quote == '"' && c != '$' && c != '`' && c != '"' && c != '\\') result += '\\';
                result += c;
            }
            escape = false;
        } else if (c == '\\' && quote != '\'') escape = true;
        else if (!quote.isNull()) {
            if (c == quote) quote = {};
            else { if (quote == '"' && (c == '$' || c == '`')) literal = false; result += c; }
        } else if (c == '\'' || c == '"') quote = c;
        else {
            if (QString("$`*?[]{}~").contains(c)) literal = false;
            result += c;
        }
    }
    literal &= !escape && quote.isNull(); return result;
}
struct Command { QString raw, normalized; QStringList words; bool literal = true, assigned = false; };
struct Redirect { QString path; bool write; };
struct Shell {
    QList<Command> commands; QList<Redirect> redirects;
    bool safe = true, opaque = false, unknownRedirect = false, changesDirectory = false;
};
struct ParseInput {
    QByteArray bytes; const CancellationToken& token;
    std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
};
Shell inspectShell(const QString& text, const ToolContext& context) {
    Shell result; context.cancellation.throwIfCancelled();
    ParseInput input{text.toUtf8(), context.cancellation};
    if (input.bytes.size() > 65536 || input.bytes.contains('\0')) { result.safe = false; result.opaque = true; return result; }
    std::unique_ptr<TSParser, decltype(&ts_parser_delete)> parser(ts_parser_new(), ts_parser_delete);
    require(parser && ts_parser_set_language(parser.get(), tree_sitter_bash()), "Cannot initialize Bash permission parser");
    TSInput source{&input, [](void* data, uint32_t offset, TSPoint, uint32_t* count) {
        auto& value = *static_cast<ParseInput*>(data); *count = uint32_t(value.bytes.size()) - std::min(offset, uint32_t(value.bytes.size()));
        return value.bytes.constData() + std::min(offset, uint32_t(value.bytes.size()));
    }, TSInputEncodingUTF8, nullptr};
    TSParseOptions options{&input, [](TSParseState* state) {
        auto& value = *static_cast<ParseInput*>(state->payload);
        return value.token.isCancelled() || std::chrono::steady_clock::now() >= value.deadline;
    }};
    std::unique_ptr<TSTree, decltype(&ts_tree_delete)> tree(ts_parser_parse_with_options(parser.get(), nullptr, source, options), ts_tree_delete);
    context.cancellation.throwIfCancelled();
    if (!tree) { result.safe = false; result.opaque = true; return result; }
    const auto root = ts_tree_root_node(tree.get());
    if (ts_node_has_error(root)) { result.safe = false; result.opaque = true; }
    auto raw = [&](TSNode node) { return QString::fromUtf8(input.bytes.mid(ts_node_start_byte(node), ts_node_end_byte(node) - ts_node_start_byte(node))); };
    QList<TSNode> stack{root}; int visited = 0;
    while (!stack.isEmpty()) {
        context.cancellation.throwIfCancelled();
        if (++visited > 8192 || std::chrono::steady_clock::now() >= input.deadline) { result.safe = false; result.opaque = true; break; }
        const auto node = stack.takeLast(); const QByteArray type = ts_node_type(node);
        if (type == "command") {
            Command command; command.raw = raw(node);
            for (uint32_t i = 0; i < ts_node_named_child_count(node); ++i) {
                const auto child = ts_node_named_child(node, i); const QByteArray kind = ts_node_type(child);
                if (kind == "variable_assignment") { command.assigned = true; continue; }
                if (kind == "file_redirect") continue;
                if (kind == "command_name" || kind == "word" || kind == "raw_string" || kind == "string" || kind == "number" || kind == "concatenation") {
                    bool literal = true; const auto value = unquote(raw(child), literal); command.words.append(value); command.literal &= literal;
                    if (kind == "command_name" && !literal) result.opaque = true;
                } else { command.literal = false; command.words.append(raw(child)); }
            }
            command.normalized = command.words.join(' ');
            if (command.words.isEmpty()) result.opaque = true;
            const auto name = command.words.value(0);
            // Joining normalized words must not turn one executable named
            // "git status" into the executable/argument pair git + status.
            if (std::any_of(name.cbegin(), name.cend(), [](QChar c) { return c.isSpace(); })) result.safe = false;
            if (QStringList{"cd", "pushd", "popd"}.contains(name)) result.changesDirectory = true;
            if (QStringList{"eval", "source", ".", "bash", "sh", "zsh", "dash", "ksh", "exec", "env", "command", "builtin", "sudo", "xargs", "timeout", "nice", "nohup"}.contains(name)) {
                result.safe = false;
                // Treat wrappers/interpreters as opaque for argument deny/ask rules.
                // Host whole-tool or exact-command permission remains explicit authority.
                result.opaque = true;
            }
            result.safe &= command.literal && !command.assigned; result.commands.append(std::move(command));
        } else if (type == "file_redirect") {
            TSNode target{}; QString op;
            if (ts_node_has_error(node)) { result.safe = false; result.unknownRedirect = true; }
            for (uint32_t i = 0; i < ts_node_child_count(node); ++i) {
                const auto child = ts_node_child(node, i);
                if (!ts_node_is_named(child)) op = raw(child);
                else if (QByteArray(ts_node_type(child)) != "file_descriptor") {
                    // This grammar can group trailing command arguments with the
                    // redirect destination. Do not silently choose the last one.
                    if (!ts_node_is_null(target)) { result.safe = false; result.unknownRedirect = true; }
                    target = child;
                }
            }
            bool literal = true; const auto path = ts_node_is_null(target) ? QString() : unquote(raw(target), literal);
            if ((op == ">&" || op == "<&") && QRegularExpression("\\A[012-]\\z").match(path).hasMatch()) continue;
            if (!literal || path.isEmpty() || !QStringList{">", ">>", ">|", "&>", "&>>", "<"}.contains(op)) {
                result.safe = false; result.unknownRedirect = true;
            } else result.redirects.append({path, op != "<"});
        } else if (type == "command_substitution" || type == "process_substitution" || type == "simple_expansion" || type == "expansion"
                   || type == "arithmetic_expansion" || type == "heredoc_redirect" || type == "herestring_redirect") result.safe = false;
        else if (ts_node_is_named(node) && !QList<QByteArray>{"program", "list", "pipeline", "subshell", "redirected_statement", "command_name", "word", "raw_string", "string", "string_content", "number", "concatenation", "comment", "file_descriptor"}.contains(type)) {
            result.safe = false;
            if (type != "variable_assignment" && type != "variable_name") result.opaque = true;
        } else if (!ts_node_is_named(node) && type == "&") result.safe = false;
        for (uint32_t i = ts_node_child_count(node); i > 0; --i) stack.append(ts_node_child(node, i - 1));
    }
    if (result.commands.isEmpty()) result.safe = false;
    if (result.changesDirectory) result.safe = false;
    for (const auto& redirect : result.redirects) {
        const auto rootPath = QFileInfo(context.workingDirectory).canonicalFilePath();
        const auto path = QDir::cleanPath(QDir::isAbsolutePath(redirect.path) ? redirect.path : QDir(rootPath).filePath(redirect.path));
        if (context.workingDirectory.isEmpty() || !inside(path, rootPath) || !inside(canonicalTarget(path), rootPath)) result.safe = false;
    }
    return result;
}
bool commandMatch(const QString& pattern, const QString& value) {
    if (pattern.endsWith(":*")) { const auto prefix = pattern.chopped(2); return value == prefix || value.startsWith(prefix + ' '); }
    auto expression = QRegularExpression::escape(pattern);
    expression.replace("\\*", ".*").replace("\\?", ".");
    return QRegularExpression("\\A" + expression + "\\z", QRegularExpression::DotMatchesEverythingOption).match(value).hasMatch();
}
}
QStringList parsePermissionRules(const QStringList& input) {
    QStringList result; qsizetype bytes = 0;
    for (const auto& value : input) {
        bytes += value.toUtf8().size(); require(bytes <= 65536 && !value.contains(QChar::Null), "Permission rule list exceeds limits");
        QString part; bool escaped = false; int depth = 0;
        auto append = [&] { if (!part.isEmpty()) { require(part.size() <= 4096 && result.size() < 256, "Permission rule limit exceeded"); parse(part); result.append(part); part.clear(); } };
        for (const auto c : value) {
            if (escaped) { part += c; escaped = false; continue; }
            if (c == '\\') { part += c; escaped = true; continue; }
            if (c == '(') { ++depth; require(depth == 1, "Nested permission parentheses must be escaped"); }
            if (c == ')') { --depth; require(depth == 0, "Unmatched permission rule parenthesis"); }
            if (!depth && (c.isSpace() || c == ',')) append(); else part += c;
        }
        require(!escaped && !depth, "Unclosed permission rule"); append();
    }
    result.removeDuplicates(); return result;
}
namespace detail {
bool permissionRulesMatch(const QStringList& input, const ToolDefinition& tool, const QJsonObject& args, const ToolContext& context, bool allow) {
    const auto strings = parsePermissionRules(input); QList<Rule> rules; for (const auto& s : strings) rules.append(parse(s));
    if (tool.name != "Bash") return std::any_of(rules.begin(), rules.end(), [&](const auto& r) { return valueMatch(r, tool.name, args, context, allow); });
    QList<Rule> bash, files;
    for (const auto& r : rules) {
        if (toolMatch(r.tool, tool.name)) {
            if (r.whole) return true;
            bash.append(r);
        }
        if (!allow && (toolMatch(r.tool, "Read") || toolMatch(r.tool, "Write"))) files.append(r);
    }
    if (bash.isEmpty() && files.isEmpty()) return false;
#ifndef Q_OS_UNIX
    // The current non-POSIX executor runs cmd.exe, whose syntax is not Bash.
    // Content rules cannot authorize it or prove absence of deny/ask operations.
    return !allow;
#else
    const auto command = args["command"].toString().trimmed(); const auto shell = inspectShell(command, context);
    if (!allow && (!bash.isEmpty() && shell.opaque)) return true; // Cannot exclude a denied/ask operation.
    if (!allow && !files.isEmpty() && (shell.unknownRedirect || (shell.changesDirectory && !shell.redirects.isEmpty()))) return true;
    for (const auto& r : files) for (const auto& redirect : shell.redirects)
        if (valueMatch(r, redirect.write ? "Write" : "Read", {{"path", redirect.path}}, context, false)) return true;
    for (const auto& r : bash) if (!r.content.contains('*') && !r.content.contains('?') && command == r.content) return true;
    auto covered = [&](const Command& cmd) {
        return std::any_of(bash.begin(), bash.end(), [&](const auto& r) {
            // The AST isolates leaves. Never run prefix/wildcard matching on a compound string.
            return commandMatch(r.content, cmd.raw) || commandMatch(r.content, cmd.normalized);
        });
    };
    if (allow) return shell.safe && std::all_of(shell.commands.begin(), shell.commands.end(), covered);
    return std::any_of(shell.commands.begin(), shell.commands.end(), covered);
#endif
}
}
}
