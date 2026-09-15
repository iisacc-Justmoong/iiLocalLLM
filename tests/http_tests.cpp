#include <iiLocalLLM.h>
#include <QtTest/QtTest>
#include <QtCore/QTemporaryDir>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QTcpServer>
#include <QtNetwork/QTcpSocket>
#include <chrono>
#include <mutex>
#include <thread>
using namespace iiLocalLLM;
using namespace std::chrono_literals;
namespace {
struct Probe {
    std::mutex mutex;
    QList<ChatMessage> messages;
    QString mode;
    GenerationOptions options;
    ConversationRequest conversation;
    std::atomic_int entered = 0, cancelled = 0;
};
class Context : public RuntimeContext {
public:
    explicit Context(std::shared_ptr<Probe> probe) : probe(std::move(probe)) {}
    std::shared_ptr<Probe> probe;
    RuntimeResult generateConversation(const RuntimeConversationPrompt&, const GenerationOptions&,
        const CancellationToken& token, const TextCallback& text) override {
        token.throwIfCancelled(); ++probe->entered; text("fixture raw response");
        return {FinishReason::Stop, 5, 0};
    }
    RuntimeResult generate(const TokenList&, const GenerationOptions& options, const CancellationToken& token, const TextCallback& text) override
    {
        ++probe->entered;
        QString mode;
        { std::lock_guard lock(probe->mutex); mode = probe->mode; probe->options = options; }
        int emitted = 0;
        try {
            const QStringList parts = mode == "large" ? QStringList{QString(20000, 'x')} : QStringList{QStringLiteral("안"), QStringLiteral("녕"), QStringLiteral(" 🌍")};
            const int count = mode == "slow" ? 200 : parts.size();
            for (int i = 0; i < count && emitted < options.maxTokens; ++i) {
                token.throwIfCancelled();
                std::this_thread::sleep_for(10ms);
                token.throwIfCancelled();
                ++emitted;
                if (!text(parts[i % parts.size()])) break;
                if (mode == "fail") throw Error(ErrorCode::RuntimeFailure, QStringLiteral("Fixture failure after output"));
            }
            return {FinishReason::Stop, emitted, 0};
        } catch (const Error& error) { if (error.code() == ErrorCode::Cancelled) ++probe->cancelled; throw; }
    }
};
class Model : public RuntimeModel {
public:
    explicit Model(std::shared_ptr<Probe> probe) : probe(std::move(probe)) {}
    std::shared_ptr<Probe> probe;
    RuntimeConversationPrompt prepareConversation(const ConversationRequest& request, const CancellationToken&) override {
        std::lock_guard lock(probe->mutex); probe->conversation = request;
        return {{1, 2, 3}, {}, {}};
    }
    RuntimeConversationReply parseConversation(const RuntimeConversationPrompt&, const QString&) override {
        std::lock_guard lock(probe->mutex);
        const auto last = probe->conversation.messages.last().toObject();
        if (last["role"] == "tool") return {last["content"].toString(), {}, {}};
        return {{}, {}, QJsonArray{QJsonObject{{"id", "app-call"}, {"type", "function"},
            {"function", QJsonObject{{"name", "app_lookup"}, {"arguments", "{\"key\":\"value\"}"}}}}}};
    }
    TokenList tokenize(const QList<ChatMessage>& messages, const CancellationToken&) override
    {
        std::lock_guard lock(probe->mutex);
        probe->messages = messages; probe->mode = messages.back().content;
        return TokenList(messages.size() * 4, 1);
    }
    std::unique_ptr<RuntimeContext> createContext(const CancellationToken&) override { return std::make_unique<Context>(probe); }
};
class Engine : public Runtime {
public:
    explicit Engine(std::shared_ptr<Probe> probe) : probe(std::move(probe)) {}
    std::shared_ptr<Probe> probe;
    QString id() const override { return "http-fixture"; }
    bool supportsModel(const ModelSpec& model) const override { return model.format == "test"; }
    QList<RuntimeDevice> devices(const HardwareInfo&) const override { return {{ComputeBackend::Cpu, {}}}; }
    std::shared_ptr<RuntimeModel> load(const ModelSpec&, const RuntimeDevice&, const CancellationToken&) override { return std::make_shared<Model>(probe); }
};
void write(const QString& name, const QByteArray& bytes)
{
    QFile file(name);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()) throw std::runtime_error("Cannot create fixture");
}
struct Fixture {
    QTemporaryDir root{QDir::current().filePath("http-XXXXXX")};
    std::shared_ptr<Probe> probe = std::make_shared<Probe>();
    std::unique_ptr<Service> service;
    explicit Fixture(int queue = 64)
    {
        if (!root.isValid()) throw std::runtime_error("Cannot create test directory");
        ServiceOptions options; options.modelsDirectory = root.filePath("Models"); options.maxQueuedRequests = queue; options.keepAliveMs = 300000;
        service = std::make_unique<Service>(options);
        service->registerRuntime(std::make_shared<Engine>(probe)).get();
        const auto source = root.filePath("source"); QDir().mkpath(source);
        write(source + "/manifest.json", QJsonDocument(QJsonObject{{"id", "test"}, {"architecture", "fixture"}, {"format", "test"},
            {"entry_point", "model.test"}, {"quantization", "none"}, {"context_length", 512}, {"capabilities", QJsonArray{"chat"}}}).toJson());
        write(source + "/model.test", "fixture");
        (void)service->installModel(source).get(); (void)service->loadModel({"model://test", 512}).get();
    }
};
QJsonObject body(QString prompt = "hello", bool stream = false)
{
    return {{"model", "model://test"}, {"messages", QJsonArray{QJsonObject{{"role", "user"}, {"content", prompt}}}},
        {"max_tokens", 256}, {"stream", stream}, {"temperature", 0}};
}
struct Reply { int status; QByteArray bytes; QByteArray contentType; };
Reply request(quint16 port, QByteArray bytes, QString path = "/v1/chat/completions", bool get = false,
              QList<QPair<QByteArray, QByteArray>> headers = {}, std::function<void(QNetworkReply*, QByteArray)> onData = {})
{
    QNetworkAccessManager network;
    QNetworkRequest request(QUrl(QStringLiteral("http://127.0.0.1:%1").arg(port) + path));
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    for (const auto& [name, value] : headers) request.setRawHeader(name, value);
    auto* reply = get ? network.get(request) : network.post(request, bytes);
    QEventLoop loop; QTimer timeout; timeout.setSingleShot(true);
    QByteArray result;
    QObject::connect(reply, &QNetworkReply::readyRead, &loop, [&] { const auto chunk = reply->readAll(); result += chunk; if (onData) onData(reply, chunk); });
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QObject::connect(&timeout, &QTimer::timeout, &loop, [&] { reply->abort(); loop.quit(); });
    timeout.start(10000); loop.exec();
    result += reply->readAll();
    return {reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), result, reply->rawHeader("Content-Type")};
}
Reply post(quint16 port, const QJsonObject& value) { return request(port, QJsonDocument(value).toJson()); }
QJsonObject object(const Reply& reply) { return QJsonDocument::fromJson(reply.bytes).object(); }
QList<QJsonObject> events(const Reply& reply)
{
    QList<QJsonObject> result;
    for (const auto& line : reply.bytes.split('\n'))
        if (line.startsWith("data: ") && line != "data: [DONE]") result.append(QJsonDocument::fromJson(line.mid(6)).object());
    return result;
}
}
class HttpTests : public QObject {
    Q_OBJECT
private slots:
    void waitingRpcCannotOccupyThePermissionControlCapacity() {
        class Handler final:public RpcHandler {
        public:
            std::promise<QJsonValue> held,heldControl;std::atomic_bool entered=false,controlEntered=false;
            bool isControlMethod(const QString& method) const override {return method=="agent.permissions.pending"||method=="test/controlWait";}
            RpcHandle dispatch(QString method,QJsonObject,QString,RpcEventCallback) override {
                if(method=="test/wait") {entered=true;return {"wait",{},held.get_future().share()};}
                if(method=="test/controlWait") {controlEntered=true;return {"control-wait",{},heldControl.get_future().share()};}
                std::promise<QJsonValue> value;auto future=value.get_future().share();value.set_value(QJsonObject{{"requests",QJsonArray{}}});
                return {"control",{},future};
            }
        };
        Fixture fixture;auto handler=std::make_shared<Handler>();HttpOptions limits;limits.workerThreads=1;limits.maxControlRequests=1;limits.requestTimeoutMs=1800;
        HttpApiServer server(*fixture.service,limits);server.setRpcHandler(handler);QVERIFY(server.listen());
        const QList<QPair<QByteArray,QByteArray>> headers{{"Authorization","Bearer fixture-credential"}};
        auto rpc=[&](const QString& method){return request(server.port(),QJsonDocument(QJsonObject{{"id",method},{"method",method}}).toJson(),"/v1/rpc",false,headers);};
        auto waiting=std::async(std::launch::async,[&]{return rpc("test/wait");});
        struct Release {std::shared_ptr<Handler> handler;~Release(){try{handler->held.set_value(QJsonObject{});}catch(...) {}}} release{handler};
        QTRY_VERIFY(handler->entered.load());QElapsedTimer timer;timer.start();
        const auto control=rpc("agent.permissions.pending");QCOMPARE(control.status,200);
        QVERIFY2(timer.elapsed()<700,"Permission control waited behind ordinary HTTP work");
        QCOMPARE(rpc("test/ordinary").status,429);
        QCOMPARE(request(server.port(),{},"/v1/models",true).status,429);
        QCOMPARE(request(server.port(),{},"/health",true).status,200);
        handler->held.set_value(QJsonObject{});QCOMPARE(waiting.get().status,200);
        auto controlWaiting=std::async(std::launch::async,[&]{return rpc("test/controlWait");});
        struct ControlRelease {std::shared_ptr<Handler> handler;~ControlRelease(){try{handler->heldControl.set_value(QJsonObject{});}catch(...) {}}} controlRelease{handler};
        QTRY_VERIFY(handler->controlEntered.load());
        QCOMPARE(rpc("agent.permissions.pending").status,429);
        QCOMPARE(rpc("test/ordinary").status,200);
        handler->heldControl.set_value(QJsonObject{});QCOMPARE(controlWaiting.get().status,200);
        QCOMPARE(rpc("agent.permissions.pending").status,200);
    }
    void slowResponseKeepsCapacityUntilTheSocketCloses_data() {
        QTest::addColumn<bool>("streaming");QTest::addColumn<bool>("control");
        QTest::newRow("json-work")<<false<<false;QTest::newRow("sse-work")<<true<<false;
        QTest::newRow("json-control")<<false<<true;QTest::newRow("sse-control")<<true<<true;
    }
    void slowResponseKeepsCapacityUntilTheSocketCloses() {
        QFETCH(bool,streaming);QFETCH(bool,control);
        class Handler final:public RpcHandler {
        public:
            bool isControlMethod(const QString& method) const override {return method.startsWith("control/");}
            RpcHandle dispatch(QString method,QJsonObject,QString,RpcEventCallback) override {
                std::promise<QJsonValue> value;auto future=value.get_future().share();
                value.set_value(QJsonObject{{"data",method.endsWith("large")?QString(16*1024*1024,'x'):QString("ok")}});
                return {"response",{},future};
            }
        };
        Fixture fixture;HttpOptions limits;limits.workerThreads=1;limits.maxControlRequests=1;
        limits.maxBufferedOutputBytes=24*1024*1024;limits.writeTimeoutMs=10000;
        HttpApiServer server(*fixture.service,limits);server.setRpcHandler(std::make_shared<Handler>());QVERIFY(server.listen());
        const auto prefix=control?QString("control/"):QString("work/");
        QTcpSocket stalled;stalled.setReadBufferSize(1);stalled.connectToHost("127.0.0.1",server.port());QVERIFY(stalled.waitForConnected());
        stalled.setSocketOption(QAbstractSocket::ReceiveBufferSizeSocketOption,1024);
        const auto body=QJsonDocument(QJsonObject{{"id","slow"},{"method",prefix+"large"},{"stream",streaming}}).toJson(QJsonDocument::Compact);
        const QByteArray headers="POST /v1/rpc HTTP/1.1\r\nHost: 127.0.0.1:"+QByteArray::number(server.port())
            +"\r\nAuthorization: Bearer fixture\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: "+QByteArray::number(body.size())+"\r\n\r\n";
        stalled.write(headers+body);QVERIFY(stalled.waitForBytesWritten());QVERIFY(stalled.waitForReadyRead());
        auto probe=[&](const QString& method){return request(server.port(),QJsonDocument(QJsonObject{{"id","probe"},{"method",method}}).toJson(),
            "/v1/rpc",false,{{"Authorization","Bearer fixture"}}).status;};
        QCOMPARE(probe(prefix+"small"),429);
        QCOMPARE(probe((control?QString("work/"):QString("control/"))+"small"),200);
        stalled.abort();QTRY_COMPARE(probe(prefix+"small"),200);
    }
    void structuredToolsRoundTripAndSse()
    {
        Fixture fixture; HttpApiServer http(*fixture.service); QVERIFY(http.listen());
        auto value = body();
        value["tools"] = QJsonArray{QJsonObject{{"type", "function"}, {"function", QJsonObject{{"name", "app_lookup"},
            {"description", "Read an app value"}, {"parameters", QJsonObject{{"type", "object"}}}}}}};
        value["tool_choice"] = "required"; value["parallel_tool_calls"] = false;
        const auto first = post(http.port(), value); QCOMPARE(first.status, 200);
        const auto choice = object(first)["choices"].toArray().first().toObject();
        QCOMPARE(choice["finish_reason"].toString(), "tool_calls");
        const auto message = choice["message"].toObject(); QVERIFY(message["content"].isNull());
        const auto call = message["tool_calls"].toArray().first().toObject();
        QCOMPARE(call["id"].toString(), "app-call"); QCOMPARE(call["function"].toObject()["arguments"].toString(), "{\"key\":\"value\"}");
        { std::lock_guard lock(fixture.probe->mutex);
            QCOMPARE(fixture.probe->conversation.toolChoice, "required"); QVERIFY(!fixture.probe->conversation.parallelToolCalls); }
        auto history = value["messages"].toArray(); history.append(message);
        history.append(QJsonObject{{"role", "tool"}, {"tool_call_id", "app-call"}, {"content", "actual app value"}});
        auto followup = value; followup["messages"] = history; followup["tool_choice"] = "auto";
        const auto second = post(http.port(), followup); QCOMPARE(second.status, 200);
        QCOMPARE(object(second)["choices"].toArray().first().toObject()["message"].toObject()["content"].toString(), "actual app value");
        value["stream"] = true; value["stream_options"] = QJsonObject{{"include_usage", true}};
        const auto stream = post(http.port(), value); QCOMPARE(stream.status, 200);
        QVERIFY(stream.bytes.endsWith("data: [DONE]\n\n"));
        int toolChunks = 0, terminals = 0;
        for (const auto& event : events(stream)) for (const auto& item : event["choices"].toArray()) {
            const auto part = item.toObject(); const auto calls = part["delta"].toObject()["tool_calls"].toArray();
            if (!calls.isEmpty()) {
                ++toolChunks; QCOMPARE(calls.first().toObject()["index"].toInt(), 0); QCOMPARE(calls.first().toObject()["id"].toString(), "app-call");
            }
            if (part["finish_reason"] == "tool_calls") ++terminals;
        }
        QCOMPARE(toolChunks, 1); QCOMPARE(terminals, 1);
        QCOMPARE(events(stream).last()["usage"].toObject()["completion_tokens"].toInt(), 5);
        QCOMPARE(fixture.service->stats().get().sessions, 0);
    }
    void automaticResidencyAndKeepAlive()
    {
        Fixture fixture; HttpApiServer http(*fixture.service); QVERIFY(http.listen());
        fixture.service->unloadModel("model://test").get();
        auto value = body(); value["keep_alive"] = "5m";
        QCOMPARE(post(http.port(), value).status, 200);
        QCOMPARE(fixture.service->models().get().size(), 1);
        QCOMPARE(fixture.service->models().get().first().keepAliveMs, qint64(300000));
        const auto before = fixture.service->stats().get().modelLoads;
        QCOMPARE(post(http.port(), value).status, 200);
        QCOMPARE(fixture.service->stats().get().modelLoads, before);
        value["keep_alive"] = 0;
        QCOMPARE(post(http.port(), value).status, 200);
        QVERIFY(fixture.service->models().get().isEmpty());
        QCOMPARE(fixture.service->stats().get().cachedContexts, 0);
        for (const auto& invalid : {QJsonValue(-1), QJsonValue("forever"), QJsonValue(QJsonValue::Null)}) {
            value["keep_alive"] = invalid; QCOMPARE(post(http.port(), value).status, 400);
        }
        QCOMPARE(fixture.service->stats().get().modelLoads, before);
    }
    void jsonConversationAndDiscovery()
    {
        Fixture fixture; HttpApiServer http(*fixture.service); QVERIFY(http.listen());
        QCOMPARE(object(request(http.port(), {}, "/health", true)).value("status").toString(), QStringLiteral("ok"));
        const auto models = object(request(http.port(), {}, "/v1/models", true)).value("data").toArray();
        QCOMPARE(models.size(), 1); QCOMPARE(models[0].toObject().value("id").toString(), QStringLiteral("model://test"));
        auto value = body();
        value["messages"] = QJsonArray{QJsonObject{{"role", "system"}, {"content", "concise"}}, QJsonObject{{"role", "user"}, {"content", "old"}},
            QJsonObject{{"role", "assistant"}, {"content", "previous"}}, QJsonObject{{"role", "user"}, {"content", "latest"}}};
        const auto reply = post(http.port(), value); QCOMPARE(reply.status, 200);
        const auto result = object(reply);
        QCOMPARE(result.value("object").toString(), QStringLiteral("chat.completion"));
        QCOMPARE(result.value("choices").toArray()[0].toObject().value("message").toObject().value("content").toString(), QStringLiteral("안녕 🌍"));
        QCOMPARE(result.value("usage").toObject().value("prompt_tokens").toInt(), 16);
        { std::lock_guard lock(fixture.probe->mutex); QCOMPARE(fixture.probe->messages.size(), 4); QCOMPARE(fixture.probe->messages[2].content, QStringLiteral("previous")); }
        QCOMPARE(fixture.service->stats().get().sessions, 0); QCOMPARE(fixture.service->stats().get().cachedContexts, 0);
        const auto second = post(http.port(), body("fresh")); QCOMPARE(second.status, 200);
        { std::lock_guard lock(fixture.probe->mutex); QCOMPARE(fixture.probe->messages.size(), 1); }
        http.close(); QCOMPARE(http.port(), quint16(0)); QVERIFY(http.listen());
    }
    void sseStreamsUnicodeUsageAndStop()
    {
        Fixture fixture; HttpApiServer http(*fixture.service); QVERIFY(http.listen());
        auto value = body("slow", true); value["max_tokens"] = 5; value["stream_options"] = QJsonObject{{"include_usage", true}};
        int fragments = 0;
        const auto reply = request(http.port(), QJsonDocument(value).toJson(), "/v1/chat/completions", false, {},
            [&](auto*, const auto&) { ++fragments; });
        QCOMPARE(reply.status, 200); QVERIFY(reply.contentType.startsWith("text/event-stream")); QVERIFY(fragments > 1);
        QVERIFY(reply.bytes.endsWith("data: [DONE]\n\n")); QCOMPARE(reply.bytes.count("data: [DONE]"), 1);
        const auto chunks = events(reply); QCOMPARE(chunks.first().value("object").toString(), QStringLiteral("chat.completion.chunk"));
        QCOMPARE(chunks.first().value("choices").toArray()[0].toObject().value("delta").toObject().value("role").toString(), QStringLiteral("assistant"));
        QVERIFY(chunks.first().value("usage").isNull());
        QCOMPARE(chunks.last().value("usage").toObject().value("completion_tokens").toInt(), 5);
        value = body("hello", true); value["stop"] = QStringLiteral("안녕");
        const auto stopped = events(post(http.port(), value)); QString text;
        for (const auto& chunk : stopped) for (const auto& choice : chunk.value("choices").toArray()) text += choice.toObject().value("delta").toObject().value("content").toString();
        QVERIFY(text.isEmpty()); QCOMPARE(fixture.service->stats().get().cachedContexts, 0);
    }
    void validationAndHttpBoundaries()
    {
        Fixture fixture; HttpOptions options; options.maxRequestBytes = 2048; HttpApiServer http(*fixture.service, options); QVERIFY(http.listen());
        QCOMPARE(request(http.port(), "{").status, 400);
        QCOMPARE(request(http.port(), QByteArray(3000, 'x')).status, 413);
        QCOMPARE(request(http.port(), "{}", "/missing").status, 404);
        QCOMPARE(request(http.port(), "{}", "/v1/chat/completions", false, {{"Content-Type", "text/plain"}}).status, 415);
        QCOMPARE(request(http.port(), "{}", "/v1/chat/completions", false, {{"Origin", "https://example.com"}}).status, 403);
        QCOMPARE(request(http.port(), "{}", "/v1/chat/completions", false, {{"Host", "rebind.example"}}).status, 403);
        for (const auto& patch : QList<QJsonObject>{{{"model", "/model.gguf"}}, {{"runtime", "mlx"}}, {{"n", 2}}, {{"tools", QJsonObject{}}},
            {{"tool_choice", "invalid"}}, {{"tool_choice", "required"}}, {{"parallel_tool_calls", "false"}},
            {{"stream", 1}}, {{"max_tokens", 3.5}}, {{"max_tokens", 0}}, {{"max_tokens", 4}, {"max_completion_tokens", 4}},
            {{"min_p",1.1}}, {{"frequency_penalty","bad"}}, {{"logit_bias",QJsonObject{{"bad",1}}}},
            {{"messages", QJsonArray{QJsonObject{{"role", "assistant"}, {"content", "invalid end"}}}}},
            {{"messages", QJsonArray{QJsonObject{{"role", "user"}, {"content", QJsonArray{}}}}}}}) {
            auto value = body(); for (auto it = patch.begin(); it != patch.end(); ++it) value[it.key()] = it.value();
            const auto reply = post(http.port(), value); QCOMPARE(reply.status, 400); QVERIFY(object(reply).contains("error"));
        }
        auto missing = body(); missing["model"] = "model://missing";
        QCOMPARE(post(http.port(), missing).status, 404);
        QCOMPARE(fixture.probe->entered.load(), 0); QCOMPARE(fixture.service->stats().get().sessions, 0);
        QTcpServer occupied; QVERIFY(occupied.listen(QHostAddress::LocalHost));
        HttpApiServer conflict(*fixture.service); QVERIFY(!conflict.listen(occupied.serverPort())); QVERIFY(!conflict.errorString().isEmpty());
    }
    void detailedGenerationParametersReachRuntime()
    {
        Fixture fixture; HttpApiServer http(*fixture.service); QVERIFY(http.listen());
        auto value = body();
        value.insert("min_p",.05); value.insert("repetition_penalty",1.1);
        value.insert("presence_penalty",.2); value.insert("frequency_penalty",.3);
        value.insert("logit_bias",QJsonObject{{"42",-5}});
        QCOMPARE(post(http.port(),value).status,200);
        std::lock_guard lock(fixture.probe->mutex);
        QCOMPARE(fixture.probe->options.minP,.05);
        QCOMPARE(fixture.probe->options.repetitionPenalty,1.1);
        QCOMPARE(fixture.probe->options.presencePenalty,.2);
        QCOMPARE(fixture.probe->options.frequencyPenalty,.3);
        QCOMPARE(fixture.probe->options.logitBias,QJsonObject({{"42",-5}}));
    }
    void runtimeFailureDisconnectAndCleanup()
    {
        Fixture fixture; HttpApiServer http(*fixture.service); QVERIFY(http.listen());
        const auto failed = post(http.port(), body("fail")); QCOMPARE(failed.status, 500);
        const auto stream = post(http.port(), body("fail", true)); QCOMPARE(stream.status, 200);
        const auto chunks = events(stream); QVERIFY(chunks.last().contains("error")); QVERIFY(stream.bytes.endsWith("data: [DONE]\n\n"));
        request(http.port(), QJsonDocument(body("slow", true)).toJson(), "/v1/chat/completions", false, {},
            [](auto* reply, const auto&) { reply->abort(); });
        QTRY_VERIFY_WITH_TIMEOUT(fixture.probe->cancelled.load() > 0, 3000);
        QCOMPARE(fixture.service->stats().get().sessions, 0); QCOMPARE(fixture.service->stats().get().cachedContexts, 0);
        QCOMPARE(post(http.port(), body()).status, 200);
    }
    void queueLimitsDeadlineAndSharedScheduler()
    {
        Fixture fixture(1); const auto session = fixture.service->createSession("model://test").get();
        auto native = fixture.service->chat({session, "slow", {}});
        QTRY_VERIFY(fixture.probe->entered.load() == 1);
        auto queued = fixture.service->stats();
        HttpApiServer http(*fixture.service); QVERIFY(http.listen());
        const auto full = post(http.port(), body()); QCOMPARE(full.status, 429);
        QCOMPARE(object(full).value("error").toObject().value("code").toString(), QStringLiteral("queue_full"));
        native.cancel(); (void)native.result.get(); (void)queued.get();
        HttpOptions options; options.requestTimeoutMs = 80; HttpApiServer deadline(*fixture.service, options); QVERIFY(deadline.listen());
        native = fixture.service->chat({session, "slow", {}}); QTRY_VERIFY(fixture.probe->entered.load() == 2);
        QCOMPARE(post(deadline.port(), body()).status, 504);
        native.cancel(); (void)native.result.get();
        // The timed-out HTTP job is cancelled but may still occupy the sole queue slot
        // until the scheduler observes it. Wait for admission before asserting cleanup.
        const auto drained = [&] {
            try { (void)fixture.service->stats().get(); return true; }
            catch (const Error& error) { if (error.code() == ErrorCode::QueueFull) return false; throw; }
        };
        QTRY_VERIFY(drained());
        fixture.service->closeSession(session).get(); QCOMPARE(fixture.service->stats().get().sessions, 0);
    }
    void outputLimitAndServerShutdown()
    {
        Fixture fixture; HttpOptions options; options.maxBufferedOutputBytes = 1024; HttpApiServer http(*fixture.service, options); QVERIFY(http.listen());
        QCOMPARE(post(http.port(), body("large")).status, 429);
        QCOMPARE(fixture.service->stats().get().sessions, 0);
        QNetworkAccessManager network;
        QNetworkRequest request(QUrl(QStringLiteral("http://127.0.0.1:%1/v1/chat/completions").arg(http.port())));
        request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
        auto* reply = network.post(request, QJsonDocument(body("slow", true)).toJson());
        QTRY_VERIFY(fixture.probe->entered.load() >= 2);
        http.close(); reply->abort();
        QCOMPARE(fixture.service->stats().get().sessions, 0); QCOMPARE(fixture.service->stats().get().cachedContexts, 0);
        QVERIFY(fixture.probe->cancelled.load() > 0);
    }
};
QTEST_GUILESS_MAIN(HttpTests)
#include "http_tests.moc"
