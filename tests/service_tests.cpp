#include <iiLocalLLM.h>
#include "../src/runtimes/Utf8Stream.h"
#include "QtEventWait.h"
#include <QtCore/QCoreApplication>
#include <QtCore/QScopeGuard>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonArray>
#include <QtCore/QUuid>
#include <QtCore/QTemporaryDir>
#include <QtNetwork/QLocalSocket>
#include <atomic>
#include <chrono>
#include <limits>
#include <thread>

using namespace iiLocalLLM;
using namespace std::chrono_literals;

struct Probe {
    std::atomic<int> loads{0}, unloads{0}, contexts{0}, destroyed{0}, active{0}, peak{0};
    std::atomic<int> preparedDestroyedAfterUnload{0};
    quint64 memoryBytes = 128 * 1024 * 1024;
    std::atomic<bool> started{false}, release{true}, fail{false};
    QString answer = QStringLiteral("가나다");
    QList<ConversationRequest> preparedConversations;
    qint64 nativeInputTokens=0;
    std::atomic<int> embeddingCalls{0};
    bool invalidEmbedding = false;
};
class FakePromptState final : public RuntimePromptState {
public:
    explicit FakePromptState(std::shared_ptr<Probe> probe) : p(std::move(probe)), unloaded(p->unloads.load()) {}
    ~FakePromptState() override { if (p->unloads.load() > unloaded) ++p->preparedDestroyedAfterUnload; }
private:
    std::shared_ptr<Probe> p;
    int unloaded;
};

class FakeContext final : public RuntimeContext {
public:
    explicit FakeContext(std::shared_ptr<Probe> probe) : p(std::move(probe)) { ++p->contexts; }
    ~FakeContext() override { ++p->destroyed; }
    RuntimeResult generateConversation(const RuntimeConversationPrompt& prompt, const GenerationOptions& options,
        const CancellationToken& cancel, const TextCallback& emitText) override {
        return generate(prompt.tokens, options, cancel, emitText);
    }
    RuntimeResult generate(const TokenList& prompt, const GenerationOptions&,
                           const CancellationToken& cancel, const TextCallback& emitText) override
    {
        ++p->active;
        p->peak = std::max(p->peak.load(), p->active.load());
        struct Exit { Probe* p; ~Exit() { --p->active; } } exit{p.get()};
        p->started = true;
        while (!p->release) { cancel.throwIfCancelled(); std::this_thread::sleep_for(1ms); }
        cancel.throwIfCancelled();
        if (p->fail) throw Error(ErrorCode::RuntimeFailure, QStringLiteral("injected failure"));
        int reuse = 0;
        while (reuse < std::min(previous.size(), prompt.size()) && previous[reuse] == prompt[reuse]) ++reuse;
        for (const QChar ch : p->answer) {
            cancel.throwIfCancelled();
            if (!emitText(QString(ch))) break;
        }
        previous = prompt;
        return {FinishReason::Stop, 3, reuse};
    }
private:
    std::shared_ptr<Probe> p;
    TokenList previous;
};

class FakeModel final : public RuntimeModel, public EmbeddingRuntimeModel {
public:
    explicit FakeModel(std::shared_ptr<Probe> probe) : p(std::move(probe)) {}
    ~FakeModel() override { ++p->unloads; }
    RuntimeConversationPrompt prepareConversation(const ConversationRequest& request, const CancellationToken&) override {
        p->preparedConversations.append(request);
        TokenList tokens;
        for (const auto& message : request.messages)
            for (const auto ch : message.toObject()["content"].toString()) tokens.append(ch.unicode());
        tokens.append(3);
        return {tokens, {}, std::make_shared<FakePromptState>(p),p->nativeInputTokens};
    }
    RuntimeConversationReply parseConversation(const RuntimeConversationPrompt&, const QString& text) override {
        const auto reply = QJsonDocument::fromJson(text.toUtf8()).object();
        return {reply["text"].toString(), reply["reasoning"].toString(), reply["tool_calls"].toArray()};
    }
    TokenList tokenize(const QList<ChatMessage>& messages, const CancellationToken&) override
    {
        TokenList tokens;
        for (const auto& message : messages) {
            tokens.push_back(message.role == Role::System ? 1 : message.role == Role::User ? 2 : 3);
            for (const auto ch : message.content) tokens.push_back(ch.unicode());
        }
        tokens.push_back(3);
        return tokens;
    }
    std::unique_ptr<RuntimeContext> createContext(const CancellationToken&) override
    { return std::make_unique<FakeContext>(p); }
    QList<float> embedText(const QString& text, const CancellationToken& cancel) override {
        cancel.throwIfCancelled(); ++p->embeddingCalls;
        if (p->invalidEmbedding) return {std::numeric_limits<float>::quiet_NaN(), 1.0f};
        return {float(text.size()), 1.0f};
    }
private:
    std::shared_ptr<Probe> p;
};

class FakeRuntime final : public Runtime {
public:
    explicit FakeRuntime(std::shared_ptr<Probe> probe) : p(std::move(probe)) {}
    QString id() const override { return QStringLiteral("test"); }
    bool supportsModel(const ModelSpec& spec) const override { return spec.format == QStringLiteral("test"); }
    QList<RuntimeDevice> devices(const HardwareInfo&) const override { return {{ComputeBackend::Cpu, {}}}; }
    MemoryEstimate estimateMemory(const ModelSpec&, int) const override { return {p->memoryBytes, 0, 0, "test reservation"}; }
    std::shared_ptr<RuntimeModel> load(const ModelSpec&, const RuntimeDevice&, const CancellationToken&) override
    { ++p->loads; return std::make_shared<FakeModel>(p); }
private:
    std::shared_ptr<Probe> p;
};

