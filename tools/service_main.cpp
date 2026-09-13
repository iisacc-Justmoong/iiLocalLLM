#include <iiLocalLLM.h>
#include "IpcEndpoint.h"
#include <QtCore/QCommandLineParser>
#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QTimer>
#include <csignal>
#include <iostream>

namespace {
volatile std::sig_atomic_t interrupted = 0;
void interrupt(int) { interrupted = 1; }
QString required(const QJsonObject& object, const QString& key)
{
    const auto value = object.value(key);
    if (!value.isString() || value.toString().trimmed().isEmpty())
        throw iiLocalLLM::Error(iiLocalLLM::ErrorCode::InvalidArgument, key + QStringLiteral(" is required"));
    return value.toString();
}
}
int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("iiLocalLLMD"));
    app.setApplicationVersion(QStringLiteral("0.3.0"));
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("iiLocalLLM local JSON IPC service"));
    parser.addHelpOption(); parser.addVersionOption();
    parser.addOptions({{{QStringLiteral("s"), QStringLiteral("socket")}, QStringLiteral("Local socket path/name"), QStringLiteral("path")},
        {{QStringLiteral("c"), QStringLiteral("config")}, QStringLiteral("JSON model catalog (optional)"), QStringLiteral("file")},
        {QStringLiteral("mlx-python"), QStringLiteral("Python with mlx-lm installed"), QStringLiteral("executable"), QStringLiteral("python3")},
        {QStringLiteral("mlx-worker"), QStringLiteral("Path to mlx_worker.py"), QStringLiteral("path")},
        {QStringLiteral("models-root"), QStringLiteral("Service-owned Models directory"), QStringLiteral("directory"), QStringLiteral("Models")},
        {QStringLiteral("registry"), QStringLiteral("Download registry JSON (aliases, pinned manifests and URLs)"), QStringLiteral("file")},
        {QStringLiteral("memory-budget-mib"), QStringLiteral("Model residency budget; 0 uses physical RAM policy"), QStringLiteral("MiB"), QStringLiteral("0")},
        {QStringLiteral("memory-reserve-mib"), QStringLiteral("Live available RAM headroom"), QStringLiteral("MiB"), QStringLiteral("256")},
        {QStringLiteral("max-models"), QStringLiteral("Maximum resident models"), QStringLiteral("count"), QStringLiteral("4")},
        {QStringLiteral("context-tokens"), QStringLiteral("Default model context capacity"), QStringLiteral("count"), QStringLiteral("2048")},
        {QStringLiteral("keep-alive"), QStringLiteral("Idle model lifetime, e.g. 5m or 0; default is hardware-dependent"), QStringLiteral("duration")},
        {QStringLiteral("http-port"), QStringLiteral("Enable HTTP on 127.0.0.1; 0 selects an available port"), QStringLiteral("port")},
        {QStringLiteral("install"), QStringLiteral("Install a local manifest bundle; repeat for multiple bundles"), QStringLiteral("directory")},
        {QStringLiteral("hardware"), QStringLiteral("Inspect startup hardware as JSON and exit")}});
    parser.process(app);
    try {
        auto worker = parser.value(QStringLiteral("mlx-worker"));
        iiLocalLLM::ServiceOptions options;
        options.modelsDirectory = parser.value(QStringLiteral("models-root"));
        options.registryFile = parser.value(QStringLiteral("registry"));
        auto integer = [&](const char* key, quint64 maximum) {
            bool valid = false;
            const auto value = parser.value(QLatin1String(key)).toULongLong(&valid);
            if (!valid || value > maximum) throw std::runtime_error(std::string(key) + " has an invalid integer value");
            return value;
        };
        options.memoryBudgetBytes = integer("memory-budget-mib", 1048576) * 1024 * 1024;
        options.memoryReserveBytes = integer("memory-reserve-mib", 1048576) * 1024 * 1024;
        options.maxModels = int(integer("max-models", 1024));
        options.defaultContextTokens = int(integer("context-tokens", 1048576));
        if (parser.isSet("keep-alive")) options.keepAliveMs = iiLocalLLM::parseKeepAlive(parser.value("keep-alive"));
        iiLocalLLM::Service service(options, {parser.value(QStringLiteral("mlx-python")), worker});
        if (parser.isSet(QStringLiteral("hardware"))) {
            std::cout << QJsonDocument(iiLocalLLM::hardwareObject(service.hardware())).toJson().constData();
            return 0;
        }
        for (const auto& directory : parser.values(QStringLiteral("install"))) {
            const auto model = service.installModel(directory).get();
            std::cout << QJsonDocument(iiLocalLLM::modelRecordObject(model)).toJson(QJsonDocument::Compact).constData() << std::endl;
        }
        if (parser.isSet("install") && !parser.isSet("socket") && !parser.isSet("http-port")) return 0;
        if (parser.isSet(QStringLiteral("config"))) {
            const auto configPath = parser.value(QStringLiteral("config"));
            QFile config(configPath);
            if (!config.open(QIODevice::ReadOnly)) throw std::runtime_error(config.errorString().toStdString());
            if (config.size() > 1024 * 1024) throw std::runtime_error("Config exceeds 1 MiB");
            QJsonParseError parse;
            const auto document = QJsonDocument::fromJson(config.readAll(), &parse);
            if (parse.error != QJsonParseError::NoError || !document.isObject()
                || !document.object().value(QStringLiteral("models")).isArray())
                throw std::runtime_error("Config must contain a models array");
            for (const auto& value : document.object().value(QStringLiteral("models")).toArray()) {
                if (!value.isObject()) throw std::runtime_error("Model must be an object");
                const auto o = value.toObject();
                for (auto it = o.begin(); it != o.end(); ++it) {
                    if (it.key() != QStringLiteral("model")
                        && it.key() != QStringLiteral("context_tokens") && it.key() != QStringLiteral("options") && it.key() != QStringLiteral("keep_alive"))
                        throw iiLocalLLM::Error(iiLocalLLM::ErrorCode::InvalidArgument,
                            QStringLiteral("Unknown model field; execution is selected by the service: ") + it.key());
                }
                const auto ctx = o.value(QStringLiteral("context_tokens"));
                if (!ctx.isUndefined() && (!ctx.isDouble() || ctx.toDouble() != ctx.toInt() || ctx.toInt() < 0 || ctx.toInt() > 1024 * 1024))
                    throw std::runtime_error("context_tokens must be an integer in [0, 1048576]");
                if (o.contains(QStringLiteral("options")) && !o.value(QStringLiteral("options")).isObject())
                    throw std::runtime_error("options must be an object");
                const auto model = service.loadModel({required(o, QStringLiteral("model")),
                    ctx.toInt(0), o.value(QStringLiteral("options")).toObject(), iiLocalLLM::parseKeepAlive(o.value("keep_alive"))}).get();
                std::cout << "Model " << model.model.uri.toStdString() << ": "
                    << QJsonDocument(iiLocalLLM::executionObject(model.execution)).toJson(QJsonDocument::Compact).constData() << std::endl;
            }
        }
        iiLocalLLM::LocalIpcServer server(service);
        if (parser.isSet(QStringLiteral("socket")) || !parser.isSet(QStringLiteral("http-port"))) {
            if (!server.listen(parser.isSet("socket") ? parser.value("socket") : iiLocalLLMClient::defaultEndpoint())) throw std::runtime_error(server.errorString().toStdString());
            std::cout << "iiLocalLLM listening: " << server.serverName().toStdString() << std::endl;
        }
        iiLocalLLM::HttpApiServer http(service);
        if (parser.isSet(QStringLiteral("http-port"))) {
            bool valid = false;
            const auto port = parser.value(QStringLiteral("http-port")).toUInt(&valid);
            if (!valid || port > 65535) throw std::runtime_error("http-port must be an integer in [0, 65535]");
            if (!http.listen(quint16(port))) throw std::runtime_error(http.errorString().toStdString());
            std::cout << "iiLocalLLM HTTP: http://127.0.0.1:" << http.port() << std::endl;
        }
        std::signal(SIGINT, interrupt); std::signal(SIGTERM, interrupt);
        QTimer shutdown;
        QObject::connect(&shutdown, &QTimer::timeout, &app, [&] { if (interrupted) app.quit(); });
        shutdown.start(100);
        return app.exec();
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
