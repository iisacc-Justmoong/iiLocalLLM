#include <agent/Api.h>
#include <HttpApiServer.h>
#include <LocalIpcServer.h>
#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QJsonDocument>
#include <QtCore/QTemporaryDir>
#include <QtNetwork/QLocalSocket>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtTest/QTest>
#include <chrono>
#include <thread>
using namespace iiLocalLLM;
using namespace std::chrono_literals;
namespace a = iiLocalLLM::agent;
namespace {
const QString credential(48, 'a'), otherCredential(48, 'b');
QByteArray bytes(QJsonObject value) { return QJsonDocument(value).toJson(QJsonDocument::Compact); }
class Model final : public a::Model {
public:
    std::atomic_int entered = 0, cancelled = 0;
    a::ModelReply generate(const a::ModelRequest& r, const CancellationToken& token, const TextCallback& delta) override {
        ++entered; const auto prompt = r.messages.last().text;
        if (prompt == "wait") {
            while (!token.isCancelled()) std::this_thread::sleep_for(1ms);
            ++cancelled; token.throwIfCancelled();
        }
        const auto value = prompt == "large" ? QString(8192, 'x') : prompt;
        if (delta) delta(value); return {value, {}, {1, 1, 0, 0}};
    }
};
struct Fixture {
    QTemporaryDir root{QDir::current().filePath("rpc-XXXXXX")};
    std::shared_ptr<Model> model = std::make_shared<Model>();
    std::unique_ptr<Service> service;
    std::shared_ptr<a::Api> api;
    std::unique_ptr<LocalIpcServer> ipc;
    std::unique_ptr<HttpApiServer> http;
    explicit Fixture(int timeout = 3000, int buffer = 65536) {
        ServiceOptions serviceOptions; serviceOptions.modelsDirectory = root.filePath("Models");
        service = std::make_unique<Service>(serviceOptions);
        a::ApiOptions options; options.workingDirectory = root.filePath("work"); QDir().mkpath(options.workingDirectory);
        options.stateDirectory = root.filePath("private"); options.clientTokens = {{"society", credential}, {"dreamscapes", otherCredential}};
        api = std::make_shared<a::Api>(model, std::make_shared<a::ToolRegistry>(), std::make_shared<a::RulePolicy>(), options);
        ipc = std::make_unique<LocalIpcServer>(*service); ipc->setRpcHandler(api);
        if (!ipc->listen(root.filePath("s"))) throw std::runtime_error(ipc->errorString().toStdString());
        HttpOptions h; h.requestTimeoutMs = timeout; h.maxBufferedOutputBytes = buffer;
        http = std::make_unique<HttpApiServer>(*service, h); http->setRpcHandler(api);
        if (!http->listen()) throw std::runtime_error(http->errorString().toStdString());
    }
};
struct Response { int status = 0; QByteArray body, authenticate; };
Response post(Fixture& f, QJsonObject value, QString token = credential,
    std::function<void(QNetworkReply*, const QByteArray&)> received = {}, QByteArray origin = {}) {
    QNetworkAccessManager network;
    QNetworkRequest request(QUrl(QString("http://127.0.0.1:%1/v1/rpc").arg(f.http->port())));
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    if (!token.isEmpty()) request.setRawHeader("Authorization", "Bearer " + token.toUtf8());
    if (!origin.isEmpty()) request.setRawHeader("Origin", origin);
    auto reply = network.post(request, bytes(value)); Response result; QElapsedTimer timer; timer.start();
    QObject::connect(reply, &QNetworkReply::readyRead, [&] { const auto chunk = reply->readAll(); result.body += chunk; if (received) received(reply, chunk); });
    while (!reply->isFinished() && timer.elapsed() < 5000) QTest::qWait(1);
    if (!reply->isFinished()) reply->abort();
    result.body += reply->readAll(); result.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    result.authenticate = reply->rawHeader("WWW-Authenticate"); return result;
}
QJsonObject request(QString method, QJsonObject params = {}, bool stream = false) {
    return {{"id", "client-request"}, {"method", method}, {"params", params}, {"stream", stream}};
}
QJsonObject object(const Response& r) { return QJsonDocument::fromJson(r.body).object(); }
QList<QJsonObject> events(const QByteArray& body) {
    QList<QJsonObject> result;
    for (const auto& line : body.split('\n')) if (line.startsWith("data: ") && line != "data: [DONE]")
        result.append(QJsonDocument::fromJson(line.mid(6)).object());
    return result;
}
class Native {
public:
    QLocalSocket socket; QByteArray input; QList<QJsonObject> frames;
    explicit Native(const Fixture& f) { socket.connectToServer(f.ipc->serverName()); if (!socket.waitForConnected(1000)) throw std::runtime_error("IPC connect failed"); }
    void send(QString id, QString method, QJsonObject params = {}, QString auth = credential) {
        socket.write(bytes({{"id", id}, {"method", method}, {"params", params}, {"auth", auth}}) + '\n'); socket.flush();
    }
    void receive() {
        QTest::qWait(1); input += socket.readAll();
        while (input.contains('\n')) { const auto end = input.indexOf('\n'); frames.append(QJsonDocument::fromJson(input.first(end)).object()); input.remove(0, end + 1); }
    }
    QJsonObject until(QString id, QString field) {
        QElapsedTimer timer; timer.start();
        do { receive(); for (const auto& f : frames) if (f["id"] == id && f.contains(field)) return f; }
        while (timer.elapsed() < 4000 && socket.state() == QLocalSocket::ConnectedState);
        throw std::runtime_error("IPC response did not arrive");
    }
};
}
class AgentTransportTests : public QObject {
    Q_OBJECT
private slots:
    void authSessionsForkAndEventOrder() {
        Fixture f;
        const auto unauth = post(f, request("agent.info"), {}); QCOMPARE(unauth.status, 401); QVERIFY(unauth.authenticate.startsWith("Bearer"));
        QCOMPARE(object(unauth)["error"].toObject()["code"].toString(), "unauthorized");
        QCOMPARE(post(f, request("agent.info"), "invalid").status, 401);
        QCOMPARE(post(f, request("agent.info"), credential, {}, "https://example.com").status, 403);
        auto invalid = request("agent.info"); invalid["auth"] = credential; QCOMPARE(post(f, invalid).status, 400);
        Native client(f); client.send("bad", "agent.info", {}, "wrong");
        QCOMPARE(client.until("bad", "error")["error"].toObject()["code"].toString(), "unauthorized");
        const auto created = post(f, request("agent.sessions.create", {{"model", "fixture"}})); QCOMPARE(created.status, 200);
        const auto id = object(created)["result"].toObject().value("session_id"); QVERIFY(id.isString());
        client.send("run", "agent.run", {{"session_id", id}, {"prompt", "안녕 🌍"}});
        QCOMPARE(client.until("run", "result")["result"].toObject()["text"].toString(), "안녕 🌍");
        QList<QJsonObject> runFrames; for (const auto& frame : client.frames) if (frame["id"] == "run") runFrames.append(frame);
        QCOMPARE(runFrames.first()["event"].toString(), "accepted"); QVERIFY(runFrames.last().contains("result"));
        QCOMPARE(runFrames[runFrames.size() - 2]["data"].toObject()["event"].toString(), "finished");
        const auto get = post(f, request("agent.sessions.get", {{"session_id", id}})); QCOMPARE(get.status, 200);
        QCOMPARE(object(get)["result"].toObject()["message_count"].toInt(), 2);
        QCOMPARE(post(f, request("agent.sessions.get", {{"session_id", id}}), otherCredential).status, 404);
        const auto fork = post(f, request("agent.sessions.fork", {{"session_id", id}})); QCOMPARE(fork.status, 200);
        const auto forkId = object(fork)["result"].toObject().value("session_id"); QVERIFY(forkId != id);
        const auto stream = post(f, request("agent.run", {{"session_id", forkId}, {"prompt", "continued"}}, true));
        QCOMPARE(stream.status, 200); QVERIFY(stream.body.endsWith("data: [DONE]\n\n"));
        const auto frames = events(stream.body); QCOMPARE(frames.first()["event"].toString(), "accepted");
        QCOMPARE(frames.last()["event"].toString(), "done"); QCOMPARE(frames.last()["result"].toObject()["text"].toString(), "continued");
        QVERIFY(!stream.body.contains(credential.toUtf8()));
    }
    void crossTransportCancellationAndDisconnect() {
        Fixture f; Native client(f);
        const auto create = post(f, request("agent.sessions.create", {{"model", "fixture"}}));
        const auto id = object(create)["result"].toObject().value("session_id");
        client.send("run", "agent.run", {{"session_id", id}, {"prompt", "wait"}});
        const auto accepted = client.until("run", "request_id"); QTRY_COMPARE(f.model->entered.load(), 1);
        QCOMPARE(post(f, request("agent.status", {{"request_id", accepted.value("request_id")}}), otherCredential).status, 404);
        const auto cancelled = post(f, request("agent.cancel", {{"request_id", accepted.value("request_id")}})); QCOMPARE(cancelled.status, 200);
        QCOMPARE(client.until("run", "result")["result"].toObject()["status"].toString(), "cancelled");
        bool aborted = false;
        post(f, request("agent.run", {{"session_id", id}, {"prompt", "wait"}}, true), credential,
            [&](QNetworkReply* reply, const QByteArray&) { if (f.model->entered.load() >= 2) { aborted = true; reply->abort(); } });
        QVERIFY(aborted);
        QTRY_VERIFY(f.model->cancelled.load() >= 2);
        client.send("disconnect", "agent.run", {{"session_id", id}, {"prompt", "wait"}});
        client.until("disconnect", "request_id"); QTRY_COMPARE(f.model->entered.load(), 3);
        client.socket.abort(); QTRY_COMPARE(f.model->cancelled.load(), 3);
    }
    void timeoutAndShutdown() {
        Fixture f(100, 2048);
        const auto create = post(f, request("agent.sessions.create", {{"model", "fixture"}}));
        QVERIFY2(create.status == 200, create.body.constData());
        const auto id = object(create)["result"].toObject().value("session_id");
        const auto timed = post(f, request("agent.run", {{"session_id", id}, {"prompt", "wait"}})); QCOMPARE(timed.status, 504);
        QTRY_COMPARE(f.model->cancelled.load(), 1);
        Native client(f); client.send("run", "agent.run", {{"session_id", id}, {"prompt", "wait"}});
        client.until("run", "request_id"); QTRY_COMPARE(f.model->entered.load(), 2);
        f.ipc->close(); f.http->close(); f.api->close(); QCOMPARE(f.model->cancelled.load(), 2);
    }
    void streamOverflowHasIndependentDeadline() {
        Fixture f(3000, 2048);
        const auto create = post(f, request("agent.sessions.create", {{"model", "fixture"}}));
        QVERIFY2(create.status == 200, create.body.constData());
        const auto id = object(create)["result"].toObject().value("session_id");
        const auto large = post(f, request("agent.run", {{"session_id", id}, {"prompt", "large"}}, true));
        const auto frames = events(large.body); QVERIFY(!frames.isEmpty());
        QCOMPARE(frames.last()["error"].toObject()["code"].toString(), "resource_limit");
    }
};
QTEST_GUILESS_MAIN(AgentTransportTests)
#include "agent_transport_tests.moc"