class ServiceTests : public QObject {
private:
    std::unique_ptr<QTemporaryDir> storage;
    ServiceOptions options() const
    {
        ServiceOptions result;
        result.modelsDirectory = storage->filePath(QStringLiteral("Models"));
        result.keepAliveMs = 300000;
        return result;
    }
    static void installFixture(Service& service, const QString& id = QStringLiteral("small"), int context = 128,
                               const QStringList& capabilities = {"text-generation", "chat"})
    {
        QTemporaryDir package(QDir::temp().filePath(QStringLiteral("bundle-XXXXXX")));
        if (!package.isValid()) throw std::runtime_error("Cannot create test package");
        ModelManifest manifest{id, "test", "test", "none", context, capabilities, "model.test"};
        QFile metadata(package.filePath(QStringLiteral("manifest.json")));
        if (!metadata.open(QIODevice::WriteOnly) || metadata.write(QJsonDocument(manifestObject(manifest)).toJson()) < 1)
            throw std::runtime_error("Cannot write test manifest");
        metadata.close();
        QFile weights(package.filePath(QStringLiteral("model.test")));
        if (!weights.open(QIODevice::WriteOnly) || weights.write("fixture") != 7) throw std::runtime_error("Cannot write test weights");
        weights.close();
        (void)service.installModel(package.path()).get();
    }
    static void setup(Service& service, const std::shared_ptr<Probe>& p, int context = 128)
    {
        service.registerRuntime(std::make_shared<FakeRuntime>(p)).get();
        installFixture(service, QStringLiteral("small"), context);
        (void)service.loadModel({QStringLiteral("model://small"), context}).get();
    }
    static ChatRequest request(const QString& session, const QString& prompt = QStringLiteral("hello"))
    {
        ChatRequest r;
        r.sessionId = session;
        r.prompt = prompt;
        r.options.maxTokens = 8;
        return r;
    }
public:
    void init()
    {
        storage = std::make_unique<QTemporaryDir>(QDir::temp().filePath(QStringLiteral("store-XXXXXX")));
        CTEST_CHECK(storage->isValid());
    }
    void cleanup() { storage.reset(); }

    void embeddingUsesDeclaredCapabilityAndReleasesTheModel() {
        auto p = std::make_shared<Probe>(); Service service(options());
        service.registerRuntime(std::make_shared<FakeRuntime>(p)).get();
        installFixture(service, "embedding", 128, {"embedding"});
        const auto values = service.embedText("model://embedding", "creative work").get();
        CTEST_COMPARE(values.size(), 2); CTEST_COMPARE(values[0], 13.0f); CTEST_COMPARE(values[1], 1.0f);
        CTEST_COMPARE(p->embeddingCalls.load(), 1);
        CancellationToken cancelled; cancelled.cancel();
        CTEST_THROWS(Error, (void)service.embedText("model://embedding", "test", cancelled).get());
        CTEST_THROWS(Error, (void)service.embedText("model://embedding", " ").get());
        p->invalidEmbedding = true;
        CTEST_THROWS(Error, (void)service.embedText("model://embedding", "invalid").get());
        service.unloadModel("model://embedding").get();
        CTEST_COMPARE(p->unloads.load(), 1);
        installFixture(service, "chat-only", 128, {"chat"});
        CTEST_THROWS(Error, (void)service.embedText("model://chat-only", "test").get());
    }
    void embeddingReleasesCachedContextsWithoutDeletingSessions() {
        auto p = std::make_shared<Probe>(); Service service(options());
        service.registerRuntime(std::make_shared<FakeRuntime>(p)).get();
        installFixture(service, "both", 128, {"text-generation", "chat", "embedding"});
        (void)service.loadModel({"model://both", 128}).get();
        const auto session = service.createSession("model://both").get();
        CTEST_COMPARE(service.chat(request(session)).result.get().errorCode, ErrorCode::None);
        CTEST_COMPARE(service.stats().get().cachedContexts, 1);
        (void)service.embedText("model://both", "original artwork").get();
        const auto stats = service.stats().get();
        CTEST_COMPARE(stats.cachedContexts, 0); CTEST_COMPARE(stats.sessions, 1);
        CTEST_CHECK(!service.session(session).get().messages.isEmpty());
    }

