#include "Runtime.h"
#include "Parameters.h"
#include "MemoryEstimate.h"
#include <QtCore/QFile>
#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QProcess>
#include <QtCore/QProcessEnvironment>
#include <QtCore/QUuid>

namespace iiLocalLLM {
namespace {
class MlxWorker {
public:
    MlxWorker(ModelSpec spec, MlxRuntimeOptions options, RuntimeDevice device)
        : spec(std::move(spec)), options(std::move(options)), device(std::move(device)) {}
    ~MlxWorker() { stop(); }
    void stop()
    {
        if (process.state() != QProcess::NotRunning) { process.kill(); process.waitForFinished(1000); }
        input.clear();
    }
    void start(const CancellationToken& cancel)
    {
        if (process.state() == QProcess::Running) return;
        cancel.throwIfCancelled();
        input.clear(); diagnostics.clear();
        auto env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("HF_HUB_OFFLINE"), QStringLiteral("1"));
        env.insert(QStringLiteral("TRANSFORMERS_OFFLINE"), QStringLiteral("1"));
        env.insert(QStringLiteral("TOKENIZERS_PARALLELISM"), QStringLiteral("false"));
        env.insert(QStringLiteral("PYTHONUNBUFFERED"), QStringLiteral("1"));
        process.setProcessEnvironment(env);
        process.setProgram(options.pythonExecutable);
        process.setArguments({QStringLiteral("-u"), options.workerScript});
        process.start();
        if (!process.waitForStarted(5000)) {
            const auto error = process.errorString();
            stop();
            throw Error(ErrorCode::RuntimeUnavailable, error);
        }
        const auto result = call({{QStringLiteral("op"), QStringLiteral("load")}, {QStringLiteral("path"), spec.path},
              {QStringLiteral("context_tokens"), spec.contextTokens}, {QStringLiteral("backend"), enumName(device.backend)}}, cancel);
        if (result.value(QStringLiteral("backend")).toString() != enumName(device.backend)) {
            stop();
            throw Error(ErrorCode::ProtocolError, QStringLiteral("MLX worker did not confirm the selected execution backend"));
        }
    }
    QJsonObject call(const QJsonObject& request, const CancellationToken& cancel, const TextCallback& onText = {}, int timeout = 0)
    {
        QElapsedTimer elapsed;
        elapsed.start();
        const auto wire = QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n';
        if (process.write(wire) != wire.size()) { stop(); throw Error(ErrorCode::RuntimeFailure, QStringLiteral("MLX worker write failed")); }
        try {
            for (;;) {
                cancel.throwIfCancelled();
                if (elapsed.elapsed() > (timeout > 0 ? timeout : options.timeoutMs))
                    throw Error(ErrorCode::Timeout, QStringLiteral("MLX worker timed out"));
                process.waitForReadyRead(20);
                input += process.readAllStandardOutput();
                diagnostics = (diagnostics + process.readAllStandardError()).right(4096);
                if (input.size() > 16 * 1024 * 1024) throw Error(ErrorCode::ProtocolError, QStringLiteral("MLX worker frame too large"));
                for (;;) {
                    const auto pos = input.indexOf('\n');
                    if (pos < 0) break;
                    const auto bytes = input.first(pos);
                    input.remove(0, pos + 1);
                    QJsonParseError error;
                    const auto doc = QJsonDocument::fromJson(bytes, &error);
                    if (error.error != QJsonParseError::NoError || !doc.isObject())
                        throw Error(ErrorCode::ProtocolError, QStringLiteral("Invalid MLX worker JSON"));
                    const auto event = doc.object();
                    const auto type = event.value(QStringLiteral("type")).toString();
                    if (type == QStringLiteral("error"))
                        throw Error(ErrorCode::RuntimeFailure, event.value(QStringLiteral("message")).toString());
                    if (type == QStringLiteral("result")) return event;
                    if (type != QStringLiteral("delta") || !onText || !event.value(QStringLiteral("text")).isString())
                        throw Error(ErrorCode::ProtocolError, QStringLiteral("Unexpected MLX worker event"));
                    if (!onText(event.value(QStringLiteral("text")).toString())) {
                        // Process termination also releases every native MLX cache; next request reloads lazily.
                        stop();
                        return {{QStringLiteral("finish_reason"), QStringLiteral("stop")},
                            {QStringLiteral("generated_tokens"), event.value(QStringLiteral("generated_tokens"))},
                            {QStringLiteral("cached_tokens"), event.value(QStringLiteral("cached_tokens"))}};
                    }
                }
                if (process.state() == QProcess::NotRunning)
                    throw Error(ErrorCode::RuntimeFailure, QStringLiteral("MLX worker exited: ") + QString::fromUtf8(diagnostics));
            }
        } catch (...) { stop(); throw; }
    }
    ModelSpec spec;
    MlxRuntimeOptions options;
    RuntimeDevice device;
    QProcess process;
private:
    QByteArray input, diagnostics;
};
class MlxContext final : public RuntimeContext {
public:
    explicit MlxContext(std::shared_ptr<MlxWorker> worker) : worker(std::move(worker)), id(QUuid::createUuid().toString(QUuid::WithoutBraces)) {}
    ~MlxContext() override
    {
        if (worker->process.state() == QProcess::Running) {
            try { worker->call({{QStringLiteral("op"), QStringLiteral("drop")}, {QStringLiteral("context_id"), id}}, {}, {}, 1000); }
            catch (...) {}
        }
    }
    RuntimeResult generate(const TokenList& prompt, const GenerationOptions& options, const CancellationToken& cancel,
                           const TextCallback& onText) override
    {
        validateGenerationOptions(options, "mlx");
        worker->start(cancel);
        QJsonArray tokens;
        for (const auto token : prompt) tokens.append(token);
        auto request = generationOptionsToJson(options);
        request.insert("op", "generate"); request.insert("context_id", id); request.insert("tokens", tokens);
        const auto result = worker->call(request, cancel, onText);
        const auto reason = result.value(QStringLiteral("finish_reason")).toString();
        if (reason != QStringLiteral("stop") && reason != QStringLiteral("length"))
            throw Error(ErrorCode::ProtocolError, QStringLiteral("MLX returned an invalid finish reason"));
        return {reason == QStringLiteral("stop") ? FinishReason::Stop : FinishReason::Length,
            result.value(QStringLiteral("generated_tokens")).toInt(), result.value(QStringLiteral("cached_tokens")).toInt()};
    }
private:
    std::shared_ptr<MlxWorker> worker;
    QString id;
};
class MlxModel final : public RuntimeModel {
public:
    explicit MlxModel(std::shared_ptr<MlxWorker> worker) : worker(std::move(worker)) {}
    TokenList tokenize(const QList<ChatMessage>& messages, const CancellationToken& cancel) override
    {
        worker->start(cancel);
        QJsonArray chat;
        for (const auto& message : messages)
            chat.append(QJsonObject{{QStringLiteral("role"), enumName(message.role)}, {QStringLiteral("content"), message.content}});
        const auto result = worker->call({{QStringLiteral("op"), QStringLiteral("tokenize")}, {QStringLiteral("messages"), chat}}, cancel);
        if (!result.value(QStringLiteral("tokens")).isArray())
            throw Error(ErrorCode::ProtocolError, QStringLiteral("MLX tokenizer omitted tokens"));
        TokenList tokens;
        for (const auto& v : result.value(QStringLiteral("tokens")).toArray()) {
            if (!v.isDouble() || v.toDouble() != v.toInt(-1) || v.toInt(-1) < 0)
                throw Error(ErrorCode::ProtocolError, QStringLiteral("MLX tokenizer returned an invalid token"));
            tokens.append(v.toInt());
        }
        return tokens;
    }
    std::unique_ptr<RuntimeContext> createContext(const CancellationToken& cancel) override
    { cancel.throwIfCancelled(); return std::make_unique<MlxContext>(worker); }
private:
    std::shared_ptr<MlxWorker> worker;
};
class MlxRuntime final : public Runtime {
public:
    explicit MlxRuntime(MlxRuntimeOptions options) : options(std::move(options)) {}
    QString id() const override { return QStringLiteral("mlx"); }
    bool supportsModel(const ModelSpec& spec) const override
    {
        if (!spec.format.isEmpty() && spec.format != QStringLiteral("mlx")) return false;
        const QDir dir(spec.path);
        return dir.exists() && QFileInfo(dir.filePath(QStringLiteral("config.json"))).isFile()
            && (QFileInfo(dir.filePath(QStringLiteral("tokenizer.json"))).isFile()
                || QFileInfo(dir.filePath(QStringLiteral("tokenizer.model"))).isFile())
            && !dir.entryList({QStringLiteral("*.safetensors")}, QDir::Files).isEmpty();
    }
    QList<RuntimeDevice> devices(const HardwareInfo& hardware) const override
    {
        QList<RuntimeDevice> result{{ComputeBackend::Cpu, {}}};
        // This worker implements MLX CPU/Metal, not CUDA/Vulkan MLX variants.
        // Apple Silicon has one built-in Metal GPU; do not claim arbitrary external GPU routing.
        if (hardware.appleSilicon) {
            for (const auto& gpu : hardware.gpus) {
                if (gpu.id.startsWith(QStringLiteral("metal/")) && gpu.vendor == GpuVendor::Apple
                    && gpu.availableBackends.contains(ComputeBackend::Metal)) {
                    result.append({ComputeBackend::Metal, gpu.id});
                    break;
                }
            }
        }
        return result;
    }
    MemoryEstimate estimateMemory(const ModelSpec& spec, int contextSlots) const override
    {
        auto estimate = Runtime::estimateMemory(spec, contextSlots);
        QFile config(QDir(spec.path).filePath(QStringLiteral("config.json")));
        if (!config.open(QIODevice::ReadOnly) || config.size() > 1024 * 1024) return estimate;
        const auto object = QJsonDocument::fromJson(config.readAll()).object();
        auto integer = [&](const char* name, quint64 fallback = 0) -> quint64 {
            const auto value = object.value(QLatin1String(name));
            return value.isDouble() && value.toDouble() > 0 && value.toDouble() <= 1048576
                && value.toDouble() == value.toInt() ? quint64(value.toInt()) : fallback;
        };
        const auto heads = integer("num_attention_heads");
        const auto dim = integer("head_dim", heads ? integer("hidden_size") / heads : 0);
        const auto context = detail::kvReservation(integer("num_hidden_layers"), integer("num_key_value_heads", heads),
            dim, dim, spec.contextTokens, contextSlots);
        if (context) { estimate.contextBytes = context; estimate.basis = QStringLiteral("MLX bundle sizes + f16 KV config + scratch reserve"); }
        return estimate;
    }
    std::shared_ptr<RuntimeModel> load(const ModelSpec& spec, const RuntimeDevice& device, const CancellationToken& cancel) override
    {
        if (!QFileInfo(spec.path).isDir()) throw Error(ErrorCode::NotFound, QStringLiteral("MLX requires a local model directory"));
        if (!QFileInfo(options.workerScript).isFile() || options.pythonExecutable.isEmpty() || options.timeoutMs < 1)
            throw Error(ErrorCode::RuntimeUnavailable, QStringLiteral("Set MLX Python executable, worker script and positive timeout in the service installation"));
        if (device.backend != ComputeBackend::Cpu && device.backend != ComputeBackend::Metal)
            throw Error(ErrorCode::RuntimeUnavailable, QStringLiteral("This MLX worker supports CPU and Metal only"));
        if (!spec.options.isEmpty()) throw Error(ErrorCode::InvalidArgument, QStringLiteral("MLX model options are not supported; configure the model tokenizer files"));
        auto worker = std::make_shared<MlxWorker>(spec, options, device);
        worker->start(cancel);
        return std::make_shared<MlxModel>(worker);
    }
private:
    MlxRuntimeOptions options;
};
}
std::shared_ptr<Runtime> createMlxRuntime(MlxRuntimeOptions options)
{
    if (options.workerScript.isEmpty()) options.workerScript = QDir(QCoreApplication::applicationDirPath()).filePath(
        QStringLiteral("../share/iiLocalLLM/runtimes/mlx_worker.py"));
    return std::make_shared<MlxRuntime>(std::move(options));
}
} // namespace iiLocalLLM
