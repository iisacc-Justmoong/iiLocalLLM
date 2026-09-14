#include "ProjectContext.h"
#include "ContextFile.h"
#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QDirIterator>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QRegularExpression>
#include <QtCore/QSet>
#include <QtCore/QStringDecoder>
#include <md4c.h>
#include <yaml.h>
#include <algorithm>
#include <cstdint>

namespace iiLocalLLM::agent {
namespace {
void require(bool condition, const QString& message, ErrorCode code = ErrorCode::InvalidArgument) {
    if (!condition) throw Error(code, message);
}
QString hash(const QByteArray& value) { return QString::fromLatin1(QCryptographicHash::hash(value, QCryptographicHash::Sha256).toHex()); }
bool inside(const QString& path, const QString& root) { return path == root || path.startsWith(root + '/'); }
QString decode(const QByteArray& bytes) {
    QStringDecoder decoder(QStringDecoder::Utf8); const QString text = decoder.decode(bytes);
    require(!decoder.hasError() && !text.contains(QChar::Null), "Project instructions must be UTF-8 text", ErrorCode::ProtocolError);
    return text;
}
QStringList splitPatterns(const QString& input, int depth = 0) {
    require(depth <= 8 && input.size() <= 4096, "Instruction glob exceeds limit", ErrorCode::ResourceLimit);
    QStringList parts; int braces = 0; qsizetype start = 0;
    for (qsizetype i = 0; i < input.size(); ++i) {
        if (input[i] == '{') ++braces;
        if (input[i] == '}') --braces;
        require(braces >= 0, "Unbalanced instruction glob braces");
        if (input[i] == ',' && braces == 0) { parts.append(input.mid(start, i - start).trimmed()); start = i + 1; }
    }
    require(braces == 0, "Unbalanced instruction glob braces");
    parts.append(input.mid(start).trimmed()); QStringList expanded;
    for (const auto& part : parts) {
        if (part.isEmpty()) continue;
        const auto open = part.indexOf('{');
        if (open < 0) expanded.append(part);
        else {
            int nesting = 1; auto close = open + 1;
            for (; close < part.size(); ++close) { if (part[close] == '{') ++nesting; if (part[close] == '}' && --nesting == 0) break; }
            require(close < part.size(), "Unbalanced instruction glob braces");
            for (const auto& item : splitPatterns(part.mid(open + 1, close - open - 1), depth + 1)) {
                expanded.append(splitPatterns(part.left(open) + item + part.mid(close + 1), depth + 1));
                require(expanded.size() <= 128, "Too many instruction glob expansions", ErrorCode::ResourceLimit);
            }
        }
        require(expanded.size() <= 128, "Too many instruction globs", ErrorCode::ResourceLimit);
    }
    return expanded;
}
struct Pattern { QRegularExpression expression; bool negative = false; };
QList<Pattern> compilePatterns(const QStringList& source) {
    QList<Pattern> result;
    for (const auto& line : source) for (auto pattern : splitPatterns(line)) {
        require(result.size() < 128 && pattern.size() <= 512, "Instruction glob exceeds limit", ErrorCode::ResourceLimit);
        bool negative = pattern.startsWith('!'); if (negative) pattern.remove(0, 1);
        require(!pattern.isEmpty() && !pattern.contains('\\') && !pattern.split('/').contains(".."), "Unsupported instruction glob");
        const bool anchored = pattern.startsWith('/'); if (anchored) pattern.remove(0, 1);
        if (pattern.endsWith("/**")) pattern.chop(3);
        if (pattern.endsWith('/')) pattern.chop(1);
        QString regex = !anchored && !pattern.contains('/') ? "^(?:.*/)?" : "^";
        for (qsizetype i = 0; i < pattern.size(); ++i) {
            const auto ch = pattern[i];
            if (ch == '*') {
                if (i + 1 < pattern.size() && pattern[i + 1] == '*' && (i == 0 || pattern[i - 1] == '/')
                    && (i + 2 == pattern.size() || pattern[i + 2] == '/')) {
                    ++i; if (i + 1 < pattern.size()) { ++i; regex += "(?:.*/)?"; } else regex += ".*";
                } else regex += "[^/]*";
            } else if (ch == '?') regex += "[^/]";
            else if (ch == '[') {
                const auto end = pattern.indexOf(']', i + 1); require(end > i + 1, "Invalid instruction glob character class");
                auto body = pattern.mid(i + 1, end - i - 1);
                require(!body.contains('[') && !body.contains('/'), "Invalid instruction glob character class");
                if (body.startsWith('!')) body[0] = '^';
                regex += '[' + body + ']'; i = end;
            } else regex += QRegularExpression::escape(QString(ch));
        }
        QRegularExpression expression(regex + "(?:/.*)?$"); require(expression.isValid(), "Invalid instruction glob");
        result.append({std::move(expression), negative});
    }
    return result;
}
bool matches(const QList<Pattern>& patterns, const QString& relative) {
    if (relative.isEmpty() || relative == "." || relative.startsWith("../") || QDir::isAbsolutePath(relative)) return false;
    bool value = false;
    for (const auto& pattern : patterns) if (pattern.expression.match(relative).hasMatch()) value = !pattern.negative;
    return value;
}
struct Frontmatter { QByteArray body; QStringList paths; };
Frontmatter frontmatter(const QByteArray& bytes, const CancellationToken& token) {
    const auto first = bytes.indexOf('\n');
    if (first < 0 || bytes.left(first).trimmed() != "---") return {bytes, {}};
    qsizetype end = first + 1, body = -1;
    while (end < bytes.size() && end <= 16384) {
        auto next = bytes.indexOf('\n', end); if (next < 0) next = bytes.size();
        if (bytes.mid(end, next - end).trimmed() == "---") { body = std::min(next + 1, bytes.size()); break; }
        end = next + 1;
    }
    require(body >= 0, "Unclosed or oversized instruction frontmatter", ErrorCode::ProtocolError);
    const auto yaml = bytes.mid(first + 1, end - first - 1);
    // Reject aliases before constructing a document; alias graphs must not expand or recurse.
    struct Parser {
        yaml_parser_t value{};
        Parser(const QByteArray& input) {
            require(yaml_parser_initialize(&value), "Cannot initialize YAML parser", ErrorCode::ResourceLimit);
            yaml_parser_set_input_string(&value, reinterpret_cast<const unsigned char*>(input.constData()), size_t(input.size()));
        }
        ~Parser() { yaml_parser_delete(&value); }
    } parser(yaml);
    int depth = 0, count = 0;
    for (;;) {
        token.throwIfCancelled(); yaml_event_t event{};
        require(yaml_parser_parse(&parser.value, &event), "Malformed YAML instruction frontmatter", ErrorCode::ProtocolError);
        const auto type = event.type; yaml_event_delete(&event);
        require(type != YAML_ALIAS_EVENT, "YAML aliases are not supported in instructions", ErrorCode::ProtocolError);
        if (type == YAML_MAPPING_START_EVENT || type == YAML_SEQUENCE_START_EVENT) ++depth;
        if (type == YAML_MAPPING_END_EVENT || type == YAML_SEQUENCE_END_EVENT) --depth;
        require(depth <= 16 && ++count <= 4096, "Instruction frontmatter exceeds limit", ErrorCode::ResourceLimit);
        if (type == YAML_STREAM_END_EVENT) break;
    }
    Parser documentParser(yaml);
    struct Document { yaml_document_t value{}; ~Document() { yaml_document_delete(&value); } } document;
    require(yaml_parser_load(&documentParser.value, &document.value), "Malformed YAML instruction frontmatter", ErrorCode::ProtocolError);
    auto* root = yaml_document_get_root_node(&document.value); QStringList paths;
    if (root) {
        require(root->type == YAML_MAPPING_NODE, "Instruction frontmatter must be a mapping", ErrorCode::ProtocolError);
        bool found = false;
        auto scalar = [](const yaml_node_t* node) {
            require(node && node->type == YAML_SCALAR_NODE, "Instruction paths must contain strings", ErrorCode::ProtocolError);
            return QString::fromUtf8(reinterpret_cast<const char*>(node->data.scalar.value), qsizetype(node->data.scalar.length));
        };
        for (auto pair = root->data.mapping.pairs.start; pair < root->data.mapping.pairs.top; ++pair) {
            if (scalar(yaml_document_get_node(&document.value, pair->key)) != "paths") continue;
            require(!found, "Duplicate instruction paths field", ErrorCode::ProtocolError); found = true;
            auto* node = yaml_document_get_node(&document.value, pair->value);
            if (node->type == YAML_SEQUENCE_NODE) {
                for (auto item = node->data.sequence.items.start; item < node->data.sequence.items.top; ++item)
                    paths.append(splitPatterns(scalar(yaml_document_get_node(&document.value, *item))));
            } else {
                const auto value = scalar(node);
                if (!(node->data.scalar.style == YAML_PLAIN_SCALAR_STYLE && (value.isEmpty() || value == "null" || value == "~")))
                    paths = splitPatterns(value);
            }
        }
    }
    require(paths.size() <= 128, "Too many instruction paths", ErrorCode::ResourceLimit);
    if (std::all_of(paths.cbegin(), paths.cend(), [](const QString& value) { return value == "**"; })) paths.clear();
    return {bytes.mid(body), paths};
}
struct Markdown {
    const QByteArray& source;
    const CancellationToken& token;
    QByteArray allowed;
    QList<QPair<qsizetype, qsizetype>> comments;
    bool html = false;
    qsizetype htmlStart = -1, htmlEnd = 0;
    static int enter(MD_BLOCKTYPE type, void*, void* state) {
        auto& self = *static_cast<Markdown*>(state);
        if (type == MD_BLOCK_HTML) { self.html = true; self.htmlStart = -1; }
        return self.token.isCancelled() ? 1 : 0;
    }
    static int leave(MD_BLOCKTYPE type, void*, void* state) {
        auto& self = *static_cast<Markdown*>(state);
        if (type == MD_BLOCK_HTML) {
            self.html = false;
            if (self.htmlStart >= 0) {
                const auto block = self.source.mid(self.htmlStart, self.htmlEnd - self.htmlStart);
                if (block.trimmed().startsWith("<!--") && block.contains("-->")) {
                    std::fill(self.allowed.begin() + self.htmlStart, self.allowed.begin() + self.htmlEnd, char(1));
                    qsizetype start = 0;
                    while ((start = block.indexOf("<!--", start)) >= 0) {
                        const auto end = block.indexOf("-->", start + 4); if (end < 0) break;
                        self.comments.append({self.htmlStart + start, end + 3 - start});
                        std::fill(self.allowed.begin() + self.htmlStart + start, self.allowed.begin() + self.htmlStart + end + 3, char(0));
                        start = end + 3;
                    }
                }
            }
        }
        return self.token.isCancelled() ? 1 : 0;
    }
    static int span(MD_SPANTYPE, void*, void* state) { return static_cast<Markdown*>(state)->token.isCancelled() ? 1 : 0; }
    static int text(MD_TEXTTYPE type, const MD_CHAR* text, MD_SIZE size, void* state) {
        auto& self = *static_cast<Markdown*>(state);
        const auto address = reinterpret_cast<std::uintptr_t>(text), base = reinterpret_cast<std::uintptr_t>(self.source.constData());
        if (address >= base && address - base <= size_t(self.source.size()) && size <= size_t(self.source.size()) - (address - base)) {
            const auto start = qsizetype(address - base), end = start + size;
            if (type == MD_TEXT_NORMAL) std::fill(self.allowed.begin() + start, self.allowed.begin() + end, char(1));
            if (self.html) { if (self.htmlStart < 0) self.htmlStart = start; self.htmlEnd = end; }
        }
        return self.token.isCancelled() ? 1 : 0;
    }
};
struct ParsedMarkdown { QString text; QStringList imports; };
ParsedMarkdown markdown(const QByteArray& source, const CancellationToken& token) {
    Markdown state{source, token, QByteArray(source.size(), char(0))};
    MD_PARSER parser{}; parser.enter_block = Markdown::enter; parser.leave_block = Markdown::leave;
    parser.enter_span = Markdown::span; parser.leave_span = Markdown::span; parser.text = Markdown::text;
    const int result = md_parse(source.constData(), MD_SIZE(source.size()), &parser, &state);
    token.throwIfCancelled(); require(result == 0, "Cannot parse instruction Markdown", ErrorCode::ProtocolError);
    // Byte-aligned Latin1 is used only for locating references; file paths are decoded as UTF-8 afterwards.
    static const QRegularExpression references(R"((?:^|\s)@((?:[^\s\\]|\\ )+))");
    auto iterator = references.globalMatch(QString::fromLatin1(source)); QStringList imports;
    while (iterator.hasNext()) {
        const auto match = iterator.next(); const auto start = match.capturedStart(1), end = match.capturedEnd(1);
        bool allowed = state.allowed[start - 1];
        for (auto i = start; allowed && i < end; ++i)
            allowed = state.allowed[i] || (source[i] == '\\' && i + 1 < end && source[i + 1] == ' ');
        if (!allowed) continue;
        auto value = source.mid(start, end - start); const auto fragment = value.indexOf('#'); if (fragment >= 0) value.truncate(fragment);
        if (value.isEmpty() || value.startsWith('@')) continue;
        value.replace("\\ ", " "); imports.append(decode(value));
    }
    QByteArray content; qsizetype cursor = 0;
    for (const auto& [start, length] : state.comments) { content += source.mid(cursor, start - cursor); cursor = start + length; }
    content += source.mid(cursor); imports.removeDuplicates(); return {decode(content), imports};
}
class Loader {
public:
    QString root, cwd;
    ProjectContextOptions options;
    CancellationToken token;
    ProjectContext result;
    QSet<QString> visited;
    QList<Pattern> exclusions;
    int fileCount = 0, entries = 0;
    qint64 total = 0;
    QString resolve(QString path, const QString& base) const {
        require(!path.contains(QChar::Null) && path.size() <= 4096 && !path.startsWith("~") && !path.contains("://"), "Invalid project context path");
        path = QDir::cleanPath(QDir::isAbsolutePath(path) ? path : QDir(base).absoluteFilePath(path));
        require(inside(path, root), "Project context path escapes the configured root", ErrorCode::InvalidArgument);
        auto existing = path; QStringList missing;
        while (!QFileInfo(existing).exists()) {
            require(!QFileInfo(existing).isSymLink(), "Dangling instruction symlink", ErrorCode::InvalidArgument);
            const QFileInfo info(existing); missing.prepend(info.fileName());
            const auto parent = info.absolutePath(); require(parent != existing, "Cannot resolve project context path"); existing = parent;
        }
        auto canonical = QFileInfo(existing).canonicalFilePath();
        require(!canonical.isEmpty() && inside(canonical, root), "Instruction symlink escapes the configured root");
        for (const auto& part : missing) canonical = QDir(canonical).filePath(part);
        return canonical;
    }
    QByteArray read(const QString& path) {
        token.throwIfCancelled(); require(++fileCount <= options.maxFiles, "Too many instruction files", ErrorCode::ResourceLimit);
        auto data = detail::readContextFile(root, path, options.maxFileBytes, token);
        total += data.size(); require(total <= options.maxTotalBytes, "Instruction input exceeds total byte limit", ErrorCode::ResourceLimit);
        decode(data); return data;
    }
    void file(const QString& candidate, const QString& scope, bool rule = false, const QString& parent = {}, int depth = 0,
        const QStringList& inheritedPatterns = {}) {
        token.throwIfCancelled(); const auto path = resolve(candidate, cwd);
        if (matches(exclusions, QDir(root).relativeFilePath(path)) || visited.contains(path) || !QFileInfo(path).exists()) return;
        require(QFileInfo(path).isFile(), "Instruction is not a regular file", ErrorCode::StorageFailure);
        require(depth < options.maxImportDepth, "Instruction import depth exceeded", ErrorCode::ResourceLimit);
        visited.insert(path); const auto raw = read(path);
        const auto textBytes = raw.startsWith("\xEF\xBB\xBF") ? raw.mid(3) : raw;
        auto parsed = frontmatter(textBytes, token);
        const auto patterns = compilePatterns(parsed.paths);
        if (rule && !patterns.isEmpty() && std::none_of(result.targetPaths.cbegin(), result.targetPaths.cend(), [&](const auto& target) {
            return matches(patterns, QDir(scope).relativeFilePath(target));
        })) return;
        auto body = markdown(parsed.body, token);
        if (body.text.trimmed().isEmpty()) return;
        const auto effectivePatterns = rule ? parsed.paths : inheritedPatterns;
        result.files.append({path, scope, parent, body.text, effectivePatterns, hash(raw), body.text.toUtf8() != raw});
        for (const auto& imported : body.imports) file(resolve(imported, QFileInfo(path).absolutePath()), scope, false, path, depth + 1, effectivePatterns);
    }
    void rules(const QString& directory) {
        const auto start = resolve(QDir(directory).filePath(".claude/rules"), cwd);
        if (!QFileInfo(start).exists()) return;
        QList<QString> queue{start}; QSet<QString> scanned; QStringList paths;
        while (!queue.isEmpty()) {
            token.throwIfCancelled(); const auto dir = queue.takeLast(); if (scanned.contains(dir)) continue; scanned.insert(dir);
            require(QFileInfo(dir).isDir(), "Instruction rules path is not a directory");
            QDirIterator iterator(dir, QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot);
            while (iterator.hasNext()) {
                token.throwIfCancelled(); require(++entries <= options.maxScannedEntries, "Instruction scan exceeds limit", ErrorCode::ResourceLimit);
                const auto path = resolve(iterator.next(), cwd); const QFileInfo info(path);
                if (matches(exclusions, QDir(root).relativeFilePath(path))) continue;
                if (info.isDir()) queue.append(path);
                else if (info.suffix().compare("md", Qt::CaseInsensitive) == 0) paths.append(path);
            }
        }
        paths.sort(Qt::CaseSensitive); for (const auto& path : paths) file(path, directory, true);
    }
    void directory(const QString& path) {
        file(QDir(path).filePath("CLAUDE.md"), path);
        file(QDir(path).filePath(".claude/CLAUDE.md"), path);
        rules(path);
        file(QDir(path).filePath("CLAUDE.local.md"), path);
        file(QDir(path).filePath("AGENTS.md"), path);
    }
};
}
ProjectContext loadProjectContext(const QString& workingDirectory, const QStringList& targets,
    const ProjectContextOptions& options, const CancellationToken& token) {
    token.throwIfCancelled(); if (!options.enabled) return {};
    require(options.maxFileBytes > 0 && options.maxFileBytes <= 1024 * 1024
        && options.maxTotalBytes > 0 && options.maxTotalBytes <= 16 * 1024 * 1024
        && options.maxFiles > 0 && options.maxFiles <= 4096 && options.maxScannedEntries > 0 && options.maxScannedEntries <= 65536
        && options.maxImportDepth > 0 && options.maxImportDepth <= 32 && options.maxTargetPaths > 0 && options.maxTargetPaths <= 1024,
        "Invalid project context limits");
    Loader loader; loader.options = options; loader.token = token;
    loader.cwd = QFileInfo(workingDirectory).canonicalFilePath();
    loader.root = QFileInfo(options.rootDirectory.isEmpty() ? workingDirectory : options.rootDirectory).canonicalFilePath();
    require(!loader.root.isEmpty() && !QDir(loader.root).isRoot() && QFileInfo(loader.root).isDir()
        && QFileInfo(loader.cwd).isDir() && inside(loader.cwd, loader.root), "Invalid project context root or working directory");
    loader.result.workingDirectory = loader.cwd;
    loader.exclusions = compilePatterns(options.excludes);
    require(targets.size() <= options.maxTargetPaths, "Too many project context targets", ErrorCode::ResourceLimit);
    for (const auto& target : targets) { require(!target.isEmpty(), "Empty project context target"); loader.result.targetPaths.append(loader.resolve(target, loader.cwd)); }
    loader.result.targetPaths.removeDuplicates(); loader.result.targetPaths.sort();
    QStringList directories; auto directory = loader.cwd;
    while (true) { directories.prepend(directory); if (directory == loader.root) break; directory = QFileInfo(directory).absolutePath(); }
    QSet<QString> discovered(directories.cbegin(), directories.cend());
    for (const auto& target : loader.result.targetPaths) {
        auto dir = QFileInfo(target).isDir() ? target : QFileInfo(target).absolutePath(); QStringList nested;
        while (inside(dir, loader.root) && !discovered.contains(dir)) {
            nested.prepend(dir); discovered.insert(dir); dir = QFileInfo(dir).absolutePath();
        }
        directories.append(nested);
    }
    require(directories.size() <= options.maxScannedEntries, "Too many instruction scope directories", ErrorCode::ResourceLimit);
    for (const auto& dir : directories) loader.directory(dir);
    auto payload = loader.result.toJson(); payload.remove("fingerprint");
    loader.result.fingerprint = hash(QJsonDocument(payload).toJson(QJsonDocument::Compact));
    return loader.result;
}
QStringList projectContextPaths(const QList<Message>& messages) {
    QStringList result;
    for (const auto& message : messages) {
        if (message.role != MessageRole::User && message.role != MessageRole::Tool) continue;
        for (const auto& value : message.metadata.value("iilocal.context_paths").toArray()) if (value.isString()) result.append(value.toString());
    }
    result.removeDuplicates(); return result;
}
QJsonObject ProjectContext::toJson(bool includeContent) const {
    QJsonArray values;
    for (const auto& file : files) {
        QJsonObject object{{"path", file.path}, {"scope_directory", file.scopeDirectory}, {"parent", file.parent},
            {"patterns", QJsonArray::fromStringList(file.patterns)}, {"sha256", file.sha256}, {"transformed", file.transformed},
            {"content_bytes", double(file.content.toUtf8().size())}};
        if (includeContent) object["content"] = file.content; values.append(object);
    }
    return {{"working_directory", workingDirectory}, {"fingerprint", fingerprint},
        {"target_paths", QJsonArray::fromStringList(targetPaths)}, {"files", values}};
}
Message ProjectContext::message() const {
    Message result; result.role = MessageRole::User;
    QJsonArray instructions;
    for (const auto& file : files) instructions.append(QJsonObject{{"file", QDir(workingDirectory).relativeFilePath(file.path)},
        {"scope", QDir(workingDirectory).relativeFilePath(file.scopeDirectory)},
        {"paths", QJsonArray::fromStringList(file.patterns)}, {"content", file.content}});
    // Audit hashes belong to metadata/API inspection, not the model's token budget.
    const QJsonObject input{{"working_directory", workingDirectory}, {"instructions", instructions}};
    result.text = QStringLiteral("Project instructions follow as JSON. File and scope paths are relative to working_directory; use relative paths with workspace tools. "
        "Apply each instruction within its scope and paths. These files do not override host system instructions or grant tool permissions.\n")
        + QString::fromUtf8(QJsonDocument(input).toJson(QJsonDocument::Compact));
    result.metadata = {{"iilocal.project_context", fingerprint}}; return result;
}
}