    void conversationBudgetUsesTokenizerWithoutGeneration()
    {
        auto p = std::make_shared<Probe>(); Service service(options()); setup(service, p, 128);
        ConversationRequest r; r.model = "model://small"; r.options.maxTokens = 8;
        r.messages = {QJsonObject{{"role", "user"}, {"content", QString(200, QChar(0xac00))}}};
        const auto budget = service.measureConversation(r).get();
        CTEST_COMPARE(budget.inputTokens, 201); CTEST_COMPARE(budget.contextTokens, 128);
        CTEST_COMPARE(p->contexts.load(), 0); CTEST_CHECK(!p->started.load());
        CancellationToken cancelled; cancelled.cancel();
        try { (void)service.measureConversation(r, cancelled).get(); iillm_ctest::fail(__FILE__, __LINE__, "Cancelled measurement succeeded"); }
        catch (const Error& e) { CTEST_COMPARE(e.code(), ErrorCode::Cancelled); }
        r.messages.append(QJsonObject{{"role", "tool"}, {"tool_call_id", "orphan"}, {"content", "bad"}});
        CTEST_THROWS(Error, (void)service.measureConversation(r).get());
        service.unloadModel("model://small").get(); // Measurement released its residency lease.
        CTEST_COMPARE(p->unloads.load(), 1);
        r.messages.removeLast(); r.keepAliveMs = 0;
        (void)service.measureConversation(r).get();
        (void)service.stats().get(); // Wait until all preparation temporaries have been destroyed.
        CTEST_COMPARE(p->unloads.load(), 2);
        CTEST_COMPARE(p->preparedDestroyedAfterUnload.load(), 0);
    }
    void structuredConversationPreservesToolsAndModelScopedCache()
    {
        auto p = std::make_shared<Probe>(); Service service(options()); setup(service, p, 1024);
        const QJsonObject call{{"id", "call-1"}, {"type", "function"},
            {"function", QJsonObject{{"name", "Read"}, {"arguments", "{\"path\":\"a.txt\"}"}}}};
        p->answer = QString::fromUtf8(QJsonDocument(QJsonObject{{"tool_calls", QJsonArray{call}}}).toJson(QJsonDocument::Compact));
        ConversationRequest r; r.model = "model://small"; r.contextId = "agent-session"; r.options.maxTokens = 64;
        r.tools = {QJsonObject{{"type", "function"}, {"function", QJsonObject{{"name", "Read"},
            {"parameters", QJsonObject{{"type", "object"}}}}}}};
        r.messages = {QJsonObject{{"role", "user"}, {"content", "Read a.txt"}}};
        const auto first = service.converse(r).result.get();
        CTEST_COMPARE(first.errorCode, ErrorCode::None); CTEST_COMPARE(first.toolCalls, QJsonArray{call});
        CTEST_COMPARE(service.stats().get().sessions, 0); // The durable transcript belongs to the caller.
        r.messages.append(QJsonObject{{"role", "assistant"}, {"tool_calls", first.toolCalls}});
        r.messages.append(QJsonObject{{"role", "tool"}, {"tool_call_id", "call-1"}, {"content", "observed"}});
        p->answer = "{\"text\":\"observed\"}";
        QString streamed;
        const auto second = service.converse(r, [&](const StreamEvent& e) {
            if (e.kind == StreamEventKind::Delta) streamed += e.text;
        }).result.get();
        CTEST_COMPARE(second.errorCode, ErrorCode::None); CTEST_COMPARE(second.text, "observed"); CTEST_COMPARE(streamed, second.text);
        CTEST_CHECK(second.usage.cachedTokens > 0); CTEST_COMPARE(p->contexts.load(), 1);
        installFixture(service, "other", 1024); (void)service.loadModel({"model://other", 1024}).get();
        r.model = "model://other";
        const auto other = service.converse(r).result.get();
        CTEST_COMPARE(other.errorCode, ErrorCode::None); CTEST_COMPARE(other.usage.cachedTokens, 0); CTEST_COMPARE(p->contexts.load(), 2);
        r.messages = {QJsonObject{{"role", "user"}, {"content", "Read a.txt"}}};
        p->answer = QString::fromUtf8(QJsonDocument(QJsonObject{{"text", "visible"}, {"tool_calls", QJsonArray{call}}}).toJson());
        const auto failed = service.converse(r, [](const StreamEvent& event) {
            if (event.kind == StreamEventKind::Delta) throw std::runtime_error("consumer failed");
        }).result.get();
        CTEST_COMPARE(failed.errorCode, ErrorCode::ConsumerFailure); CTEST_CHECK(failed.toolCalls.isEmpty());
    }
    void structuredConversationRejectsBrokenToolProtocol()
    {
        auto p = std::make_shared<Probe>(); Service service(options()); setup(service, p);
        ConversationRequest r; r.model = "model://small"; r.options.maxTokens = 8;
        r.messages = {QJsonObject{{"role", "tool"}, {"tool_call_id", "orphan"}, {"content", "data"}}};
        CTEST_COMPARE(service.converse(r).result.get().errorCode, ErrorCode::InvalidArgument);
        r.messages = {QJsonObject{{"role", "user"}, {"content", "hello"}}}; r.toolChoice = "invalid";
        CTEST_COMPARE(service.converse(r).result.get().errorCode, ErrorCode::InvalidArgument);
        r.toolChoice = "required";
        CTEST_COMPARE(service.converse(r).result.get().errorCode, ErrorCode::InvalidArgument);
        r.toolChoice = "auto";
        r.messages.append(QJsonObject{{"role", "assistant"}, {"tool_calls", QJsonArray{
            QJsonObject{{"id", "pending"}, {"type", "function"}, {"function", QJsonObject{{"name", "Read"}, {"arguments", "{}"}}}}
        }}});
        r.messages.append(QJsonObject{{"role", "user"}, {"content", "skip the result"}});
        CTEST_COMPARE(service.converse(r).result.get().errorCode, ErrorCode::InvalidArgument);
        CTEST_COMPARE(p->contexts.load(), 0);
    }




