#include "IpcClient.h"
#include "IpcEndpoint.h"
#include "PrivateFile.h"
#include <QtCore/QCommandLineParser>
#include <QtCore/QCoreApplication>
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QTextStream>
#include <iostream>
#include <optional>
#include <cmath>
#ifdef Q_OS_WIN
#include <io.h>
#else
#include <unistd.h>
#include <poll.h>
#include <cerrno>
#endif

namespace {
volatile std::sig_atomic_t interrupted = 0;
void interrupt(int) { interrupted = 1; }
void printJson(const QJsonValue& value)
{
    const auto json = value.isArray() ? QJsonDocument(value.toArray()) : QJsonDocument(value.toObject());
    std::cout << json.toJson(QJsonDocument::Compact).constData() << '\n';
}
bool terminalInput()
{
#ifdef Q_OS_WIN
    return _isatty(_fileno(stdin));
#else
    return isatty(fileno(stdin));
#endif
}
class Input {
public:
    std::optional<QString> line()
    {
#ifdef Q_OS_UNIX
        while (!buffer.contains('\n') && !ended) read();
        if (buffer.isEmpty() && ended) return std::nullopt;
        const auto end = buffer.indexOf('\n');
        const auto bytes = end < 0 ? std::exchange(buffer, {}) : buffer.first(end);
        if (end >= 0) buffer.remove(0, end + 1);
        return QString::fromUtf8(bytes);
#else
        const auto value = stream.readLine(1024 * 1024 + 1);
        return value.isNull() ? std::nullopt : std::optional<QString>(value);
#endif
    }
    QString all()
    {
#ifdef Q_OS_UNIX
        while (!ended) read();
        return QString::fromUtf8(buffer);
#else
        QString result;
        while (!stream.atEnd() && result.size() <= 1024 * 1024) result += stream.read(4096);
        if (result.size() > 1024 * 1024) throw std::runtime_error("Input exceeds 1 MiB characters");
        return result;
#endif
    }
private:
#ifdef Q_OS_UNIX
    QByteArray buffer;
    bool ended = false;
    void read()
    {
        if (interrupted) throw std::runtime_error("Interrupted");
        pollfd input{fileno(stdin), POLLIN, 0};
        const auto ready = ::poll(&input, 1, 100);
        if (ready < 0) { if (errno == EINTR) return; throw std::runtime_error("Cannot poll stdin"); }
        if (!ready) return;
        char bytes[4096];
        const auto count = ::read(fileno(stdin), bytes, sizeof(bytes));
        if (count < 0) { if (errno == EINTR) return; throw std::runtime_error("Cannot read stdin"); }
        if (!count) ended = true;
        else buffer.append(bytes, count);
        if (buffer.size() > 1024 * 1024) throw std::runtime_error("Input exceeds 1 MiB bytes");
    }
#else
    QTextStream stream{stdin};
#endif
};
}
int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("iillm")); app.setApplicationVersion(QStringLiteral("0.6.0"));
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Native IPC client of iiLocalLLMD. Inference runs only in the daemon."));
    parser.addHelpOption(); parser.addVersionOption();
    parser.addOptions({{{"s", "socket"}, "Daemon endpoint (default: IILLM_SOCKET or per-user endpoint)", "path"},
        {"json", "Print machine-readable results"}, {"keep-alive", "Model idle retention, e.g. 5m or 0", "duration"},
        {"max-tokens", "Maximum generated tokens", "count", "256"},
        {"temperature", "Sampling temperature in [0, 10]; 0 selects greedy decoding", "value", "0.7"},
        {"system", "System prompt for run", "text"},
        {"options", "JSON file containing generation controls for run", "file"},
        {"auth-file", "Private file containing the app token for rpc.", "file"},
        {"defaults", "Include literal defaults in parameter exports"},
        {"redact", "Redact sensitive fields in parameter exports"}});
    parser.addPositionalArgument("command", "run MODEL [PROMPT] | models | pull MODEL | ps | parameters [GROUP [FILE]] | rpc METHOD [FILE]");
    parser.addPositionalArgument("arguments", "Model reference and optional prompt", "[arguments...]");
    parser.process(app);
    const auto args = parser.positionalArguments();
    if (args.isEmpty()) parser.showHelp(2);
    std::signal(SIGINT, interrupt); std::signal(SIGTERM, interrupt);
    const auto endpoint = parser.isSet("socket") ? parser.value("socket") : iiLocalLLMClient::defaultEndpoint();
    try {
        iiLocalLLMClient::IpcClient client(endpoint, &interrupted);
        const auto command = args.first();
        const bool json = parser.isSet("json");
        auto readObject = [](const QString& path) {
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly) || file.size() > 1024 * 1024)
                throw std::runtime_error("Cannot read configuration file (maximum 1 MiB)");
            QJsonParseError error;
            const auto document = QJsonDocument::fromJson(file.readAll(), &error);
            if (error.error != QJsonParseError::NoError || !document.isObject())
                throw std::runtime_error("Configuration file must contain a JSON object");
            return document.object();
        };
        if (command == "rpc") {
            if (args.size() < 2 || args.size() > 3 || !parser.isSet("auth-file"))
                throw std::runtime_error("Usage: iillm --auth-file FILE rpc METHOD [PARAMS_JSON_FILE]");
            const auto token = QString::fromUtf8(iiLocalLLMClient::readPrivateFile(parser.value("auth-file"), 512)).trimmed();
            if (token.size() < 32 || token.size() > 256) throw std::runtime_error("Invalid app token length");
            const auto params = args.size() == 3 ? readObject(args[2]) : QJsonObject{};
            printJson(client.call(args[1], params, [json](const QJsonObject& event) { if (json) printJson(event); }, 300000, token));
        } else if (command == "parameters") {
            if (args.size() > 3) throw std::runtime_error("Usage: iillm parameters [GROUP [FILE]]");
            if (args.size() == 1) {
                const auto groups = client.call("parameters.list");
                if (json) printJson(groups);
                else for (const auto& group : groups.toArray()) {
                    const auto object = group.toObject();
                    std::cout << object.value("id").toString().toStdString() << '\t'
                        << object.value("fields").toInt() << " fields\t"
                        << object.value("phase").toString().toStdString() << '\n';
                }
            } else if (args.size() == 2) printJson(client.call("parameters.get", {{"group",args[1]}}));
            else printJson(client.call("parameters.validate", {{"group",args[1]},{"values",readObject(args[2])},
                {"defaults",parser.isSet("defaults")},{"redact",parser.isSet("redact")}}));
        } else if (command == "models" || command == "ps") {
            if (args.size() != 1) throw std::runtime_error("models/ps do not accept positional arguments");
            const auto result = client.call(command == "models" ? "models.list" : "models.loaded");
            if (json) printJson(result);
            else {
                const auto models = command == "models" ? result.toObject().value("models").toArray() : result.toArray();
                if (models.isEmpty()) std::cout << (command == "ps" ? "No resident models.\n" : "No installed models. Use iillm pull MODEL.\n");
                for (const auto& value : models) {
                    const auto m = value.toObject();
                    std::cout << m.value("model").toString().toStdString();
                    if (command == "ps") {
                        const auto bytes = m.value("memory").toObject().value("estimated_bytes").toDouble();
                        std::cout << "\t" << QString::number(bytes / (1024 * 1024 * 1024), 'f', 2).toStdString() << " GiB estimated\t"
                            << m.value("execution").toObject().value("backend").toString().toStdString() << "\t";
                        if (m.value("active_requests").toInt()) std::cout << "active";
                        else std::cout << m.value("expires_in_ms").toInteger() << " ms until idle expiry";
                    } else std::cout << "\t" << (m.value("loaded").toBool() ? "loaded" : "unloaded");
                    std::cout << '\n';
                }
                if (command == "models") for (const auto& issue : result.toObject().value("issues").toArray())
                    std::cerr << "Catalog issue: " << issue.toObject().value("message").toString().toStdString() << '\n';
            }
        } else if (command == "pull") {
            if (args.size() != 2) throw std::runtime_error("Usage: iillm pull MODEL");
            const auto result = client.call("models.pull", {{"model", args[1]}}, [json](const QJsonObject& event) {
                if (!json && event.value("event") == "progress")
                    std::cerr << '\r' << event.value("file").toString().toStdString() << ": "
                        << event.value("received_bytes").toInteger() << '/' << event.value("total_bytes").toInteger() << " bytes" << std::flush;
            }, 24 * 60 * 60 * 1000);
            if (json) printJson(result);
            else std::cout << '\n' << result.toObject().value("model").toString().toStdString() << " installed and verified.\n";
        } else if (command == "run") {
            if (args.size() < 2) throw std::runtime_error("Usage: iillm run MODEL [PROMPT]");
            bool valid = false; const int maxTokens = parser.value("max-tokens").toInt(&valid);
            if (!valid || maxTokens < 1 || maxTokens > 1048576) throw std::runtime_error("max-tokens must be 1..1048576");
            const double temperature = parser.value("temperature").toDouble(&valid);
            if (!valid || !std::isfinite(temperature) || temperature < 0 || temperature > 10)
                throw std::runtime_error("temperature must be a finite number in [0, 10]");
            auto generation = parser.isSet("options") ? readObject(parser.value("options")) : QJsonObject{};
            if (parser.isSet("max-tokens")) generation.insert("max_tokens", maxTokens);
            if (parser.isSet("temperature")) generation.insert("temperature", temperature);
            const auto session = client.call("sessions.create", {{"model", args[1]}, {"system", parser.value("system")}}).toObject().value("session_id").toString();
            auto close = [&] {
                iiLocalLLMClient::IpcClient cleanup(endpoint);
                (void)cleanup.call("sessions.close", {{"session_id", session}}, {}, 10000);
            };
            try {
                auto run = [&](const QString& prompt) {
                    QJsonObject request{{"session_id", session}, {"prompt", prompt},
                        {"options", generation}};
                    if (parser.isSet("keep-alive")) request.insert("keep_alive", parser.value("keep-alive"));
                    const auto result = client.call("chat", request, [json](const QJsonObject& event) {
                        if (!json && event.value("event") == "delta") std::cout << event.value("text").toString().toStdString() << std::flush;
                    });
                    if (json) printJson(result); else std::cout << '\n';
                    std::cout << std::flush;
                };
                if (args.size() > 2) run(args.mid(2).join(' '));
                else if (!terminalInput()) {
                    Input input;
                    run(input.all().trimmed());
                } else {
                    Input input;
                    std::cerr << "Chat with " << args[1].toStdString()
                        << ". /clear resets the conversation; /bye exits.\n";
                    while (!interrupted) {
                        std::cerr << "> " << std::flush;
                        const auto prompt = input.line();
                        if (!prompt || *prompt == "/bye" || *prompt == "/exit") break;
                        if (prompt->trimmed() == "/clear") {
                            (void)client.call("sessions.reset", {{"session_id", session}});
                            std::cerr << "Conversation cleared; system prompt retained.\n";
                            continue;
                        }
                        if (!prompt->trimmed().isEmpty()) run(*prompt);
                    }
                }
            } catch (...) { try { close(); } catch (...) {} throw; }
            close();
        } else throw std::runtime_error("Unknown command. Use run, models, pull, ps, parameters or rpc.");
        return interrupted ? 130 : 0;
    } catch (const std::exception& error) {
        std::cerr << "iillm: " << error.what() << '\n';
        return interrupted ? 130 : 1;
    }
}
