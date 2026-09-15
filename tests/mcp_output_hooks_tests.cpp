#include <agent/Api.h>
#include <agent/CommandHooks.h>
#include <agent/McpTools.h>
#include <agent/McpServer.h>
#include <mcp/HttpClient.h>
#include <mcp/HttpServer.h>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QTemporaryDir>
#include <QtTest/QtTest>
#include "../third_party/cpp-httplib/httplib.h"
#include <thread>
using namespace iiLocalLLM;
namespace a=iiLocalLLM::agent;
namespace m=iiLocalLLM::mcp;
namespace {
const QString remoteName="mcp__app__observe", original="ORIGINAL_OBSERVATION_83", replacement="교체된 관측 42";
QJsonObject textBlock(const QString& value){return {{"type","text"},{"text",value}};}
QJsonObject schema(){return {{"type","object"},{"properties",QJsonObject{{"value",QJsonObject{{"type","string"}}}}},{"required",QJsonArray{"value"}}};}
m::HttpServerOptions transport() {
    m::HttpServerOptions options;options.authenticate=[](const QByteArray& token){return token=="test-credential"?QString("app"):QString{};};return options;
}
m::HttpOptions connection(const m::HttpServer& server) {
    m::HttpOptions options;options.endpoint=server.endpoint();options.bearerToken=[] {return QByteArray("test-credential");};return options;
}
struct Remote {
    std::atomic_int calls=0;
    m::HttpServer server;
    std::shared_ptr<m::HttpClient> client;
    std::shared_ptr<a::ToolRegistry> registry=std::make_shared<a::ToolRegistry>();
    Remote():server([this](const QString&) {
        m::ServerOptions options;
        options.lists["tools/list"]=[](const auto&) {return QJsonArray{QJsonObject{{"name","observe"},{"inputSchema",schema()},{"outputSchema",schema()}}};};
        options.handlers["tools/call"]=[this](const QJsonObject& p,const auto&) {
            ++calls;const auto value=p["arguments"].toObject()["value"].toString();
            return QJsonObject{{"content",QJsonArray{textBlock(value)}},{"structuredContent",QJsonObject{{"value",value=="bad-output"?QJsonValue(5):QJsonValue(value)}}},
                {"_meta",QJsonObject{{"app","fixture"}}},{"isError",value=="tool-error"}};
        };return options;
    },transport()) {
        if(!server.listen())throw std::runtime_error("MCP fixture failed to listen");
        client=std::make_shared<m::HttpClient>(connection(server));
        for(auto tool:a::mcpTools(client,{"app","com.iisacc.fixture"}))registry->add(std::move(tool));
    }
};
QJsonObject response(const QJsonValue& value,const QString& context={}) {
    QJsonObject specific{{"hookEventName","PostToolUse"},{"updatedMCPToolOutput",value}};
    if(!context.isEmpty())specific["additionalContext"]=context;
    return {{"hookSpecificOutput",specific},{"suppressOutput",true}};
}
QJsonObject settings(QJsonArray hooks,const QString& event="PostToolUse") {
    return {{"hooks",QJsonObject{{event,QJsonArray{QJsonObject{{"hooks",hooks}}}}}}};
}
QString output(const QJsonObject& value) {
    // Qt/macOS normalizes non-ASCII process arguments. JSON escapes test exact
    // stdout decoding without making that unrelated argv behavior the oracle.
    QString json;for(const auto character:QString::fromUtf8(QJsonDocument(value).toJson(QJsonDocument::Compact))) {
        if(character.unicode()>127)json+=QString("\\u%1").arg(character.unicode(),4,16,QChar('0'));else json+=character;
    }
    json.replace("'","'\\''");return "printf '%s\\n' '"+json+"'";
}
QJsonObject command(const QJsonValue& value,const QString& context={}) {return {{"type","command"},{"command",output(response(value,context))}};}
a::CommandHookOptions hookOptions(const QString& root){a::CommandHookOptions value;value.workingDirectory=root;value.environment={};return value;}
std::shared_ptr<a::RulePolicy> policy(){return std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass);}
a::ToolResult run(Remote& remote,const QString& root,const QList<a::Hook>& hooks,const QString& value=original,const a::EventCallback& event={}) {
    return a::ToolRunner(remote.registry,policy(),{hooks}).run({"call",remoteName,{{"value",value}}},{"s","r",root},event);
}
struct HttpHook {
    httplib::Server server;std::thread thread;int port=0;
    QJsonObject request;
    explicit HttpHook(QJsonObject reply) {
        server.Post("/hook",[this,reply](const auto& input,auto& output) {
            request=QJsonDocument::fromJson(QByteArray::fromStdString(input.body)).object();
            output.set_content(QJsonDocument(reply).toJson().toStdString(),"application/json");
        });port=server.bind_to_any_port("127.0.0.1");
        if(port<1)throw std::runtime_error("Hook fixture failed to listen");
        thread=std::thread([this]{server.listen_after_bind();});server.wait_until_ready();
    }
    ~HttpHook(){server.stop();if(thread.joinable())thread.join();}
    QJsonObject hook() const{return {{"type","http"},{"url",QString("http://127.0.0.1:%1/hook").arg(port)}};}
};
struct Observer final:a::Model {
    a::Message observed;
    a::ModelReply generate(const a::ModelRequest& request,const CancellationToken&,const TextCallback&) override {
        if(request.messages.last().role!=a::MessageRole::Tool)return {{},{{"call",remoteName,{{"value",original}}}}};
        observed=request.messages.last();return {observed.text};
    }
};
}
class McpOutputHooksTests final:public QObject {
    Q_OBJECT
private slots:
    void cppArrayPreservesMcpContentWithoutRetainingOldData() {
        QTemporaryDir root;Remote remote;
        const QJsonArray content{textBlock(replacement),QJsonObject{{"type","image"},{"data","AA=="},{"mimeType","image/png"}},
            QJsonObject{{"type","audio"},{"data","AA=="},{"mimeType","audio/wav"}},
            QJsonObject{{"type","resource_link"},{"uri","test://selected"},{"name","selected"}},
            QJsonObject{{"type","resource"},{"resource",QJsonObject{{"uri","test://note"},{"text","embedded note"}}}}};
        const auto hook=[&](const a::HookInput& input,const auto&) {
            a::HookResult result;if(input.kind==a::HookKind::BeforeTool)result.feedback="pre context";
            if(input.kind==a::HookKind::AfterTool) {result.updatedMCPToolOutput=content;result.feedback="post context";}return result;
        };
        const auto result=run(remote,root.path(),{hook});
        QVERIFY(!result.isError);QVERIFY(result.data.isEmpty());QCOMPARE(result.content,content);QCOMPARE(result.metadata["app"],"fixture");
        QCOMPARE(result.text,replacement+"\nResource: selected (test://selected)\ntest://note\nembedded note\npre context\npost context");
        QVERIFY(remote.registry->get(remoteName).isMcp);QVERIFY(remote.registry->snapshot()->get(remoteName).isMcp);
        QVERIFY(!a::toJson(remote.registry->get(remoteName).definition).contains("isMcp"));
    }
    void cppInvalidReplacementRetainsObservationAndBlockDecision() {
        QTemporaryDir root;Remote remote;
        auto hook=[](const a::HookInput& input,const auto&) {
            a::HookResult result;if(input.kind==a::HookKind::AfterTool) {
                result.updatedMCPToolOutput=QJsonObject{{"content",QJsonArray{textBlock("invalid envelope")}}};result.block=true;result.feedback="blocked context";
            }return result;
        };
        QJsonArray diagnostics;const auto result=run(remote,root.path(),{hook},original,[&](const a::Event& event){if(event.kind==a::EventKind::Hook)diagnostics.append(event.data);});
        QVERIFY(result.isError);QCOMPARE(result.data["value"],original);QVERIFY(result.text.endsWith("\nblocked context"));
        QCOMPARE(diagnostics.size(),1);QCOMPARE(diagnostics[0].toObject()["outcome"],"non_blocking_error");
    }
    void apiRunAndSessionExposeTheReplacedObservation() {
        QTemporaryDir root;Remote remote;HttpHook endpoint(response(replacement));
        a::ApiOptions options;options.workingDirectory=root.filePath("workspace");QDir().mkpath(options.workingDirectory);
        options.stateDirectory=root.filePath("private");const QString credential(40,'x');options.clientTokens={{"society",credential}};
        a::CommandHooks hooks(settings({endpoint.hook()}),hookOptions(options.workingDirectory));options.engine.hooks={hooks.callback()};
        auto model=std::make_shared<Observer>();a::Api api(model,remote.registry,policy(),options);
        const auto call=[&](const QString& method,const QJsonObject& params) {return api.dispatch(method,params,credential).result.get().toObject();};
        const auto id=call("agent.sessions.create",{{"model","fixture"}}).value("session_id");
        const auto result=call("agent.run",{{"session_id",id},{"prompt","observe"}});QCOMPARE(result["status"],"completed");QCOMPARE(result["text"],replacement);
        bool found=false;for(const auto& value:call("agent.sessions.get",{{"session_id",id}})["messages"].toArray()) {
            const auto message=value.toObject();if(message["role"]!="tool")continue;
            found=true;QCOMPARE(message["text"],replacement);QVERIFY(message["data"].toObject().isEmpty());
            QCOMPARE(message["content"].toArray(),QJsonArray{textBlock(replacement)});
        }
        QVERIFY(found);QCOMPARE(remote.calls.load(),1);
    }
    void mcpReexportDropsOnlyTheRewritableOutputSchema() {
        QTemporaryDir root;Remote remote;
        a::Tool native;native.definition=remote.registry->get(remoteName).definition;native.definition.name="native";
        native.execute=[](const auto&,const auto&){return a::ToolResult{original,{{"value",original}}};};remote.registry->add(native);
        for(const bool enabled:{false,true}) {
            a::McpServerOptions bridge;bridge.workingDirectory=root.path();
            if(enabled)bridge.tools.hooks={[](const a::HookInput& input,const auto&) {a::HookResult result;
                if(input.kind==a::HookKind::AfterTool)result.updatedMCPToolOutput=replacement;return result;}};
            const auto protocol=a::mcpServerOptions(remote.registry,policy(),bridge);
            m::HttpServer server([protocol](const auto&){return protocol;},transport());QVERIFY(server.listen());m::HttpClient client(connection(server));
            bool imported=false,local=false;
            for(const auto& value:client.listTools()) {
                const auto definition=value.toObject();
                if(definition["name"]==remoteName){imported=true;QCOMPARE(definition.contains("outputSchema"),!enabled);}
                if(definition["name"]=="native"){local=true;QCOMPARE(definition["outputSchema"].toObject(),schema());}
            }
            QVERIFY(imported&&local);
            const auto result=client.callTool(remoteName,{{"value",original}});QVERIFY(!result["isError"].toBool());
            if(enabled){QVERIFY(result["structuredContent"].toObject().isEmpty());QCOMPARE(result["content"].toArray(),QJsonArray{textBlock(replacement)});}
            else QCOMPARE(result["structuredContent"].toObject()["value"],original);
            QCOMPARE(result["_meta"].toObject()["app"],"fixture");
            QVERIFY(client.callTool(remoteName,{{"value","bad-output"}})["isError"].toBool());
            QCOMPARE(client.callTool("native",{{"value",original}})["structuredContent"].toObject()["value"],original);
        }
    }
    void httpReplacementReachesModelTranscriptAndToolFinished() {
        QTemporaryDir root;Remote remote;HttpHook endpoint(response(replacement));
        a::CommandHooks hooks(settings({endpoint.hook()}),hookOptions(root.path()));
        auto model=std::make_shared<Observer>();a::EngineOptions options;options.sessionsDirectory=root.filePath("sessions");options.hooks={hooks.callback()};
        a::Engine engine(model,remote.registry,policy(),options);const auto session=engine.createSession("fixture",root.path());
        a::Event finished;const auto result=engine.run({session.id,"observe"},[&](const a::Event& event){if(event.kind==a::EventKind::ToolFinished)finished=event;}).result.get();
        QCOMPARE(result.status,a::RunStatus::Completed);QCOMPARE(result.text,replacement);QCOMPARE(remote.calls.load(),1);
        QCOMPARE(model->observed.text,replacement);QVERIFY(model->observed.data.isEmpty());QCOMPARE(model->observed.content,QJsonArray{textBlock(replacement)});
        QCOMPARE(model->observed.metadata["app"],"fixture");QCOMPARE(finished.text,replacement);QVERIFY(finished.data["result"].toObject().isEmpty());
        QCOMPARE(finished.data["content"].toArray(),model->observed.content);
        const auto stored=engine.session(session.id);bool found=false;
        for(const auto& message:stored.messages)if(message.role==a::MessageRole::Tool) {
            found=true;QCOMPARE(message.text,replacement);QVERIFY(message.data.isEmpty());QCOMPARE(message.content,model->observed.content);
            QVERIFY(!QJsonDocument(a::toJson(message)).toJson().contains(original.toUtf8()));
        }
        QVERIFY(found);endpoint.server.stop();endpoint.thread.join();
        QCOMPARE(endpoint.request["hook_event_name"],"PostToolUse");
        QCOMPARE(endpoint.request["tool_response"].toObject()["data"].toObject()["value"],original);
    }
#if defined(Q_OS_UNIX) && !defined(Q_OS_IOS) && !defined(Q_OS_ANDROID) && !defined(Q_OS_WASM)
    void wrongEventNonzeroExitAndStopCannotApplyOutput() {
        QTemporaryDir root;Remote remote;
        for(const auto& event:QStringList{"PreToolUse","PostToolUseFailure","Stop","PermissionRequest"}) {
            auto value=response(replacement);auto specific=value["hookSpecificOutput"].toObject();specific["hookEventName"]=event;value["hookSpecificOutput"]=specific;
            a::CommandHooks hooks(settings({QJsonObject{{"type","command"},{"command",output(value)}}},event),hookOptions(root.path()));
            a::HookInput input{event=="PreToolUse"?a::HookKind::BeforeTool:event=="Stop"?a::HookKind::Stop:event=="PermissionRequest"?a::HookKind::PermissionRequest:a::HookKind::AfterTool,
                "s","r",{"call",remoteName,{}},{},{}};
            input.result.isError=event=="PostToolUseFailure";const auto result=hooks.callback()(input,{});
            QVERIFY(!result.updatedMCPToolOutput);QCOMPARE(result.diagnostics.last().toObject()["outcome"],"non_blocking_error");
        }
        for(const int code:{1,2}) {
            auto value=command(replacement);value["command"]=value["command"].toString()+QString("; exit %1").arg(code);
            a::CommandHooks hooks(settings({value}),hookOptions(root.path()));const auto result=run(remote,root.path(),{hooks.callback()});
            QCOMPARE(result.data["value"],original);QVERIFY(result.text!=replacement);
        }
        auto stop=response(replacement);stop["continue"]=false;stop["stopReason"]="stop after effect";
        a::CommandHooks hooks(settings({QJsonObject{{"type","command"},{"command",output(stop)}}}),hookOptions(root.path()));
        const auto before=remote.calls.load();bool finished=false;
        try {run(remote,root.path(),{hooks.callback()},original,[&](const a::Event& event){finished|=event.kind==a::EventKind::ToolFinished;});QFAIL("Stop did not propagate");}
        catch(const Error& error){QCOMPARE(error.code(),ErrorCode::Cancelled);}
        QVERIFY(!finished);QCOMPARE(remote.calls.load(),before+1);
    }
    void cancellingAPostHookKeepsTheCompletedExternalEffect() {
        QTemporaryDir root;Remote remote;
        const QJsonObject value{{"type","command"},{"command","touch hook-started; sleep 30; "+output(response(replacement))}};
        a::CommandHooks hooks(settings({value}),hookOptions(root.path()));CancellationToken token;
        struct Cancel {CancellationToken& token;~Cancel(){token.cancel();}} cancel{token};
        std::atomic_bool finished=false;
        auto pending=std::async(std::launch::async,[&] {
            try {a::ToolContext context{"s","r",root.path()};context.cancellation=token;
                a::ToolRunner(remote.registry,policy(),{{hooks.callback()}}).run({"call",remoteName,{{"value",original}}},context,
                    [&](const a::Event& event){if(event.kind==a::EventKind::ToolFinished)finished=true;});return ErrorCode::None;
            }catch(const Error& error){return error.code();}
        });
        // Cancel before joining even if the fixture's readiness assertion fails.
        const bool started=QTest::qWaitFor([&]{return QFileInfo::exists(root.filePath("hook-started"));},5000);token.cancel();
        QCOMPARE(pending.get(),ErrorCode::Cancelled);QVERIFY(started);QVERIFY(!finished.load());QCOMPARE(remote.calls.load(),1);
    }
    void commandReplacementClearsOriginalStructuredOutput() {
        QTemporaryDir root;Remote remote;
        a::CommandHooks hooks(settings({command(replacement)}),hookOptions(root.path()));
        const auto result=run(remote,root.path(),{hooks.callback()});
        QVERIFY(!result.isError);QCOMPARE(result.text,replacement);QVERIFY(result.data.isEmpty());
        QCOMPARE(result.content,QJsonArray{textBlock(replacement)});QCOMPARE(result.metadata["app"],"fixture");QCOMPARE(remote.calls.load(),1);
    }
    void emptyArraysClearWhileFalseValuesDoNotReplace() {
        QTemporaryDir root;Remote remote;
        for(const QJsonValue value:{QJsonValue(QJsonValue::Null),QJsonValue(false),QJsonValue(0),QJsonValue("")}) {
            a::CommandHooks hooks(settings({command(value)}),hookOptions(root.path()));
            const auto result=run(remote,root.path(),{hooks.callback()});QCOMPARE(result.data["value"],original);QVERIFY(result.text.contains(original));
        }
        a::CommandHooks hooks(settings({command(QJsonArray{})}),hookOptions(root.path()));
        const auto result=run(remote,root.path(),{hooks.callback()});
        QVERIFY(!result.isError);QVERIFY(result.text.isEmpty());QVERIFY(result.content.isEmpty());QVERIFY(result.data.isEmpty());
    }
    void malformedReplacementCannotEraseAValidEarlierResult() {
        QTemporaryDir root;Remote remote;
        for(const QJsonValue value:{QJsonValue(true),QJsonValue(7),QJsonValue(QJsonObject{}),QJsonValue(QJsonArray{QJsonObject{{"type","text"}}}),
            QJsonValue(QJsonArray{QJsonObject{{"type","tool_use"},{"name","Write"}}})}) {
            auto late=command(value);late["command"]="sleep .05; "+late["command"].toString();
            a::CommandHooks hooks(settings({late,command(replacement)}),hookOptions(root.path()));
            QJsonArray diagnostics;const auto result=run(remote,root.path(),{hooks.callback()},original,[&](const a::Event& e){if(e.kind==a::EventKind::Hook)diagnostics.append(e.data);});
            QVERIFY(!result.isError);QCOMPARE(result.text,replacement);QVERIFY(result.data.isEmpty());
            bool invalid=false;for(const auto& d:diagnostics)invalid|=d.toObject()["outcome"]=="non_blocking_error";QVERIFY(invalid);
        }
    }
    void callbacksObserveReplacementAndKeepAdditionalContext() {
        QTemporaryDir root;Remote remote;auto slow=command("last","second context");slow["command"]="sleep .05; "+slow["command"].toString();
        a::CommandHooks first(settings({slow,command("first","first context")}),hookOptions(root.path()));
        a::CommandHooks second(settings({command(replacement,"third context")}),hookOptions(root.path()));
        a::ToolResult seen;
        auto observer=[&](const a::HookInput& input,const auto&){if(input.kind==a::HookKind::AfterTool)seen=input.result;return a::HookResult{};};
        const auto result=run(remote,root.path(),{first.callback(),observer,second.callback()});
        QVERIFY(seen.text.startsWith("last\n"));QVERIFY(seen.data.isEmpty());QCOMPARE(seen.content,QJsonArray{textBlock("last")});
        QCOMPARE(result.text,replacement+"\nfirst context\nsecond context\nthird context");QVERIFY(!result.isError);
    }
    void nativeMetadataSpoofsErrorsAndDeniedCallsRetainTheirResults() {
        QTemporaryDir root;Remote remote;
        auto native=remote.registry->get(remoteName);native.definition.name="mcp__spoof__observe";
        // Copy the public definition, not the trusted imported Tool identity.
        a::Tool spoof;spoof.definition=native.definition;spoof.execute=[](const auto&,const auto&){return a::ToolResult{original,{{"value",original}}};};remote.registry->add(spoof);
        a::CommandHooks hooks(settings({command(replacement)}),hookOptions(root.path()));
        const auto nativeResult=a::ToolRunner(remote.registry,policy(),{{hooks.callback()}}).run({"call",spoof.definition.name,{{"value",original}}},{"s","r",root.path()});
        QCOMPARE(nativeResult.text,original);QCOMPARE(nativeResult.data["value"],original);
        for(const auto& value:QStringList{"tool-error","bad-output"}) {const auto result=run(remote,root.path(),{hooks.callback()},value);QVERIFY(result.isError);QVERIFY(result.text!=replacement);}
        const auto before=remote.calls.load();
        a::ToolRunner denied(remote.registry,std::make_shared<a::RulePolicy>(a::PermissionMode::DontAsk),{{hooks.callback()}});
        QVERIFY(denied.run({"call",remoteName,{{"value",original}}},{"s","r",root.path()}).isError);QCOMPARE(remote.calls.load(),before);
    }
#endif
};
QTEST_GUILESS_MAIN(McpOutputHooksTests)
#include "mcp_output_hooks_tests.moc"