    void bundledStarterAliasSupportsConversation()
    {
        Service service(options());
        auto probe = std::make_shared<Probe>();
        service.registerRuntime(std::make_shared<FakeRuntime>(probe)).get();
        installFixture(service, "qwen2.5-0.5b-instruct-q4");
        const auto model = service.resolveModel("qwen2.5:0.5b").get();
        CTEST_COMPARE(model.uri, QStringLiteral("model://qwen2.5-0.5b-instruct-q4"));
        const auto session = service.createSession("qwen2.5:0.5b", "Be concise.").get();
        CTEST_COMPARE(service.chat(request(session)).result.get().errorCode, ErrorCode::None);
        const auto next = service.chat(request(session, "Again.")).result.get();
        CTEST_COMPARE(next.errorCode, ErrorCode::None);
        CTEST_CHECK(next.usage.cachedTokens > 0);
        service.resetSession(session).get();
        const auto history = service.session(session).get().messages;
        CTEST_COMPARE(history.size(), 1);
        CTEST_COMPARE(history.first().role, Role::System);
        CTEST_COMPARE(history.first().content, QStringLiteral("Be concise."));
        CTEST_COMPARE(service.chat(request(session)).result.get().usage.cachedTokens, 0);
        service.closeSession(session).get();
    }
    void residencyReuseLruAndSessionRestoration()
    {
        auto p = std::make_shared<Probe>();
        auto o = options(); o.memoryBudgetBytes = 2 * p->memoryBytes;
        Service service(o); setup(service, p);
        installFixture(service, "b"); installFixture(service, "c");
        (void)service.loadModel({"model://small"}).get(); CTEST_COMPARE(p->loads.load(), 1);
        const auto a = service.createSession("model://small").get();
        const auto b = service.createSession("model://b").get();
        const auto c = service.createSession("model://c").get();
        CTEST_COMPARE(service.chat(request(a)).result.get().errorCode, ErrorCode::None);
        CTEST_COMPARE(service.chat(request(b)).result.get().errorCode, ErrorCode::None);
        CTEST_COMPARE(p->loads.load(), 2); CTEST_COMPARE(service.models().get().size(), 2);
        CTEST_COMPARE(service.chat(request(a)).result.get().errorCode, ErrorCode::None); // Touch a; b is oldest.
        CTEST_COMPARE(service.chat(request(c)).result.get().errorCode, ErrorCode::None);
        CTEST_CHECK(!service.resolveModel("model://b").get().loaded);
        CTEST_COMPARE(service.session(b).get().messages.size(), 2); // LRU discarded KV, not conversation.
        const auto restored = service.chat(request(b, "again")).result.get();
        CTEST_COMPARE(restored.errorCode, ErrorCode::None); CTEST_COMPARE(restored.usage.cachedTokens, 0);
        CTEST_COMPARE(service.session(b).get().messages.size(), 4);
        const auto stats = service.stats().get();
        CTEST_COMPARE(stats.loadedModels, 2); CTEST_COMPARE(stats.residentBytes, 2 * p->memoryBytes);
        CTEST_COMPARE(stats.modelLoads, quint64(4)); CTEST_COMPARE(stats.modelEvictions, quint64(2));
        CTEST_CHECK(stats.availableRamKnown); CTEST_CHECK(stats.availableRamBytes > 0);
    }
    void hostPinnedModelSurvivesIdleAndMemoryPressure()
    {
        auto p = std::make_shared<Probe>();
        auto o = options(); o.memoryBudgetBytes = p->memoryBytes; o.keepAliveMs = 0;
        Service service(o); service.registerRuntime(std::make_shared<FakeRuntime>(p)).get();
        installFixture(service, "resident"); installFixture(service, "other");
        CancellationToken cancelled; cancelled.cancel();
        CTEST_THROWS(Error, (void)service.pinModel({"model://resident", 128}, cancelled).get());
        CTEST_COMPARE(p->loads.load(), 0);
        const auto pinned = service.pinModel({"model://resident", 128}).get();
        CTEST_CHECK(service.isModelPinned("model://resident").get()); CTEST_COMPARE(pinned.expiresInMs, qint64(-1));
        CTEST_COMPARE(p->loads.load(), 1);
        CTEST_COMPARE(service.pinModel({"model://resident", 128}).get().expiresInMs, qint64(-1));
        CTEST_COMPARE(p->loads.load(), 1);
        iillm_ctest::processFor(200);
        CTEST_COMPARE(service.models().get().size(), 1);
        CTEST_THROWS(Error, (void)service.loadModel({"model://other", 128}).get());
        CTEST_THROWS(Error, service.unloadModel("model://resident").get());
        CTEST_COMPARE(p->unloads.load(), 0);
        service.unpinModel("model://resident").get();
        CTEST_WAIT_COMPARE_TIMEOUT(p->unloads.load(), 1, 2000);
        (void)service.loadModel({"model://other", 128}).get();
        CTEST_COMPARE(p->loads.load(), 2);
    }
    void expiryRunsWithoutRequestsAndZeroLifetimeReleasesAfterCancellation()
    {
        auto p = std::make_shared<Probe>();
        Service service(options()); setup(service, p);
        const auto session = service.createSession("model://small").get();
        auto r = request(session); r.keepAliveMs = 30;
        CTEST_COMPARE(service.chat(r).result.get().errorCode, ErrorCode::None);
        CTEST_WAIT_COMPARE_TIMEOUT(p->unloads.load(), 1, 2000); // No service API calls drive this eviction.
        CTEST_COMPARE(service.stats().get().cachedContexts, 0);
        CTEST_COMPARE(service.session(session).get().messages.size(), 2);
        p->release = false; p->started = false; r.keepAliveMs = 0;
        const auto active = service.chat(r);
        CTEST_WAIT_DEFAULT(p->started.load());
        auto snapshot = service.models();
        CTEST_COMPARE(snapshot.wait_for(50ms), std::future_status::ready);
        const auto resident = snapshot.get();
        CTEST_COMPARE(resident.size(), 1); CTEST_COMPARE(resident.first().activeRequests, 1);
        CTEST_COMPARE(resident.first().expiresInMs, qint64(-1));
        iillm_ctest::processFor(150); CTEST_COMPARE(p->unloads.load(), 1); // Active lease survives a zero TTL.
        active.cancel(); CTEST_COMPARE(active.result.get().finishReason, FinishReason::Cancelled);
        CTEST_COMPARE(p->unloads.load(), 2); CTEST_COMPARE(service.stats().get().loadedModels, 0);
        CTEST_COMPARE(service.session(session).get().messages.size(), 2);
        p->release = true;
        CTEST_COMPARE(service.chat(r).result.get().errorCode, ErrorCode::None);
        CTEST_COMPARE(service.stats().get().loadedModels, 0);
    }
    void admissionRejectsBeforeRuntimeAllocation()
    {
        auto p = std::make_shared<Probe>(); auto o = options(); o.memoryBudgetBytes = p->memoryBytes - 1;
        Service service(o); service.registerRuntime(std::make_shared<FakeRuntime>(p)).get(); installFixture(service);
        const auto session = service.createSession("model://small").get();
        CTEST_COMPARE(service.chat(request(session)).result.get().errorCode, ErrorCode::ResourceLimit);
        CTEST_COMPARE(p->loads.load(), 0); CTEST_COMPARE(service.session(session).get().messages.size(), 0);
        CTEST_COMPARE(service.stats().get().residentBytes, quint64(0));
    }
    void managedLifecycleAndIntegrityGate()
    {
        auto p = std::make_shared<Probe>();
        const auto opts = options();
        Service service(opts);
        service.registerRuntime(std::make_shared<FakeRuntime>(p)).get();
        installFixture(service);
        CTEST_COMPARE(p->loads.load(), 0); // Install/list/resolve/verify do not load the engine.
        CTEST_COMPARE(service.installedModels().get().models.size(), 1);
        CTEST_CHECK(!service.resolveModel(QStringLiteral("model://small")).get().loaded);
        CTEST_CHECK(service.verifyModel(QStringLiteral("model://small")).get().valid);
        CTEST_THROWS(Error, (void)service.loadModel({"/model.test"}).get());
        CTEST_THROWS(Error, (void)service.loadModel({"model://small", 129}).get());
        auto loaded = service.loadModel({"model://small"}).get();
        CTEST_COMPARE(loaded.contextTokens, 128);
        CTEST_CHECK(service.installedModels().get().models.first().loaded);
        CTEST_THROWS(Error, service.removeModel(QStringLiteral("model://small")).get());
        const auto session = service.createSession(QStringLiteral("model://small")).get();
        CTEST_THROWS(Error, service.unloadModel(QStringLiteral("model://small")).get());
        service.closeSession(session).get();
        service.unloadModel(QStringLiteral("model://small")).get();
        QFile file(QDir(opts.modelsDirectory).filePath(QStringLiteral("small/model.test")));
        CTEST_CHECK(file.open(QIODevice::WriteOnly)); CTEST_COMPARE(file.write("changed"), qint64(7)); file.close();
        CTEST_CHECK(!service.verifyModel(QStringLiteral("model://small")).get().valid);
        CTEST_THROWS(Error, (void)service.loadModel({"model://small"}).get());
        CTEST_COMPARE(p->loads.load(), 1); // Corrupt installed bytes never reach a runtime.
        service.removeModel(QStringLiteral("model://small")).get();
        CTEST_CHECK(service.installedModels().get().models.isEmpty());
    }
    void manifestCapabilitiesGateChat()
    {
        auto p = std::make_shared<Probe>();
        Service service(options());
        service.registerRuntime(std::make_shared<FakeRuntime>(p)).get();
        installFixture(service, QStringLiteral("text-only"), 128, {"text-generation"});
        (void)service.loadModel({"model://text-only"}).get();
        CTEST_THROWS(Error, (void)service.createSession(QStringLiteral("model://text-only")).get());
        CTEST_COMPARE(service.stats().get().sessions, 0);
    }
    void ipcOwnsExecutionChoice()
    {
        auto p = std::make_shared<Probe>();
        Service service(options());
        setup(service, p);
        LocalIpcServer server(service);
        const auto name = QDir::temp().filePath(QStringLiteral("policy-") + QUuid::createUuid().toString(QUuid::Id128).first(8));
        CTEST_CHECK(server.listen(name));
        QLocalSocket socket;
        QList<QJsonObject> messages;
        QByteArray input;
        connect(&socket, &QLocalSocket::readyRead, this, [&] {
            input += socket.readAll();
            while (input.contains('\n')) {
                const auto end = input.indexOf('\n');
                messages.append(QJsonDocument::fromJson(input.first(end)).object());
                input.remove(0, end + 1);
            }
        });
        socket.connectToServer(name);
        CTEST_WAIT_COMPARE(socket.state(), QLocalSocket::ConnectedState);
        auto send = [&](const QString& method, const QJsonObject& params) {
            socket.write(QJsonDocument(QJsonObject{{"id", "r"}, {"method", method}, {"params", params}}).toJson(QJsonDocument::Compact) + '\n');
        };
        send(QStringLiteral("hardware.get"), {});
        CTEST_WAIT_COMPARE(messages.size(), 1);
        CTEST_COMPARE(messages.takeFirst()["result"].toObject(), hardwareObject(service.hardware()));
        for (const auto& field : {"runtime", "backend", "device", "device_id", "gpu_layers"}) {
            send(QStringLiteral("models.load"), {{"model", "model://auto"}, {QString::fromLatin1(field), "cpu"}});
            CTEST_WAIT_COMPARE(messages.size(), 1);
            CTEST_COMPARE(messages.takeFirst()["error"].toObject()["code"].toString(), QStringLiteral("invalid_argument"));
        }
        send(QStringLiteral("models.load"), {{"model", "model://auto"}, {"options", QJsonObject{{"backend", "cuda"}}}});
        CTEST_WAIT_COMPARE(messages.size(), 1);
        CTEST_COMPARE(messages.takeFirst()["error"].toObject()["code"].toString(), QStringLiteral("invalid_argument"));
        CTEST_COMPARE(p->loads.load(), 1);
        installFixture(service, QStringLiteral("auto"));
        send(QStringLiteral("models.load"), {{"model", "model://auto"}});
        CTEST_WAIT_COMPARE(messages.size(), 1);
        const auto loaded = messages.takeFirst()["result"].toObject();
        CTEST_COMPARE(loaded["model"].toString(), QStringLiteral("model://auto"));
        CTEST_COMPARE(loaded["execution"].toObject()["runtime"].toString(), QStringLiteral("test"));
        CTEST_COMPARE(loaded["execution"].toObject()["backend"].toString(), QStringLiteral("cpu"));
        CTEST_COMPARE(p->loads.load(), 2);
    }
    void multiTurnAndStreaming()
    {
        auto p = std::make_shared<Probe>();
        Service service(options());
        setup(service, p);
        const auto id = service.createSession(QStringLiteral("model://small"), QStringLiteral("system")).get();
        QList<StreamEvent> events;
        auto h = service.chat(request(id), [&](const auto& e) { events.push_back(e); });
        const auto result = h.result.get();
        CTEST_COMPARE(result.text, p->answer);
        CTEST_COMPARE(result.finishReason, FinishReason::Stop);
        CTEST_COMPARE(events.front().kind, StreamEventKind::Started);
        CTEST_COMPARE(events.back().kind, StreamEventKind::Finished);
        CTEST_COMPARE(service.session(id).get().messages.size(), 3);
        const auto second = service.chat(request(id, QStringLiteral("again"))).result.get();
        CTEST_CHECK(second.usage.cachedTokens > 0);
        CTEST_COMPARE(p->contexts.load(), 1);
        CTEST_COMPARE(service.session(id).get().messages.size(), 5);
    }
    void cancellationRollsBackAndInvalidatesCache()
    {
        auto p = std::make_shared<Probe>();
        Service service(options());
        setup(service, p);
        const auto id = service.createSession(QStringLiteral("model://small")).get();
        auto h = service.chat(request(id), [&](const auto& e) {
            if (e.kind == StreamEventKind::Delta) throw std::runtime_error("consumer failed");
        });
        CTEST_COMPARE(h.result.get().finishReason, FinishReason::Error);
        CTEST_CHECK(service.session(id).get().messages.isEmpty());
        CTEST_COMPARE(service.stats().get().cachedContexts, 0);
        p->started = false;
        p->release = false;
        auto cancelled = service.chat(request(id));
        CTEST_WAIT_DEFAULT(p->started.load());
        cancelled.cancel();
        CTEST_COMPARE(cancelled.result.get().finishReason, FinishReason::Cancelled);
        CTEST_CHECK(service.session(id).get().messages.isEmpty());
    }
    void fifoAndQueueLimit()
    {
        auto p = std::make_shared<Probe>();
        auto opts = options();
        opts.maxQueuedRequests = 1;
        Service service(opts);
        setup(service, p);
        const auto id = service.createSession(QStringLiteral("model://small")).get();
        p->release = false;
        auto first = service.chat(request(id));
        CTEST_WAIT_DEFAULT(p->started.load());
        auto second = service.chat(request(id, QStringLiteral("second")));
        auto third = service.chat(request(id, QStringLiteral("third")));
        CTEST_COMPARE(third.result.get().errorCode, ErrorCode::QueueFull);
        second.cancel();
        p->release = true;
        CTEST_COMPARE(first.result.get().finishReason, FinishReason::Stop);
        CTEST_COMPARE(second.result.get().finishReason, FinishReason::Cancelled);
        CTEST_COMPARE(p->peak.load(), 1);
        CTEST_COMPARE(service.session(id).get().messages.size(), 2);
    }
    void lruAndModelLifecycle()
    {
        auto p = std::make_shared<Probe>();
        auto opts = options();
        opts.maxCachedContexts = 1;
        Service service(opts);
        setup(service, p);
        const auto a = service.createSession(QStringLiteral("model://small")).get();
        const auto b = service.createSession(QStringLiteral("model://small")).get();
        (void)service.chat(request(a)).result.get();
        (void)service.chat(request(b)).result.get();
        CTEST_COMPARE(p->destroyed.load(), 1);
        CTEST_COMPARE(service.stats().get().cacheEvictions, quint64(1));
        CTEST_THROWS(Error, service.unloadModel(QStringLiteral("model://small")).get());
        service.resetSession(b).get();
        CTEST_COMPARE(service.stats().get().cachedContexts, 0);
        service.closeSession(a).get();
        service.closeSession(b).get();
        service.unloadModel(QStringLiteral("model://small")).get();
        CTEST_CHECK(service.models().get().isEmpty());
    }
    void contextTrimsCompleteTurnsAndRejectsOversize()
    {
        auto p = std::make_shared<Probe>();
        Service service(options());
        setup(service, p, 24);
        const auto id = service.createSession(QStringLiteral("model://small"), QStringLiteral("S")).get();
        CTEST_COMPARE(service.chat(request(id)).result.get().finishReason, FinishReason::Stop);
        auto result = service.chat(request(id, QStringLiteral("world"))).result.get();
        CTEST_COMPARE(result.usage.droppedMessages, 2);
        const auto before = service.session(id).get().messages;
        auto oversized = service.chat(request(id, QString(40, 'x'))).result.get();
        CTEST_COMPARE(oversized.errorCode, ErrorCode::ContextOverflow);
        CTEST_COMPARE(service.session(id).get().messages, before);
    }
    void failuresAndInvalidRequests()
    {
        auto p = std::make_shared<Probe>();
        Service service(options());
        setup(service, p);
        const auto id = service.createSession(QStringLiteral("model://small")).get();
        auto invalid = request(id);
        invalid.options.maxTokens = 0;
        CTEST_COMPARE(service.chat(invalid).result.get().errorCode, ErrorCode::InvalidArgument);
        p->fail = true;
        CTEST_COMPARE(service.chat(request(id)).result.get().errorCode, ErrorCode::RuntimeFailure);
        p->fail = false;
        CTEST_COMPARE(service.chat(request(id)).result.get().finishReason, FinishReason::Stop);
        CTEST_COMPARE(service.chat(request(QStringLiteral("missing"))).result.get().errorCode, ErrorCode::NotFound);
    }
    void stopAcrossChunks()
    {
        auto p = std::make_shared<Probe>();
        p->answer = QStringLiteral("abcSTOPtail");
        Service service(options());
        setup(service, p);
        const auto id = service.createSession(QStringLiteral("model://small")).get();
        auto r = request(id);
        r.options.stop = {QStringLiteral("STOP")};
        QString streamed;
        const auto result = service.chat(r, [&](const auto& e) {
            if (e.kind == StreamEventKind::Delta) streamed += e.text;
        }).result.get();
        CTEST_COMPARE(result.text, QStringLiteral("abc"));
        CTEST_COMPARE(streamed, result.text);
    }
    void shutdownSettlesActiveAndQueuedRequests()
    {
        auto p = std::make_shared<Probe>();
        auto service = std::make_unique<Service>(options());
        setup(*service, p);
        const auto id = service->createSession(QStringLiteral("model://small")).get();
        p->release = false;
        auto active = service->chat(request(id));
        CTEST_WAIT_DEFAULT(p->started.load());
        auto queued = service->chat(request(id));
        service.reset();
        CTEST_COMPARE(active.result.get().finishReason, FinishReason::Cancelled);
        CTEST_COMPARE(queued.result.get().finishReason, FinishReason::Cancelled);
        CTEST_COMPARE(p->contexts.load(), p->destroyed.load());
    }
    void queuedTurnsObserveCommittedHistory()
    {
        auto p = std::make_shared<Probe>();
        Service service(options());
        setup(service, p);
        const auto id = service.createSession(QStringLiteral("model://small")).get();
        p->release = false;
        auto first = service.chat(request(id));
        CTEST_WAIT_DEFAULT(p->started.load());
        auto second = service.chat(request(id, QStringLiteral("again")));
        p->release = true;
        const auto firstResult = first.result.get();
        const auto secondResult = second.result.get();
        CTEST_COMPARE(firstResult.finishReason, FinishReason::Stop);
        CTEST_CHECK(secondResult.usage.promptTokens > firstResult.usage.promptTokens);
        CTEST_COMPARE(service.session(id).get().messages.size(), 4);
    }
    void cacheTokenBudgetAndRegistrationErrors()
    {
        auto p = std::make_shared<Probe>();
        auto opts = options();
        opts.maxCachedContexts = 3;
        opts.maxCachedContextTokens = 128;
        Service service(opts);
        setup(service, p);
        CTEST_THROWS(Error, service.registerRuntime(std::make_shared<FakeRuntime>(p)).get());
        installFixture(service, QStringLiteral("big"), 129);
        CTEST_THROWS(Error, (void)service.loadModel({QStringLiteral("model://big"), 129}).get());
        const auto a = service.createSession(QStringLiteral("model://small")).get();
        const auto b = service.createSession(QStringLiteral("model://small")).get();
        (void)service.chat(request(a)).result.get();
        (void)service.chat(request(b)).result.get();
        const auto stats = service.stats().get();
        CTEST_COMPARE(stats.reservedContextTokens, 128);
        CTEST_COMPARE(stats.cachedContexts, 1);
        CTEST_COMPARE(stats.cacheEvictions, quint64(1));
    }
    void ipcRoundTripAndFraming()
    {
        auto p = std::make_shared<Probe>();
        Service service(options());
        setup(service, p);
        LocalIpcServer server(service);
        const auto name = QDir::temp().filePath(QStringLiteral("ipc-") + QUuid::createUuid().toString(QUuid::Id128).first(8));
        CTEST_CHECK_MESSAGE(server.listen(name), qPrintable(server.errorString()));
        LocalIpcServer duplicate(service);
        CTEST_CHECK(!duplicate.listen(name)); // Never unlink a live endpoint.
        QLocalSocket socket;
        QList<QJsonObject> messages;
        QByteArray bytes;
        connect(&socket, &QLocalSocket::readyRead, this, [&] {
            bytes += socket.readAll();
            while (bytes.contains('\n')) {
                const auto pos = bytes.indexOf('\n');
                messages.append(QJsonDocument::fromJson(bytes.first(pos)).object());
                bytes.remove(0, pos + 1);
            }
        });
        socket.connectToServer(name);
        CTEST_WAIT_COMPARE(socket.state(), QLocalSocket::ConnectedState);
        socket.write("not-json\n{\"id\":\"c\",\"method\":\"sessions.");
        socket.flush();
        CTEST_WAIT_COMPARE(messages.size(), 1);
        CTEST_COMPARE(messages[0][QStringLiteral("error")].toObject()[QStringLiteral("code")].toString(), QStringLiteral("protocol_error"));
        socket.write("create\",\"params\":{\"model\":\"model://small\"}}\n");
        CTEST_WAIT_COMPARE(messages.size(), 2);
        const auto session = messages.back()[QStringLiteral("result")].toObject()[QStringLiteral("session_id")].toString();
        CTEST_CHECK(!session.isEmpty());
        const QJsonObject chat{{QStringLiteral("id"), QStringLiteral("g")}, {QStringLiteral("method"), QStringLiteral("chat")},
            {QStringLiteral("params"), QJsonObject{{QStringLiteral("session_id"), session}, {QStringLiteral("prompt"), QStringLiteral("hello")},
                {QStringLiteral("options"), QJsonObject{{QStringLiteral("max_tokens"), 8}}}}}};
        socket.write(QJsonDocument(chat).toJson(QJsonDocument::Compact) + '\n');
        CTEST_WAIT_COMPARE(messages.back()[QStringLiteral("event")].toString(), QStringLiteral("done"));
        CTEST_COMPARE(messages[2][QStringLiteral("event")].toString(), QStringLiteral("accepted"));
        CTEST_COMPARE(messages[3][QStringLiteral("event")].toString(), QStringLiteral("started"));
        QString text;
        for (const auto& m : messages) if (m[QStringLiteral("event")] == QStringLiteral("delta")) text += m[QStringLiteral("text")].toString();
        CTEST_COMPARE(text, p->answer);
        CTEST_COMPARE(messages.back()[QStringLiteral("result")].toObject()[QStringLiteral("text")].toString(), text);
        socket.disconnectFromServer();
        server.close();
    }
    void utf8BoundariesAndTruncatedFinalToken()
    {
        detail::Utf8Stream decoder;
        const auto bytes = QString::fromUtf8("가😀").toUtf8();
        QString output;
        for (const auto byte : bytes) output += QString(decoder(QByteArray(1, byte)));
        output += decoder.finish();
        CTEST_COMPARE(output, QString::fromUtf8("가😀"));
        CTEST_COMPARE(QString(decoder(QByteArray::fromHex("eab0"))), QString{});
        CTEST_COMPARE(decoder.finish(), QString(QChar::ReplacementCharacter));
    }
    void ipcCancellationBypassesFullConnectionLimit()
    {
        auto p = std::make_shared<Probe>();
        Service service(options());
        setup(service, p);
        const auto session = service.createSession(QStringLiteral("model://small")).get();
        IpcOptions options;
        options.maxInFlightPerConnection = 1;
        LocalIpcServer server(service, options);
        const auto name = QDir::temp().filePath(QStringLiteral("ipc-") + QUuid::createUuid().toString(QUuid::Id128).first(8));
        CTEST_CHECK(server.listen(name));
        QLocalSocket socket;
        QByteArray bytes;
        QList<QJsonObject> messages;
        connect(&socket, &QLocalSocket::readyRead, this, [&] {
            bytes += socket.readAll();
            while (bytes.contains('\n')) {
                const auto pos = bytes.indexOf('\n');
                messages.append(QJsonDocument::fromJson(bytes.first(pos)).object());
                bytes.remove(0, pos + 1);
            }
        });
        socket.connectToServer(name);
        CTEST_WAIT_COMPARE(socket.state(), QLocalSocket::ConnectedState);
        p->release = false;
        auto send = [&](const QJsonObject& request) { socket.write(QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n'); };
        send({{QStringLiteral("id"), QStringLiteral("g")}, {QStringLiteral("method"), QStringLiteral("chat")},
              {QStringLiteral("params"), QJsonObject{{QStringLiteral("session_id"), session}, {QStringLiteral("prompt"), QStringLiteral("hello")},
                  {QStringLiteral("options"), QJsonObject{{QStringLiteral("max_tokens"), 8}}}}}});
        CTEST_WAIT_DEFAULT(p->started.load());
        CTEST_WAIT_DEFAULT(!messages.isEmpty());
        const auto requestId = messages.front()[QStringLiteral("request_id")].toString();
        CTEST_CHECK(!requestId.isEmpty());
        send({{QStringLiteral("id"), QStringLiteral("x")}, {QStringLiteral("method"), QStringLiteral("cancel")},
              {QStringLiteral("params"), QJsonObject{{QStringLiteral("request_id"), requestId}}}});
        CTEST_WAIT_COMPARE(p->active.load(), 0);
        CTEST_WAIT_DEFAULT(std::any_of(messages.begin(), messages.end(), [](const auto& m) { return m[QStringLiteral("event")] == QStringLiteral("done"); }));
        CTEST_CHECK(service.session(session).get().messages.isEmpty());
        const auto done = *std::find_if(messages.begin(), messages.end(), [](const auto& m) { return m[QStringLiteral("event")] == QStringLiteral("done"); });
        CTEST_COMPARE(done[QStringLiteral("result")].toObject()[QStringLiteral("finish_reason")].toString(), QStringLiteral("cancelled"));
        server.close();
    }
    void ipcDisconnectCancelsAndOutputLimitRollsBack()
    {
        auto p = std::make_shared<Probe>();
        Service service(options());
        setup(service, p);
        const auto session = service.createSession(QStringLiteral("model://small")).get();
        IpcOptions options;
        options.maxBufferedOutputBytes = 256;
        LocalIpcServer server(service, options);
        const auto name = QDir::temp().filePath(QStringLiteral("ipc-") + QUuid::createUuid().toString(QUuid::Id128).first(8));
        CTEST_CHECK(server.listen(name));
        const auto chat = QJsonDocument(QJsonObject{{QStringLiteral("id"), QStringLiteral("g")}, {QStringLiteral("method"), QStringLiteral("chat")},
            {QStringLiteral("params"), QJsonObject{{QStringLiteral("session_id"), session}, {QStringLiteral("prompt"), QStringLiteral("hello")},
                {QStringLiteral("options"), QJsonObject{{QStringLiteral("max_tokens"), 8}}}}}}).toJson(QJsonDocument::Compact) + '\n';
        QLocalSocket socket;
        socket.connectToServer(name);
        CTEST_WAIT_COMPARE(socket.state(), QLocalSocket::ConnectedState);
        p->release = false;
        socket.write(chat);
        CTEST_WAIT_DEFAULT(p->started.load());
        socket.abort();
        CTEST_WAIT_COMPARE(p->active.load(), 0);
        CTEST_CHECK(service.session(session).get().messages.isEmpty());
        p->release = true;
        p->started = false;
        // An undrained worker inbox must remain bounded and must not commit a partial turn.
        p->answer = QString(1000, 'x');
        QLocalSocket slow;
        slow.connectToServer(name);
        CTEST_WAIT_COMPARE(slow.state(), QLocalSocket::ConnectedState);
        slow.write(chat);
        CTEST_WAIT_DEFAULT(p->started.load());
        CTEST_WAIT_COMPARE(slow.state(), QLocalSocket::UnconnectedState);
        CTEST_CHECK(service.session(session).get().messages.isEmpty());
        CTEST_COMPARE(service.stats().get().cachedContexts, 0);
        server.close();
    }
};

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    if (argc > 2) {
        std::cerr << "Expected a case name or --offline" << std::endl;
        return 1;
    }
    const std::string_view selection = argc == 2 ? argv[1] : "";
    const bool offline = selection == "--offline";
    iillm_ctest::CTestRunner runner(offline ? std::string_view{} : selection);
    ServiceTests tests;
    auto runCase = [&](std::string_view name, auto method) {
        runner.run(name, [&] {
            const auto cleanup = qScopeGuard([&] { tests.cleanup(); });
            tests.init();
            (tests.*method)();
        });
    };
    if (!offline)
        runCase("embeddingUsesDeclaredCapabilityAndReleasesTheModel", &ServiceTests::embeddingUsesDeclaredCapabilityAndReleasesTheModel);
    if (!offline)
        runCase("embeddingReleasesCachedContextsWithoutDeletingSessions", &ServiceTests::embeddingReleasesCachedContextsWithoutDeletingSessions);
    if (!offline)
        runCase("conversationBudgetUsesTokenizerWithoutGeneration", &ServiceTests::conversationBudgetUsesTokenizerWithoutGeneration);
    if (!offline)
        runCase("structuredConversationPreservesToolsAndModelScopedCache", &ServiceTests::structuredConversationPreservesToolsAndModelScopedCache);
    if (!offline)
        runCase("structuredConversationRejectsBrokenToolProtocol", &ServiceTests::structuredConversationRejectsBrokenToolProtocol);
    if (!offline)
        runCase("bundledStarterAliasSupportsConversation", &ServiceTests::bundledStarterAliasSupportsConversation);
    if (!offline)
        runCase("residencyReuseLruAndSessionRestoration", &ServiceTests::residencyReuseLruAndSessionRestoration);
    if (!offline)
        runCase("hostPinnedModelSurvivesIdleAndMemoryPressure", &ServiceTests::hostPinnedModelSurvivesIdleAndMemoryPressure);
    if (!offline)
        runCase("expiryRunsWithoutRequestsAndZeroLifetimeReleasesAfterCancellation", &ServiceTests::expiryRunsWithoutRequestsAndZeroLifetimeReleasesAfterCancellation);
    if (!offline)
        runCase("admissionRejectsBeforeRuntimeAllocation", &ServiceTests::admissionRejectsBeforeRuntimeAllocation);
    if (!offline)
        runCase("managedLifecycleAndIntegrityGate", &ServiceTests::managedLifecycleAndIntegrityGate);
    if (!offline)
        runCase("manifestCapabilitiesGateChat", &ServiceTests::manifestCapabilitiesGateChat);
    if (!offline)
        runCase("ipcOwnsExecutionChoice", &ServiceTests::ipcOwnsExecutionChoice);
    if (!offline)
        runCase("multiTurnAndStreaming", &ServiceTests::multiTurnAndStreaming);
    if (!offline)
        runCase("cancellationRollsBackAndInvalidatesCache", &ServiceTests::cancellationRollsBackAndInvalidatesCache);
    if (!offline)
        runCase("fifoAndQueueLimit", &ServiceTests::fifoAndQueueLimit);
    if (!offline)
        runCase("lruAndModelLifecycle", &ServiceTests::lruAndModelLifecycle);
    if (!offline)
        runCase("contextTrimsCompleteTurnsAndRejectsOversize", &ServiceTests::contextTrimsCompleteTurnsAndRejectsOversize);
    if (!offline)
        runCase("failuresAndInvalidRequests", &ServiceTests::failuresAndInvalidRequests);
    if (!offline)
        runCase("stopAcrossChunks", &ServiceTests::stopAcrossChunks);
    if (!offline)
        runCase("shutdownSettlesActiveAndQueuedRequests", &ServiceTests::shutdownSettlesActiveAndQueuedRequests);
    if (!offline)
        runCase("queuedTurnsObserveCommittedHistory", &ServiceTests::queuedTurnsObserveCommittedHistory);
    if (!offline)
        runCase("cacheTokenBudgetAndRegistrationErrors", &ServiceTests::cacheTokenBudgetAndRegistrationErrors);
    if (!offline)
        runCase("ipcRoundTripAndFraming", &ServiceTests::ipcRoundTripAndFraming);
    runCase("utf8BoundariesAndTruncatedFinalToken", &ServiceTests::utf8BoundariesAndTruncatedFinalToken);
    if (!offline)
        runCase("ipcCancellationBypassesFullConnectionLimit", &ServiceTests::ipcCancellationBypassesFullConnectionLimit);
    if (!offline)
        runCase("ipcDisconnectCancelsAndOutputLimitRollsBack", &ServiceTests::ipcDisconnectCancelsAndOutputLimitRollsBack);
    return runner.result();
}
