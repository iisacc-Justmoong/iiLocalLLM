#include <agent/WebFetch.h>
#include <agent/Tools.h>
#include <agent/Engine.h>
#include <agent/Api.h>
#include <agent/McpServer.h>
#include <QtCore/QJsonDocument>
#include <QtCore/QTemporaryDir>
#include <QtCore/QThread>
#include <QtTest/QTest>
#include "../third_party/cpp-httplib/httplib.h"
#include <atomic>
#include <mutex>
#include <thread>
using namespace iiLocalLLM;
namespace a=iiLocalLLM::agent;
namespace {
struct Endpoint {
    httplib::Server server; std::thread thread; int port;
    std::atomic<int> calls=0;std::mutex mutex;
    std::function<void(const httplib::Request&,httplib::Response&)> handler;
    Endpoint() {
        server.Get(".*",[this](const auto& req,auto& res){++calls;std::lock_guard lock(mutex);
            if(handler)handler(req,res);else res.set_content("<h1>문서</h1><p>내용 &amp; facts</p>","text/html; charset=utf-8");});
        port=server.bind_to_any_port("127.0.0.1");if(port<1)throw std::runtime_error("Cannot bind web fixture");
        thread=std::thread([this]{server.listen_after_bind();});server.wait_until_ready();
    }
    ~Endpoint(){server.stop();thread.join();}
    QString origin()const{return QString("http://127.0.0.1:%1").arg(port);}
    QString url(QString path="/page")const{return origin()+path;}
    void set(std::function<void(const httplib::Request&,httplib::Response&)> f){std::lock_guard lock(mutex);handler=std::move(f);}
};
class Model final:public a::Model {
public:
    a::ModelRequest request;std::atomic<int> calls=0;bool bad=false,slow=false;int budget=0;QString pageUrl;
    a::ModelReply generate(const a::ModelRequest& r,const CancellationToken& t,const std::function<bool(const QString&)>&)override {
        if(!r.systemPromptOnly&&!pageUrl.isEmpty()) {
            if(r.messages.last().role==a::MessageRole::Tool)return {"ANSWER "+r.messages.last().text,{},{10,5}};
            return {{},{{"fetch","WebFetch",{{"url",pageUrl},{"prompt","Read facts"}}}},{20,9}};
        }
        request=r;++calls;while(slow&&!t.isCancelled())QThread::msleep(1);t.throwIfCancelled();
        if(bad)return {{},{{"forbidden","Bash",{{"command","touch bad"}}}}};
        const auto p=QJsonDocument::fromJson(r.messages.last().text.toUtf8()).object();
        return {p["prompt"].toString()+"\n"+p["content"].toString(),{},Usage{40,7,4}};
    }
    std::optional<ContextBudget> measure(const a::ModelRequest& r,const CancellationToken&)override {if(!budget)return std::nullopt;return ContextBudget{r.messages.last().text.size(),budget};}
};
a::WebFetchOptions options(const Endpoint& e){a::WebFetchOptions o;o.privateOrigins={e.origin()};o.model="fixture";return o;}
a::ToolContext context(QString id="owner"){a::ToolContext c;c.sessionId=id;return c;}
QJsonObject args(const QString& url,QString prompt="Extract facts"){return {{"url",url},{"prompt",prompt}};}
}
class WebFetchTests:public QObject {
    Q_OBJECT
private slots:
    void htmlIsParsedAndSummarizerHasNoAmbientHistoryOrTools() {
        Endpoint e;e.set([](const auto& req,auto& res){
            if(req.has_header("Cookie")||req.has_header("Authorization"))throw std::runtime_error("Leaked credentials");
            res.set_content("<title>Hidden title</title><script>STEAL</script><style>HIDDEN</style><h2>제목 &amp; 자료</h2>"
                "<p>WEB_TEST_42 Hello <strong>bold</strong> <em>word</em><a href='/next?q=1&amp;x=2'>Link</a><img alt='Diagram' src='/pic.png'>"
                "<ul><li>첫째<li>둘째</ul><pre><code>if (a &lt; b) {\n  x++;\n}</code></pre>"
                "<table><tr><th>A<th>B<tr><td>one<td>two</table>","text/html; charset=utf-8");});
        auto model=std::make_shared<Model>();a::WebFetch fetch(model,options(e));auto c=context();
        auto parent=std::make_shared<a::Session>();parent->model="parent";parent->systemPrompt="PRIVATE_SYSTEM";
        parent->messages.append({{},a::MessageRole::User,"PRIVATE_MESSAGE"});c.sessionSnapshot=parent;
        auto result=fetch.fetch(args(e.url()),c);QVERIFY(!result.isError);const auto text=result.data["result"].toString();
        QVERIFY(text.contains("WEB_TEST_42"));QVERIFY(text.contains("## 제목 & 자료"));QVERIFY(text.contains("**bold**"));QVERIFY(text.contains("*word*"));
        QVERIFY(text.contains("[Link]("+e.origin()+"/next?q=1&x=2)"));QVERIFY(text.contains("Diagram"));
        QVERIFY(text.contains("- 첫째"));QVERIFY(text.contains("if (a < b)"));QVERIFY(text.contains("one"));
        QVERIFY(!text.contains("STEAL")&&!text.contains("HIDDEN"));QVERIFY(model->request.tools.isEmpty());
        QCOMPARE(model->request.messages.size(),1);QVERIFY(!model->request.messages.first().text.contains("PRIVATE_"));
        QVERIFY(model->request.systemPromptOnly);QCOMPARE(model->request.toolChoice,"none");
        QCOMPARE(model->request.enableThinking,std::optional<bool>(false));QCOMPARE(result.data["usage"].toObject()["generated_tokens"].toInt(),7);
    }
    void cacheStoresContentAndIsBoundedByOwnerTtlAndBytes() {
        Endpoint e;auto model=std::make_shared<Model>();auto o=options(e);o.cacheTtlMs=40;o.maxCacheEntries=1;
        a::WebFetch fetch(model,o);auto c=context();
        QVERIFY(!fetch.fetch(args(e.url(),"FIRST"),c).data["cached"].toBool());
        auto second=fetch.fetch(args(e.url(),"SECOND"),c);QVERIFY(second.data["cached"].toBool());
        QVERIFY(second.text.contains("SECOND"));QCOMPARE(e.calls.load(),1);QCOMPARE(model->calls.load(),2);
        fetch.fetch(args(e.url()),context("other"));QCOMPARE(e.calls.load(),2);
        fetch.fetch(args(e.url()),c);QCOMPARE(e.calls.load(),3);
        QThread::msleep(55);fetch.fetch(args(e.url()),c);QCOMPARE(e.calls.load(),4);
        fetch.clearCache();fetch.fetch(args(e.url()),c);QCOMPARE(e.calls.load(),5);
        o.maxCacheBytes=1;a::WebFetch tiny(model,o);tiny.fetch(args(e.url()),c);tiny.fetch(args(e.url()),c);QCOMPARE(e.calls.load(),7);
    }
    void redirectsDoNotGrantANewOriginOrRetryErrors() {
        Endpoint e,other;auto model=std::make_shared<Model>();auto o=options(e);o.privateOrigins.append(other.origin());o.maxRedirects=2;
        e.set([&](const auto& req,auto& res){if(req.path=="/same")res.set_redirect("/page",303);
            else if(req.path=="/cross")res.set_redirect(other.url().toStdString(),302);
            else if(req.path=="/loop")res.set_redirect("/loop",307);
            else if(req.path=="/bad"){res.status=503;res.set_content("broken","text/plain");}
            else res.set_content("FINAL","text/plain");});
        a::WebFetch fetch(model,o);auto c=context();
        auto same=fetch.fetch(args(e.url("/same")),c);QVERIFY(same.text.contains("FINAL"));QCOMPARE(same.data["final_url"].toString(),e.url());
        auto cross=fetch.fetch(args(e.url("/cross")),c);QCOMPARE(cross.data["redirect_url"].toString(),other.url());
        QCOMPARE(other.calls.load(),0);QCOMPARE(model->calls.load(),1);
        QVERIFY_THROWS_EXCEPTION(Error,fetch.fetch(args(e.url("/loop")),c));
        const auto before=e.calls.load();for(int i=0;i<2;++i)QVERIFY_THROWS_EXCEPTION(Error,fetch.fetch(args(e.url("/bad")),c));QCOMPARE(e.calls.load(),before+2);
    }
    void urlsPrivateNetworksAndExactDomainRulesAreCheckedBeforeIo() {
        Endpoint e;auto model=std::make_shared<Model>();a::WebFetchOptions o;o.model="fixture";a::WebFetch fetch(model,o);
        for(const QString& url:{e.url(),QString("file:///etc/passwd"),QString("https://user:pw@example.com"),QString("https://[::1]/"),QString("https://169.254.169.254/"),QString("https://192.168.1.1/"),QString("https://localhost/")})
            QVERIFY_THROWS_EXCEPTION(Error,fetch.fetch(args(url),context()));
        QCOMPARE(e.calls.load(),0);QCOMPARE(model->calls.load(),0);
        for(const auto& host:QStringList{"[::ffff:127.0.0.1]","[2002:7f00:1::]","[2001::1]","[2001:db8::1]","224.0.0.1","100.64.0.1","0.0.0.0"}) {
            try{fetch.fetch(args("https://"+host+"/"),context());QFAIL("Reserved address accepted");}catch(const Error& error){QCOMPARE(error.code(),ErrorCode::Unauthorized);}
        }
        a::WebFetch allowed(model,options(e));auto registry=std::make_shared<a::ToolRegistry>();registry->add(allowed.tool());
        auto input=args(e.url());auto definition=allowed.tool().definition;
        QCOMPARE(a::RulePolicy().decide(definition,input,context()).behavior,a::PermissionBehavior::Ask);
        a::RulePolicy rule(a::PermissionMode::Default,{{"WebFetch(domain:127.0.0.1)",a::PermissionBehavior::Allow}});
        QCOMPARE(rule.decide(definition,input,context()).behavior,a::PermissionBehavior::Allow);
        QCOMPARE(rule.decide(definition,args("https://127.0.0.1.example.org/"),context()).behavior,a::PermissionBehavior::Ask);
        a::ToolRunner denied(registry,std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass,QList<a::PermissionRule>{{"WebFetch(domain:127.0.0.1)",a::PermissionBehavior::Deny}}));
        QVERIFY(denied.run({"call","WebFetch",input},context()).isError);QCOMPARE(e.calls.load(),0);
        a::ToolRunner yes(registry,std::make_shared<a::RulePolicy>(rule));QVERIFY(!yes.run({"call","WebFetch",input},context()).isError);
        QVERIFY_THROWS_EXCEPTION(Error,registry->validateInput("WebFetch",{{"url",e.url()},{"prompt","x"},{"headers",QJsonObject{}}}));
    }
    void bytesTimeoutCancellationAndModelFailuresAreBounded() {
        Endpoint e;auto model=std::make_shared<Model>();auto o=options(e);o.maxContentBytes=1024;o.timeoutMs=50;
        e.set([](const auto& req,auto& res){if(req.path=="/slow")QThread::msleep(150);res.set_content(std::string(2048,'a'),"text/plain");});
        a::WebFetch fetch(model,o);QVERIFY_THROWS_EXCEPTION(Error,fetch.fetch(args(e.url()),context()));
        QVERIFY_THROWS_EXCEPTION(Error,fetch.fetch(args(e.url("/slow")),context()));
        auto c=context();c.cancellation.cancel();QVERIFY_THROWS_EXCEPTION(Error,fetch.fetch(args(e.url()),c));QCOMPARE(model->calls.load(),0);
        e.set([](const auto&,auto& res){res.set_content("fine","text/plain");});o.timeoutMs=2000;model->bad=true;a::WebFetch invalid(model,o);
        QVERIFY_THROWS_EXCEPTION(Error,invalid.fetch(args(e.url()),context()));
        model->bad=false;model->slow=true;o.summaryTimeoutMs=25;a::WebFetch slow(model,o);
        QVERIFY_THROWS_EXCEPTION(Error,slow.fetch(args(e.url()),context()));
    }
    void binaryFilesAreOwnedArtifactsAndNeverDecodedAsPageText() {
        Endpoint e;e.set([](const auto&,auto& res){res.set_content(std::string("%PDF-1.7\0BINARY",15),"application/pdf");});
        auto model=std::make_shared<Model>();a::WebFetch fetch(model,options(e));QTemporaryDir root{QDir::current().filePath("web-artifact-XXXXXX")};
        auto c=context();c.artifactsDirectory=root.filePath("owner");auto r=fetch.fetch(args(e.url()),c);
        const auto path=r.data["artifact"].toObject()["path"].toString();QVERIFY(path.startsWith(c.artifactsDirectory+'/'));
        QFile file(path);QVERIFY(file.open(QIODevice::ReadOnly));QCOMPARE(file.readAll(),QByteArray("%PDF-1.7\0BINARY",15));QCOMPARE(model->calls.load(),0);
        QVERIFY_THROWS_EXCEPTION(Error,fetch.fetch(args(e.url()),context()));
    }
    void encodingTruncationAndNativeContextBudgetAreHonored() {
        Endpoint e;e.set([](const auto&,auto& res){res.set_content(std::string("caf\xe9 \x80",6),"text/plain; charset=windows-1252");});
        auto model=std::make_shared<Model>();auto o=options(e);a::WebFetch fetch(model,o);
        QVERIFY(fetch.fetch(args(e.url()),context()).text.contains(QString::fromUtf8("café €")));
        e.set([](const auto&,auto& res){res.set_content((QString(3000,'x')+QString::fromUtf8("😀")).toStdString(),"text/plain");});
        model->budget=1500;a::WebFetch bounded(model,o);auto r=bounded.fetch(args(e.url()),context());
        QVERIFY(r.data["truncated"].toBool());QVERIFY(model->request.messages.last().text.size()+model->request.generation.maxTokens+64<=1500);
        model->budget=0;o.maxMarkdownCharacters=3001;a::WebFetch unicode(model,o);r=unicode.fetch(args(e.url()),context());
        const auto content=QJsonDocument::fromJson(model->request.messages.last().text.toUtf8()).object()["content"].toString();
        QCOMPARE(content.size(),3000);QVERIFY(r.data["truncated"].toBool());
        o.maxOutputBytes=10;a::WebFetch output(model,o);QVERIFY_THROWS_EXCEPTION(Error,output.fetch(args(e.url()),context()));
    }
    void htmlResourceLimitsCancellationAndConcurrentAdmission() {
        Endpoint e;auto model=std::make_shared<Model>();auto o=options(e);o.maxConcurrentFetches=1;a::WebFetch fetch(model,o);
        e.set([](const auto&,auto& res){res.set_content(QByteArray("<p>x</p>").repeated(70000).toStdString(),"text/html");});
        QVERIFY_THROWS_EXCEPTION(Error,fetch.fetch(args(e.url("/large-tree")),context()));QCOMPARE(model->calls.load(),0);
        e.set([](const auto&,auto& res){res.set_content((QByteArray("<div>").repeated(300)+"x"+QByteArray("</div>").repeated(300)).toStdString(),"text/html");});
        QVERIFY_THROWS_EXCEPTION(Error,fetch.fetch(args(e.url("/deep-tree")),context()));QCOMPARE(model->calls.load(),0);
        e.set([](const auto&,auto& res){res.set_content("<meta charset=windows-1252><p>caf\xe9</p>","text/html");});
        QVERIFY(fetch.fetch(args(e.url("/meta")),context()).text.contains(QString::fromUtf8("café")));
        e.set([](const auto&,auto& res){res.set_content("SAFE","text/plain");});model->slow=true;auto c=context();
        auto running=std::async(std::launch::async,[&]{return fetch.fetch(args(e.url()),c);});QTRY_COMPARE_WITH_TIMEOUT(model->calls.load(),2,3000);
        QVERIFY_THROWS_EXCEPTION(Error,fetch.fetch(args(e.url()),context("other")));c.cancellation.cancel();QVERIFY_THROWS_EXCEPTION(Error,running.get());
        model->slow=false;QVERIFY(!fetch.fetch(args(e.url()),context()).isError);
    }
    void engineOffersNativeToolAndKeepsExtractionUsageSeparate() {
        Endpoint e;QTemporaryDir root{QDir::current().filePath("web-engine-XXXXXX")};const auto work=root.filePath("work");QDir().mkpath(work);
        auto model=std::make_shared<Model>();model->pageUrl=e.url();auto registry=std::make_shared<a::ToolRegistry>();
        auto policy=std::make_shared<a::RulePolicy>(a::PermissionMode::Default,QList<a::PermissionRule>{{"WebFetch(domain:127.0.0.1)",a::PermissionBehavior::Allow}});
        a::EngineOptions o;o.sessionsDirectory=root.filePath("sessions");o.webFetchEnabled=true;o.webFetch=options(e);o.webFetch.deferred=false;
        o.skills.enabled=false;o.projectContext.enabled=false;o.compaction.automatic=false;o.toolSearch.enabled=false;
        a::Engine engine(model,registry,policy,o);auto id=engine.createSession("fixture",work).id;
        const auto result=engine.run({id,"Read the web page"}).result.get();QCOMPARE(result.status,a::RunStatus::Completed);
        QVERIFY(result.text.contains("내용 & facts"));QCOMPARE(result.usage.generatedTokens,14);QCOMPARE(engine.session(id).messages.size(),4);
        QCOMPARE(engine.session(id).messages[2].data["usage"].toObject()["generated_tokens"].toInt(),7);
        model->slow=true;auto future=std::async(std::launch::async,[&]{return engine.runWebFetch(id,args(e.url()));});
        QTRY_COMPARE_WITH_TIMEOUT(model->calls.load(),2,3000);engine.endSession(id);QVERIFY_THROWS_EXCEPTION(Error,future.get());
        o.sessionsDirectory=root.filePath("disabled");o.webFetchEnabled=false;a::Engine disabled(model,registry,policy,o);QVERIFY(!disabled.webFetchTool());
    }
    void authenticatedApiAndMcpBindTheSessionAndExposeCapability() {
        Endpoint e;QTemporaryDir root{QDir::current().filePath("web-bridges-XXXXXX")};const auto work=root.filePath("work");QDir().mkpath(work);
        auto model=std::make_shared<Model>();auto registry=std::make_shared<a::ToolRegistry>();
        auto policy=std::make_shared<a::RulePolicy>(a::PermissionMode::Default,QList<a::PermissionRule>{{"WebFetch(domain:127.0.0.1)",a::PermissionBehavior::Allow}});
        a::EngineOptions o;o.webFetchEnabled=true;o.webFetch=options(e);o.webFetch.model.clear();o.skills.enabled=false;o.projectContext.enabled=false;
        a::ApiOptions apiOptions;apiOptions.workingDirectory=work;apiOptions.stateDirectory=root.filePath("api");apiOptions.engine=o;
        apiOptions.clientTokens={{"society",QString(48,'a')},{"dreamscapes",QString(48,'b')}};a::Api api(model,registry,policy,apiOptions);
        auto call=[&](QString method,QJsonObject params={},QString token=QString(48,'a')){return api.dispatch(method,params,token).result.get().toObject();};
        QVERIFY(call("agent.info")["web_fetch_enabled"].toBool());const auto id=call("agent.sessions.create",{{"model","fixture"}})["session_id"].toString();
        auto params=args(e.url());params["session_id"]=id;const auto result=call("agent.web.fetch",params);QVERIFY2(!result["is_error"].toBool(),qPrintable(result["text"].toString()));
        QCOMPARE(model->request.model,"fixture");QCOMPARE(call("agent.sessions.get",{{"session_id",id}})["message_count"].toInt(),0);
        QVERIFY_THROWS_EXCEPTION(Error,call("agent.web.fetch",params,QString(48,'b')));const auto count=e.calls.load();
        params["privateOrigins"]=QJsonArray{e.origin()};QVERIFY(call("agent.web.fetch",params)["is_error"].toBool());QCOMPARE(e.calls.load(),count);
        a::Tool mark;mark.definition.name="Mark";mark.definition.inputSchema={{"type","object"},{"additionalProperties",false}};
        mark.execute=[](const QJsonObject&,const a::ToolContext&){return a::ToolResult{"MARKED",{{"marked",true}}};};registry->add(mark);
        policy=std::make_shared<a::RulePolicy>(a::PermissionMode::Default,QList<a::PermissionRule>{{"WebFetch(domain:127.0.0.1)",a::PermissionBehavior::Allow},{"Mark",a::PermissionBehavior::Allow}});
        o.sessionsDirectory=root.filePath("mcp");auto engine=std::make_shared<a::Engine>(model,registry,policy,o);
        a::McpServerOptions mo;mo.engine=engine;mo.model="fixture";mo.workingDirectory=work;
        iiLocalLLM::mcp::ServerSession server(a::mcpServerOptions(registry,policy,mo));
        auto rpc=[&](int id,QString method,QJsonObject params){server.receive(QJsonObject{{"jsonrpc","2.0"},{"id",id},{"method",method},{"params",params}});
            for(int i=0;i<400;++i)for(const auto& v:server.takeMessages(10))if(v.toObject()["id"]==id)return v.toObject();throw std::runtime_error("MCP timed out");};
        const auto init=rpc(1,"initialize",{{"protocolVersion","2025-11-25"},{"capabilities",QJsonObject{}},{"clientInfo",QJsonObject{{"name","test"},{"version","1"}}}});
        QVERIFY(QJsonDocument(init).toJson().contains("iisacc/webFetch"));server.receive(QJsonObject{{"jsonrpc","2.0"},{"method","notifications/initialized"}});
        const auto reply=rpc(2,"tools/call",{{"name","WebFetch"},{"arguments",args(e.url())}})["result"].toObject();
        QVERIFY2(!reply["isError"].toBool()&&reply.contains("structuredContent"),QJsonDocument(reply).toJson().constData());
        QCOMPARE(reply["structuredContent"].toObject()["code"].toInt(),200);QCOMPARE(model->request.model,"fixture");
        model->slow=true;
        server.receive(QJsonObject{{"jsonrpc","2.0"},{"id",3},{"method","tools/call"},{"params",QJsonObject{{"name","WebFetch"},{"arguments",args(e.url())}}}});
        QTRY_COMPARE_WITH_TIMEOUT(model->calls.load(),3,3000);
        server.receive(QJsonObject{{"jsonrpc","2.0"},{"id",4},{"method","tools/call"},{"params",QJsonObject{{"name","Mark"},{"arguments",QJsonObject{}}}}});
        bool marked=false;for(int i=0;i<25&&!marked;++i)for(const auto& v:server.takeMessages(10))if(v.toObject()["id"]==4)marked=true;
        server.receive(QJsonObject{{"jsonrpc","2.0"},{"method","notifications/cancelled"},{"params",QJsonObject{{"requestId",3}}}});
        QVERIFY2(marked,"WebFetch held the application execution lock while waiting for its own model");
    }
};
QTEST_GUILESS_MAIN(WebFetchTests)
#include "web_fetch_tests.moc"
