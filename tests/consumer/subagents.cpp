#include <agent/Subagents.h>
#include <QtCore/QCoreApplication>
#include <QtCore/QTemporaryDir>
#include <QtCore/QDir>
#include <iostream>
namespace a=iiLocalLLM::agent;
class Model final:public a::Model {
    a::ModelReply generate(const a::ModelRequest& request,const iiLocalLLM::CancellationToken&,const iiLocalLLM::TextCallback&)override {
        return {request.messages.last().text,{}};
    }
};
int main(int argc,char** argv){
    QCoreApplication app(argc,argv);
    try {
        QTemporaryDir root;const auto workspace=root.filePath("workspace");QDir().mkpath(workspace);
        auto model=std::make_shared<Model>();auto registry=std::make_shared<a::ToolRegistry>();auto policy=std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass);
        a::EngineOptions eo;eo.sessionsDirectory=root.filePath("parents");
        a::SubagentOptions so;so.workingDirectory=workspace;so.stateDirectory=root.filePath("children");
        auto children=std::make_shared<a::Subagents>(model,registry,policy,eo,so);eo.additionalTools=a::Subagents::tools(children);
        a::Engine engine(model,registry,policy,eo);const auto parent=engine.createSession("local",workspace);
        auto first=engine.runSubagentTool(parent.id,"Agent",{{"prompt","INSTALLED_CHILD"}});
        if(first.isError||first.data["result"].toObject()["text"]!="INSTALLED_CHILD")return 1;
        const auto id=first.data["agentId"];
        auto resumed=engine.runSubagentTool(parent.id,"Agent",{{"resume",id},{"prompt","RESUMED_CHILD"},{"run_in_background",true}});
        if(resumed.isError||resumed.data["status"]!="async_launched")return 1;
        auto output=engine.runSubagentTool(parent.id,"AgentOutput",{{"agent_id",id},{"block",true},{"timeout_ms",5000}});
        if(output.isError||output.data["status"]!="completed"||output.data["result"].toObject()["text"]!="RESUMED_CHILD")return 1;
        if(engine.queuedInputs(parent.id)["count"]!=1||engine.subagentToolDefinitions().size()!=4)return 1;
        std::cout<<"installed subagent ABI, foreground, background, resume and notification passed\n";
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
