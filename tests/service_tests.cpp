#include <iiLocalLLM.h>
#include <agent/Engine.h>
#include "../runtimes/Utf8Stream.h"
#include <QtTest/QtTest>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonArray>
#include <QtCore/QUuid>
#include <QtCore/QTemporaryDir>
#include <QtNetwork/QLocalSocket>
#include <atomic>
#include <chrono>
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

class FakeModel final : public RuntimeModel {
public:
    explicit FakeModel(std::shared_ptr<Probe> probe) : p(std::move(probe)) {}
    ~FakeModel() override { ++p->unloads; }
    RuntimeConversationPrompt prepareConversation(const ConversationRequest& request, const CancellationToken&) override {
        p->preparedConversations.append(request);
        TokenList tokens;
        for (const auto& message : request.messages)
            for (const auto ch : message.toObject()["content"].toString()) tokens.append(ch.unicode());
        tokens.append(3);
        return {tokens, {}, std::make_shared<FakePromptState>(p)};
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
    Q_OBJECT
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
        QTemporaryDir package(QDir::current().filePath(QStringLiteral("bundle-XXXXXX")));
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
private slots:
    void init()
    {
        storage = std::make_unique<QTemporaryDir>(QDir::current().filePath(QStringLiteral("store-XXXXXX")));
        QVERIFY(storage->isValid());
    }
    void cleanup() { storage.reset(); }
    void conversationBudgetUsesTokenizerWithoutGeneration()
    {
        auto p = std::make_shared<Probe>(); Service service(options()); setup(service, p, 128);
        ConversationRequest r; r.model = "model://small"; r.options.maxTokens = 8;
        r.messages = {QJsonObject{{"role", "user"}, {"content", QString(200, QChar(0xac00))}}};
        const auto budget = service.measureConversation(r).get();
        QCOMPARE(budget.inputTokens, 201); QCOMPARE(budget.contextTokens, 128);
        QCOMPARE(p->contexts.load(), 0); QVERIFY(!p->started.load());
        CancellationToken cancelled; cancelled.cancel();
        try { (void)service.measureConversation(r, cancelled).get(); QFAIL("Cancelled measurement succeeded"); }
        catch (const Error& e) { QCOMPARE(e.code(), ErrorCode::Cancelled); }
        r.messages.append(QJsonObject{{"role", "tool"}, {"tool_call_id", "orphan"}, {"content", "bad"}});
        QVERIFY_THROWS_EXCEPTION(Error, (void)service.measureConversation(r).get());
        service.unloadModel("model://small").get(); // Measurement released its residency lease.
        QCOMPARE(p->unloads.load(), 1);
        r.messages.removeLast(); r.keepAliveMs = 0;
        (void)service.measureConversation(r).get();
        (void)service.stats().get(); // Wait until all preparation temporaries have been destroyed.
        QCOMPARE(p->unloads.load(), 2);
        QCOMPARE(p->preparedDestroyedAfterUnload.load(), 0);
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
        QCOMPARE(first.errorCode, ErrorCode::None); QCOMPARE(first.toolCalls, QJsonArray{call});
        QCOMPARE(service.stats().get().sessions, 0); // The durable transcript belongs to the caller.
        r.messages.append(QJsonObject{{"role", "assistant"}, {"tool_calls", first.toolCalls}});
        r.messages.append(QJsonObject{{"role", "tool"}, {"tool_call_id", "call-1"}, {"content", "observed"}});
        p->answer = "{\"text\":\"observed\"}";
        QString streamed;
        const auto second = service.converse(r, [&](const StreamEvent& e) {
            if (e.kind == StreamEventKind::Delta) streamed += e.text;
        }).result.get();
        QCOMPARE(second.errorCode, ErrorCode::None); QCOMPARE(second.text, "observed"); QCOMPARE(streamed, second.text);
        QVERIFY(second.usage.cachedTokens > 0); QCOMPARE(p->contexts.load(), 1);
        installFixture(service, "other", 1024); (void)service.loadModel({"model://other", 1024}).get();
        r.model = "model://other";
        const auto other = service.converse(r).result.get();
        QCOMPARE(other.errorCode, ErrorCode::None); QCOMPARE(other.usage.cachedTokens, 0); QCOMPARE(p->contexts.load(), 2);
        r.messages = {QJsonObject{{"role", "user"}, {"content", "Read a.txt"}}};
        p->answer = QString::fromUtf8(QJsonDocument(QJsonObject{{"text", "visible"}, {"tool_calls", QJsonArray{call}}}).toJson());
        const auto failed = service.converse(r, [](const StreamEvent& event) {
            if (event.kind == StreamEventKind::Delta) throw std::runtime_error("consumer failed");
        }).result.get();
        QCOMPARE(failed.errorCode, ErrorCode::ConsumerFailure); QVERIFY(failed.toolCalls.isEmpty());
    }
    void structuredConversationRejectsBrokenToolProtocol()
    {
        auto p = std::make_shared<Probe>(); Service service(options()); setup(service, p);
        ConversationRequest r; r.model = "model://small"; r.options.maxTokens = 8;
        r.messages = {QJsonObject{{"role", "tool"}, {"tool_call_id", "orphan"}, {"content", "data"}}};
        QCOMPARE(service.converse(r).result.get().errorCode, ErrorCode::InvalidArgument);
        r.messages = {QJsonObject{{"role", "user"}, {"content", "hello"}}}; r.toolChoice = "invalid";
        QCOMPARE(service.converse(r).result.get().errorCode, ErrorCode::InvalidArgument);
        r.toolChoice = "required";
        QCOMPARE(service.converse(r).result.get().errorCode, ErrorCode::InvalidArgument);
        r.toolChoice = "auto";
        r.messages.append(QJsonObject{{"role", "assistant"}, {"tool_calls", QJsonArray{
            QJsonObject{{"id", "pending"}, {"type", "function"}, {"function", QJsonObject{{"name", "Read"}, {"arguments", "{}"}}}}
        }}});
        r.messages.append(QJsonObject{{"role", "user"}, {"content", "skip the result"}});
        QCOMPARE(service.converse(r).result.get().errorCode, ErrorCode::InvalidArgument);
        QCOMPARE(p->contexts.load(), 0);
    }
    void agentModelHookControlsReachMeasurementAndGeneration()
    {
        auto p=std::make_shared<Probe>();Service service(options());setup(service,p,8192);
        p->answer="{\"text\":\"{\\\"ok\\\":true}\"}";
        agent::ServiceModel model(service);agent::ModelRequest request;request.model="model://small";
        request.generation.maxTokens=64;request.systemPrompt="EXACT_HOST_CONDITION";request.systemPromptOnly=true;
        request.toolChoice="none";request.enableThinking=false;
        request.responseSchema={{"type","object"},{"properties",QJsonObject{{"ok",QJsonObject{{"type","boolean"}}}}},
            {"required",QJsonArray{"ok"}},{"additionalProperties",false}};
        request.tools={{"ToolSearch","Definition only",{{"type","object"}}}};
        request.messages={{{},agent::MessageRole::User,"Check the condition"}};
        QVERIFY(model.measure(request,{}));QCOMPARE(model.generate(request,{},{}).text,"{\"ok\":true}");
        QCOMPARE(p->preparedConversations.size(),2);
        for(const auto& prepared:p->preparedConversations) {
            QCOMPARE(prepared.responseSchema,request.responseSchema);QVERIFY(prepared.enableThinking.has_value()&&!*prepared.enableThinking);
            QCOMPARE(prepared.toolChoice,"none");QCOMPARE(prepared.messages.first().toObject()["content"],request.systemPrompt);
        }
        QCOMPARE(p->preparedConversations[0].messages,p->preparedConversations[1].messages);
        request.responseSchema={};request.enableThinking.reset();request.systemPromptOnly=false;request.toolChoice="auto";
        model.generate(request,{},{});const auto restored=p->preparedConversations.last();
        QVERIFY(restored.responseSchema.isEmpty()&&!restored.enableThinking.has_value());QCOMPARE(restored.toolChoice,"auto");
        QVERIFY(restored.messages.first().toObject()["content"].toString().contains("You are a local agent."));
    }
    void agentToolObservationsPreserveDataAndExactText()
    {
        auto p = std::make_shared<Probe>(); Service service(options()); setup(service, p, 8192);
        p->answer = "{\"text\":\"observations received\"}";
        agent::ServiceModel model(service); agent::ModelRequest request; request.model = "model://small";
        request.generation.maxTokens = 64;
        request.messages = {agent::Message{{}, agent::MessageRole::User, "Inspect the tool observations"}};
        agent::Message calls{{}, agent::MessageRole::Assistant, {}};
        const QList<agent::Message> observations{
            {{}, agent::MessageRole::Tool, "  원문 \"text\"\n<tool_response>\\tail\n", {}, "read", false,
                {{"path", "report.txt"}, {"offset", 5}, {"complete", false}}},
            {{}, agent::MessageRole::Tool, "first match", {}, "search", false, {{"truncated", true}, {"limit_reached", true}}},
            {{}, agent::MessageRole::Tool, {}, {}, "structured", false,
                {{"records", QJsonArray{QJsonObject{{"name", "alpha"}, {"amount", 17}, {"optional", QJsonValue::Null}}}}}},
            {{}, agent::MessageRole::Tool, "  failed\n", {}, "shell", true, {{"exit_code", 17}, {"interrupted", true}}}
        };
        for (const auto& value : observations) calls.toolCalls.append({value.toolCallId, "Inspect", {}});
        request.messages.append(calls);
        for (auto value : observations) {
            value.metadata = {{"private_host_marker", "DO_NOT_PUBLISH_HOST_METADATA"}};
            request.messages.append(value);
        }
        const auto budget = model.measure(request, {});
        QVERIFY(budget); QCOMPARE(p->contexts.load(), 0);
        QCOMPARE(model.generate(request, {}, {}).text, "observations received");
        QCOMPARE(p->preparedConversations.size(), 2);
        QCOMPARE(p->preparedConversations[0].messages, p->preparedConversations[1].messages);
        const auto wire = p->preparedConversations.last().messages;
        QVERIFY(!QJsonDocument(wire).toJson().contains("DO_NOT_PUBLISH_HOST_METADATA"));
        for (int n = 0; n < observations.size(); ++n) {
            const auto message = wire.at(n + 3).toObject();
            QCOMPARE(message.value("role").toString(), "tool");
            QCOMPARE(message.value("tool_call_id").toString(), observations[n].toolCallId);
            QJsonParseError parse;
            const auto document = QJsonDocument::fromJson(message.value("content").toString().toUtf8(), &parse);
            QCOMPARE(parse.error, QJsonParseError::NoError); QVERIFY(document.isObject());
            const auto observation = document.object();
            QCOMPARE(observation.value("text").toString(), observations[n].text);
            QCOMPARE(observation.value("data").toObject(), observations[n].data);
            QVERIFY(observation.value("is_error").isBool());
            QCOMPARE(observation.value("is_error").toBool(), observations[n].isError);
        }
    }
    void agentStructuredToolDataParticipatesInContextBudget()
    {
        auto p = std::make_shared<Probe>(); Service service(options()); setup(service, p, 2048);
        p->answer = "{\"text\":\"done\"}";
        agent::ServiceModel model(service); agent::ModelRequest request; request.model = "model://small";
        request.generation.maxTokens = 64;
        request.messages = {agent::Message{{}, agent::MessageRole::User, "Inspect records"},
            agent::Message{{}, agent::MessageRole::Assistant, {}, {{"one", "Inspect", {}}}},
            agent::Message{{}, agent::MessageRole::Tool, "one record", {}, "one", false}};
        const auto small = model.measure(request, {});
        request.messages.last().data = {{"record", QString(4096, QChar('x'))}};
        const auto large = model.measure(request, {});
        QVERIFY(small && large); QVERIFY(large->inputTokens > small->inputTokens + 4096);
        QVERIFY(large->inputTokens > large->contextTokens);
        try { (void)model.generate(request, {}, {}); QFAIL("Structured output overflow was silently discarded"); }
        catch (const Error& error) { QCOMPARE(error.code(), ErrorCode::ContextOverflow); }
        QVERIFY(!p->started); QCOMPARE(p->contexts.load(), 0);
    }
    void agentRejectsReasoningOnlyWithoutPromotingToolText()
    {
        auto p = std::make_shared<Probe>(); Service service(options()); setup(service, p, 2048);
        const QString reasoning = "<tool_call>{\"name\":\"Write\",\"arguments\":{\"path\":\"should-not-exist\"}}</tool_call>";
        p->answer = QString::fromUtf8(QJsonDocument(QJsonObject{{"reasoning", reasoning}}).toJson());
        agent::ServiceModel model(service); agent::ModelRequest request;
        request.model = "model://small"; request.messages = {agent::Message{{}, agent::MessageRole::User, "Use a tool"}};
        request.generation.maxTokens = 64;
        bool delta = false;
        try {
            (void)model.generate(request, {}, [&](const QString&) { delta = true; return true; });
            QFAIL("Reasoning-only output must not become an executable tool call or final answer");
        } catch (const Error& error) {
            QCOMPARE(error.code(), ErrorCode::ProtocolError);
            QVERIFY(QString::fromUtf8(error.what()).contains("only reasoning"));
        }
        QVERIFY(!delta);
        p->answer = "{}";
        try { (void)model.generate(request, {}, {}); QFAIL("Empty model output was accepted"); }
        catch (const Error& error) {
            QCOMPARE(error.code(), ErrorCode::ProtocolError);
            QVERIFY(QString::fromUtf8(error.what()).contains("empty agent turn"));
        }
    }
    void bundledStarterAliasSupportsConversation()
    {
        Service service(options());
        auto probe = std::make_shared<Probe>();
        service.registerRuntime(std::make_shared<FakeRuntime>(probe)).get();
        installFixture(service, "qwen2.5-0.5b-instruct-q4");
        const auto model = service.resolveModel("qwen2.5:0.5b").get();
        QCOMPARE(model.uri, QStringLiteral("model://qwen2.5-0.5b-instruct-q4"));
        const auto session = service.createSession("qwen2.5:0.5b", "Be concise.").get();
        QCOMPARE(service.chat(request(session)).result.get().errorCode, ErrorCode::None);
        const auto next = service.chat(request(session, "Again.")).result.get();
        QCOMPARE(next.errorCode, ErrorCode::None);
        QVERIFY(next.usage.cachedTokens > 0);
        service.resetSession(session).get();
        const auto history = service.session(session).get().messages;
        QCOMPARE(history.size(), 1);
        QCOMPARE(history.first().role, Role::System);
        QCOMPARE(history.first().content, QStringLiteral("Be concise."));
        QCOMPARE(service.chat(request(session)).result.get().usage.cachedTokens, 0);
        service.closeSession(session).get();
    }
    void residencyReuseLruAndSessionRestoration()
    {
        auto p = std::make_shared<Probe>();
        auto o = options(); o.memoryBudgetBytes = 2 * p->memoryBytes;
        Service service(o); setup(service, p);
        installFixture(service, "b"); installFixture(service, "c");
        (void)service.loadModel({"model://small"}).get(); QCOMPARE(p->loads.load(), 1);
        const auto a = service.createSession("model://small").get();
        const auto b = service.createSession("model://b").get();
        const auto c = service.createSession("model://c").get();
        QCOMPARE(service.chat(request(a)).result.get().errorCode, ErrorCode::None);
        QCOMPARE(service.chat(request(b)).result.get().errorCode, ErrorCode::None);
        QCOMPARE(p->loads.load(), 2); QCOMPARE(service.models().get().size(), 2);
        QCOMPARE(service.chat(request(a)).result.get().errorCode, ErrorCode::None); // Touch a; b is oldest.
        QCOMPARE(service.chat(request(c)).result.get().errorCode, ErrorCode::None);
        QVERIFY(!service.resolveModel("model://b").get().loaded);
        QCOMPARE(service.session(b).get().messages.size(), 2); // LRU discarded KV, not conversation.
        const auto restored = service.chat(request(b, "again")).result.get();
        QCOMPARE(restored.errorCode, ErrorCode::None); QCOMPARE(restored.usage.cachedTokens, 0);
        QCOMPARE(service.session(b).get().messages.size(), 4);
        const auto stats = service.stats().get();
        QCOMPARE(stats.loadedModels, 2); QCOMPARE(stats.residentBytes, 2 * p->memoryBytes);
        QCOMPARE(stats.modelLoads, quint64(4)); QCOMPARE(stats.modelEvictions, quint64(2));
        QVERIFY(stats.availableRamKnown); QVERIFY(stats.availableRamBytes > 0);
    }
    void expiryRunsWithoutRequestsAndZeroLifetimeReleasesAfterCancellation()
    {
        auto p = std::make_shared<Probe>();
        Service service(options()); setup(service, p);
        const auto session = service.createSession("model://small").get();
        auto r = request(session); r.keepAliveMs = 30;
        QCOMPARE(service.chat(r).result.get().errorCode, ErrorCode::None);
        QTRY_COMPARE_WITH_TIMEOUT(p->unloads.load(), 1, 2000); // No service API calls drive this eviction.
        QCOMPARE(service.stats().get().cachedContexts, 0);
        QCOMPARE(service.session(session).get().messages.size(), 2);
        p->release = false; p->started = false; r.keepAliveMs = 0;
        const auto active = service.chat(r);
        QTRY_VERIFY(p->started.load());
        auto snapshot = service.models();
        QCOMPARE(snapshot.wait_for(50ms), std::future_status::ready);
        const auto resident = snapshot.get();
        QCOMPARE(resident.size(), 1); QCOMPARE(resident.first().activeRequests, 1);
        QCOMPARE(resident.first().expiresInMs, qint64(-1));
        QTest::qWait(150); QCOMPARE(p->unloads.load(), 1); // Active lease survives a zero TTL.
        active.cancel(); QCOMPARE(active.result.get().finishReason, FinishReason::Cancelled);
        QCOMPARE(p->unloads.load(), 2); QCOMPARE(service.stats().get().loadedModels, 0);
        QCOMPARE(service.session(session).get().messages.size(), 2);
        p->release = true;
        QCOMPARE(service.chat(r).result.get().errorCode, ErrorCode::None);
        QCOMPARE(service.stats().get().loadedModels, 0);
    }
    void admissionRejectsBeforeRuntimeAllocation()
    {
        auto p = std::make_shared<Probe>(); auto o = options(); o.memoryBudgetBytes = p->memoryBytes - 1;
        Service service(o); service.registerRuntime(std::make_shared<FakeRuntime>(p)).get(); installFixture(service);
        const auto session = service.createSession("model://small").get();
        QCOMPARE(service.chat(request(session)).result.get().errorCode, ErrorCode::ResourceLimit);
        QCOMPARE(p->loads.load(), 0); QCOMPARE(service.session(session).get().messages.size(), 0);
        QCOMPARE(service.stats().get().residentBytes, quint64(0));
    }
    void managedLifecycleAndIntegrityGate()
    {
        auto p = std::make_shared<Probe>();
        const auto opts = options();
        Service service(opts);
        service.registerRuntime(std::make_shared<FakeRuntime>(p)).get();
        installFixture(service);
        QCOMPARE(p->loads.load(), 0); // Install/list/resolve/verify do not load the engine.
        QCOMPARE(service.installedModels().get().models.size(), 1);
        QVERIFY(!service.resolveModel(QStringLiteral("model://small")).get().loaded);
        QVERIFY(service.verifyModel(QStringLiteral("model://small")).get().valid);
        QVERIFY_THROWS_EXCEPTION(Error, (void)service.loadModel({"/model.test"}).get());
        QVERIFY_THROWS_EXCEPTION(Error, (void)service.loadModel({"model://small", 129}).get());
        auto loaded = service.loadModel({"model://small"}).get();
        QCOMPARE(loaded.contextTokens, 128);
        QVERIFY(service.installedModels().get().models.first().loaded);
        QVERIFY_THROWS_EXCEPTION(Error, service.removeModel(QStringLiteral("model://small")).get());
        const auto session = service.createSession(QStringLiteral("model://small")).get();
        QVERIFY_THROWS_EXCEPTION(Error, service.unloadModel(QStringLiteral("model://small")).get());
        service.closeSession(session).get();
        service.unloadModel(QStringLiteral("model://small")).get();
        QFile file(QDir(opts.modelsDirectory).filePath(QStringLiteral("small/model.test")));
        QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write("changed"), qint64(7)); file.close();
        QVERIFY(!service.verifyModel(QStringLiteral("model://small")).get().valid);
        QVERIFY_THROWS_EXCEPTION(Error, (void)service.loadModel({"model://small"}).get());
        QCOMPARE(p->loads.load(), 1); // Corrupt installed bytes never reach a runtime.
        service.removeModel(QStringLiteral("model://small")).get();
        QVERIFY(service.installedModels().get().models.isEmpty());
    }
    void manifestCapabilitiesGateChat()
    {
        auto p = std::make_shared<Probe>();
        Service service(options());
        service.registerRuntime(std::make_shared<FakeRuntime>(p)).get();
        installFixture(service, QStringLiteral("text-only"), 128, {"text-generation"});
        (void)service.loadModel({"model://text-only"}).get();
        QVERIFY_THROWS_EXCEPTION(Error, (void)service.createSession(QStringLiteral("model://text-only")).get());
        QCOMPARE(service.stats().get().sessions, 0);
    }
    void ipcOwnsExecutionChoice()
    {
        auto p = std::make_shared<Probe>();
        Service service(options());
        setup(service, p);
        LocalIpcServer server(service);
        const auto name = QDir::current().filePath(QStringLiteral("policy-") + QUuid::createUuid().toString(QUuid::Id128).first(8));
        QVERIFY(server.listen(name));
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
        QTRY_COMPARE(socket.state(), QLocalSocket::ConnectedState);
        auto send = [&](const QString& method, const QJsonObject& params) {
            socket.write(QJsonDocument(QJsonObject{{"id", "r"}, {"method", method}, {"params", params}}).toJson(QJsonDocument::Compact) + '\n');
        };
        send(QStringLiteral("hardware.get"), {});
        QTRY_COMPARE(messages.size(), 1);
        QCOMPARE(messages.takeFirst()["result"].toObject(), hardwareObject(service.hardware()));
        for (const auto& field : {"runtime", "backend", "device", "device_id", "gpu_layers"}) {
            send(QStringLiteral("models.load"), {{"model", "model://auto"}, {QString::fromLatin1(field), "cpu"}});
            QTRY_COMPARE(messages.size(), 1);
            QCOMPARE(messages.takeFirst()["error"].toObject()["code"].toString(), QStringLiteral("invalid_argument"));
        }
        send(QStringLiteral("models.load"), {{"model", "model://auto"}, {"options", QJsonObject{{"backend", "cuda"}}}});
        QTRY_COMPARE(messages.size(), 1);
        QCOMPARE(messages.takeFirst()["error"].toObject()["code"].toString(), QStringLiteral("invalid_argument"));
        QCOMPARE(p->loads.load(), 1);
        installFixture(service, QStringLiteral("auto"));
        send(QStringLiteral("models.load"), {{"model", "model://auto"}});
        QTRY_COMPARE(messages.size(), 1);
        const auto loaded = messages.takeFirst()["result"].toObject();
        QCOMPARE(loaded["model"].toString(), QStringLiteral("model://auto"));
        QCOMPARE(loaded["execution"].toObject()["runtime"].toString(), QStringLiteral("test"));
        QCOMPARE(loaded["execution"].toObject()["backend"].toString(), QStringLiteral("cpu"));
        QCOMPARE(p->loads.load(), 2);
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
        QCOMPARE(result.text, p->answer);
        QCOMPARE(result.finishReason, FinishReason::Stop);
        QCOMPARE(events.front().kind, StreamEventKind::Started);
        QCOMPARE(events.back().kind, StreamEventKind::Finished);
        QCOMPARE(service.session(id).get().messages.size(), 3);
        const auto second = service.chat(request(id, QStringLiteral("again"))).result.get();
        QVERIFY(second.usage.cachedTokens > 0);
        QCOMPARE(p->contexts.load(), 1);
        QCOMPARE(service.session(id).get().messages.size(), 5);
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
        QCOMPARE(h.result.get().finishReason, FinishReason::Error);
        QVERIFY(service.session(id).get().messages.isEmpty());
        QCOMPARE(service.stats().get().cachedContexts, 0);
        p->started = false;
        p->release = false;
        auto cancelled = service.chat(request(id));
        QTRY_VERIFY(p->started.load());
        cancelled.cancel();
        QCOMPARE(cancelled.result.get().finishReason, FinishReason::Cancelled);
        QVERIFY(service.session(id).get().messages.isEmpty());
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
        QTRY_VERIFY(p->started.load());
        auto second = service.chat(request(id, QStringLiteral("second")));
        auto third = service.chat(request(id, QStringLiteral("third")));
        QCOMPARE(third.result.get().errorCode, ErrorCode::QueueFull);
        second.cancel();
        p->release = true;
        QCOMPARE(first.result.get().finishReason, FinishReason::Stop);
        QCOMPARE(second.result.get().finishReason, FinishReason::Cancelled);
        QCOMPARE(p->peak.load(), 1);
        QCOMPARE(service.session(id).get().messages.size(), 2);
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
        QCOMPARE(p->destroyed.load(), 1);
        QCOMPARE(service.stats().get().cacheEvictions, quint64(1));
        QVERIFY_THROWS_EXCEPTION(Error, service.unloadModel(QStringLiteral("model://small")).get());
        service.resetSession(b).get();
        QCOMPARE(service.stats().get().cachedContexts, 0);
        service.closeSession(a).get();
        service.closeSession(b).get();
        service.unloadModel(QStringLiteral("model://small")).get();
        QVERIFY(service.models().get().isEmpty());
    }
    void contextTrimsCompleteTurnsAndRejectsOversize()
    {
        auto p = std::make_shared<Probe>();
        Service service(options());
        setup(service, p, 24);
        const auto id = service.createSession(QStringLiteral("model://small"), QStringLiteral("S")).get();
        QCOMPARE(service.chat(request(id)).result.get().finishReason, FinishReason::Stop);
        auto result = service.chat(request(id, QStringLiteral("world"))).result.get();
        QCOMPARE(result.usage.droppedMessages, 2);
        const auto before = service.session(id).get().messages;
        auto oversized = service.chat(request(id, QString(40, 'x'))).result.get();
        QCOMPARE(oversized.errorCode, ErrorCode::ContextOverflow);
        QCOMPARE(service.session(id).get().messages, before);
    }
    void failuresAndInvalidRequests()
    {
        auto p = std::make_shared<Probe>();
        Service service(options());
        setup(service, p);
        const auto id = service.createSession(QStringLiteral("model://small")).get();
        auto invalid = request(id);
        invalid.options.maxTokens = 0;
        QCOMPARE(service.chat(invalid).result.get().errorCode, ErrorCode::InvalidArgument);
        p->fail = true;
        QCOMPARE(service.chat(request(id)).result.get().errorCode, ErrorCode::RuntimeFailure);
        p->fail = false;
        QCOMPARE(service.chat(request(id)).result.get().finishReason, FinishReason::Stop);
        QCOMPARE(service.chat(request(QStringLiteral("missing"))).result.get().errorCode, ErrorCode::NotFound);
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
        QCOMPARE(result.text, QStringLiteral("abc"));
        QCOMPARE(streamed, result.text);
    }
    void shutdownSettlesActiveAndQueuedRequests()
    {
        auto p = std::make_shared<Probe>();
        auto service = std::make_unique<Service>(options());
        setup(*service, p);
        const auto id = service->createSession(QStringLiteral("model://small")).get();
        p->release = false;
        auto active = service->chat(request(id));
        QTRY_VERIFY(p->started.load());
        auto queued = service->chat(request(id));
        service.reset();
        QCOMPARE(active.result.get().finishReason, FinishReason::Cancelled);
        QCOMPARE(queued.result.get().finishReason, FinishReason::Cancelled);
        QCOMPARE(p->contexts.load(), p->destroyed.load());
    }
    void queuedTurnsObserveCommittedHistory()
    {
        auto p = std::make_shared<Probe>();
        Service service(options());
        setup(service, p);
        const auto id = service.createSession(QStringLiteral("model://small")).get();
        p->release = false;
        auto first = service.chat(request(id));
        QTRY_VERIFY(p->started.load());
        auto second = service.chat(request(id, QStringLiteral("again")));
        p->release = true;
        const auto firstResult = first.result.get();
        const auto secondResult = second.result.get();
        QCOMPARE(firstResult.finishReason, FinishReason::Stop);
        QVERIFY(secondResult.usage.promptTokens > firstResult.usage.promptTokens);
        QCOMPARE(service.session(id).get().messages.size(), 4);
    }
    void cacheTokenBudgetAndRegistrationErrors()
    {
        auto p = std::make_shared<Probe>();
        auto opts = options();
        opts.maxCachedContexts = 3;
        opts.maxCachedContextTokens = 128;
        Service service(opts);
        setup(service, p);
        QVERIFY_THROWS_EXCEPTION(Error, service.registerRuntime(std::make_shared<FakeRuntime>(p)).get());
        installFixture(service, QStringLiteral("big"), 129);
        QVERIFY_THROWS_EXCEPTION(Error, (void)service.loadModel({QStringLiteral("model://big"), 129}).get());
        const auto a = service.createSession(QStringLiteral("model://small")).get();
        const auto b = service.createSession(QStringLiteral("model://small")).get();
        (void)service.chat(request(a)).result.get();
        (void)service.chat(request(b)).result.get();
        const auto stats = service.stats().get();
        QCOMPARE(stats.reservedContextTokens, 128);
        QCOMPARE(stats.cachedContexts, 1);
        QCOMPARE(stats.cacheEvictions, quint64(1));
    }
    void ipcRoundTripAndFraming()
    {
        auto p = std::make_shared<Probe>();
        Service service(options());
        setup(service, p);
        LocalIpcServer server(service);
        const auto name = QDir::current().filePath(QStringLiteral("ipc-") + QUuid::createUuid().toString(QUuid::Id128).first(8));
        QVERIFY2(server.listen(name), qPrintable(server.errorString()));
        LocalIpcServer duplicate(service);
        QVERIFY(!duplicate.listen(name)); // Never unlink a live endpoint.
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
        QTRY_COMPARE(socket.state(), QLocalSocket::ConnectedState);
        socket.write("not-json\n{\"id\":\"c\",\"method\":\"sessions.");
        socket.flush();
        QTRY_COMPARE(messages.size(), 1);
        QCOMPARE(messages[0][QStringLiteral("error")].toObject()[QStringLiteral("code")].toString(), QStringLiteral("protocol_error"));
        socket.write("create\",\"params\":{\"model\":\"model://small\"}}\n");
        QTRY_COMPARE(messages.size(), 2);
        const auto session = messages.back()[QStringLiteral("result")].toObject()[QStringLiteral("session_id")].toString();
        QVERIFY(!session.isEmpty());
        const QJsonObject chat{{QStringLiteral("id"), QStringLiteral("g")}, {QStringLiteral("method"), QStringLiteral("chat")},
            {QStringLiteral("params"), QJsonObject{{QStringLiteral("session_id"), session}, {QStringLiteral("prompt"), QStringLiteral("hello")},
                {QStringLiteral("options"), QJsonObject{{QStringLiteral("max_tokens"), 8}}}}}};
        socket.write(QJsonDocument(chat).toJson(QJsonDocument::Compact) + '\n');
        QTRY_COMPARE(messages.back()[QStringLiteral("event")].toString(), QStringLiteral("done"));
        QCOMPARE(messages[2][QStringLiteral("event")].toString(), QStringLiteral("accepted"));
        QCOMPARE(messages[3][QStringLiteral("event")].toString(), QStringLiteral("started"));
        QString text;
        for (const auto& m : messages) if (m[QStringLiteral("event")] == QStringLiteral("delta")) text += m[QStringLiteral("text")].toString();
        QCOMPARE(text, p->answer);
        QCOMPARE(messages.back()[QStringLiteral("result")].toObject()[QStringLiteral("text")].toString(), text);
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
        QCOMPARE(output, QString::fromUtf8("가😀"));
        QCOMPARE(QString(decoder(QByteArray::fromHex("eab0"))), QString{});
        QCOMPARE(decoder.finish(), QString(QChar::ReplacementCharacter));
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
        const auto name = QDir::current().filePath(QStringLiteral("ipc-") + QUuid::createUuid().toString(QUuid::Id128).first(8));
        QVERIFY(server.listen(name));
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
        QTRY_COMPARE(socket.state(), QLocalSocket::ConnectedState);
        p->release = false;
        auto send = [&](const QJsonObject& request) { socket.write(QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n'); };
        send({{QStringLiteral("id"), QStringLiteral("g")}, {QStringLiteral("method"), QStringLiteral("chat")},
              {QStringLiteral("params"), QJsonObject{{QStringLiteral("session_id"), session}, {QStringLiteral("prompt"), QStringLiteral("hello")},
                  {QStringLiteral("options"), QJsonObject{{QStringLiteral("max_tokens"), 8}}}}}});
        QTRY_VERIFY(p->started.load());
        QTRY_VERIFY(!messages.isEmpty());
        const auto requestId = messages.front()[QStringLiteral("request_id")].toString();
        QVERIFY(!requestId.isEmpty());
        send({{QStringLiteral("id"), QStringLiteral("x")}, {QStringLiteral("method"), QStringLiteral("cancel")},
              {QStringLiteral("params"), QJsonObject{{QStringLiteral("request_id"), requestId}}}});
        QTRY_COMPARE(p->active.load(), 0);
        QTRY_VERIFY(std::any_of(messages.begin(), messages.end(), [](const auto& m) { return m[QStringLiteral("event")] == QStringLiteral("done"); }));
        QVERIFY(service.session(session).get().messages.isEmpty());
        const auto done = *std::find_if(messages.begin(), messages.end(), [](const auto& m) { return m[QStringLiteral("event")] == QStringLiteral("done"); });
        QCOMPARE(done[QStringLiteral("result")].toObject()[QStringLiteral("finish_reason")].toString(), QStringLiteral("cancelled"));
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
        const auto name = QDir::current().filePath(QStringLiteral("ipc-") + QUuid::createUuid().toString(QUuid::Id128).first(8));
        QVERIFY(server.listen(name));
        const auto chat = QJsonDocument(QJsonObject{{QStringLiteral("id"), QStringLiteral("g")}, {QStringLiteral("method"), QStringLiteral("chat")},
            {QStringLiteral("params"), QJsonObject{{QStringLiteral("session_id"), session}, {QStringLiteral("prompt"), QStringLiteral("hello")},
                {QStringLiteral("options"), QJsonObject{{QStringLiteral("max_tokens"), 8}}}}}}).toJson(QJsonDocument::Compact) + '\n';
        QLocalSocket socket;
        socket.connectToServer(name);
        QTRY_COMPARE(socket.state(), QLocalSocket::ConnectedState);
        p->release = false;
        socket.write(chat);
        QTRY_VERIFY(p->started.load());
        socket.abort();
        QTRY_COMPARE(p->active.load(), 0);
        QVERIFY(service.session(session).get().messages.isEmpty());
        p->release = true;
        p->started = false;
        // An undrained worker inbox must remain bounded and must not commit a partial turn.
        p->answer = QString(1000, 'x');
        QLocalSocket slow;
        slow.connectToServer(name);
        QTRY_COMPARE(slow.state(), QLocalSocket::ConnectedState);
        slow.write(chat);
        QTRY_VERIFY(p->started.load());
        QTRY_COMPARE(slow.state(), QLocalSocket::UnconnectedState);
        QVERIFY(service.session(session).get().messages.isEmpty());
        QCOMPARE(service.stats().get().cachedContexts, 0);
        server.close();
    }
};

QTEST_GUILESS_MAIN(ServiceTests)
#include "service_tests.moc"
