#include <agent/Engine.h>
#include <QtCore/QCoreApplication>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QTemporaryDir>
#include <QtCore/QUuid>
#include <filesystem>
#include <iostream>
using namespace iiLocalLLM;
namespace a=iiLocalLLM::agent;
namespace {
void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
void write(const QString& path,const QByteArray& bytes){QFile file(path);require(file.open(QIODevice::WriteOnly)&&file.write(bytes)==bytes.size(),"Cannot write fixture");}
QByteArray read(const QString& path){QFile file(path);require(file.open(QIODevice::ReadOnly),"Cannot read fixture");return file.readAll();}
}
int main(int argc,char** argv){QCoreApplication app(argc,argv);
    if(argc<3||argc>7)return 2;bool toolGrammar=true,qwenSampling=false,thinking=false,checkpoints=false;
    for(int i=3;i<argc;++i){const auto option=QString::fromLocal8Bit(argv[i]);
        if(option=="--no-tool-grammar")toolGrammar=false;else if(option=="--qwen-sampling")qwenSampling=true;
        else if(option=="--thinking"){thinking=true;qwenSampling=true;}else if(option=="--checkpoints")checkpoints=true;else return 2;}
    try {
        QTemporaryDir root(QDir::current().filePath("notebook-native-XXXXXX"));require(root.isValid(),"Cannot create fixture");
        const auto work=root.filePath("work");QDir().mkpath(work);
        const auto marker="notebook_"+QUuid::createUuid().toString(QUuid::WithoutBraces).remove('-').left(12);
        const QJsonObject preserved{{"cell_type","markdown"},{"id","keep"},{"source",QJsonArray{"# Keep\n","한글 metadata"}},{"metadata",QJsonObject{{"custom",true}}}};
        const QJsonObject notebook{{"nbformat",4},{"nbformat_minor",5},{"metadata",QJsonObject{{"language_info",QJsonObject{{"name","python"}}},{"custom","preserved"}}},
            {"cells",QJsonArray{QJsonObject{{"cell_type","code"},{"id","main"},{"source","secret = \""+marker+"\""},{"metadata",QJsonObject{}},
                {"execution_count",1},{"outputs",QJsonArray{QJsonObject{{"output_type","stream"},{"name","stdout"},{"text","old output"}}}}},preserved}}};
        const auto path=work+"/book.ipynb";const auto originalBytes=QJsonDocument(notebook).toJson();write(path,originalBytes);
        const auto uri=QString::fromLocal8Bit(argv[2]),catalog=QDir(QString::fromLocal8Bit(argv[1])).filePath(modelId(uri));
        const auto manifest=parseModelManifest(QJsonDocument::fromJson(read(catalog+"/manifest.json")).object());
        const auto models=root.filePath("models"),package=models+'/'+manifest.id;QDir().mkpath(package);
        for(const auto& item:manifest.files){const auto target=package+'/'+item.path;QDir().mkpath(QFileInfo(target).absolutePath());std::error_code error;
            std::filesystem::create_hard_link((catalog+'/'+item.path).toStdString(),target.toStdString(),error);require(!error||QFile::copy(catalog+'/'+item.path,target),"Cannot provision model");}
        write(package+"/manifest.json",QJsonDocument(manifestObject(manifest)).toJson());
        ServiceOptions so;so.modelsDirectory=models;so.maxCachedContexts=1;so.maxCachedContextTokens=4096;
        Service service(so);ModelLoadRequest load{uri,4096};load.options={{"tool_grammar",toolGrammar},{"enable_thinking",thinking}};(void)service.loadModel(load).get();
        auto model=std::make_shared<a::ServiceModel>(service);auto registry=std::make_shared<a::ToolRegistry>();a::registerWorkspaceTools(*registry,work);
        auto rules=QList<a::PermissionRule>{{"NotebookEdit",a::PermissionBehavior::Allow}};
        if(checkpoints){rules.append({"Write",a::PermissionBehavior::Allow});rules.append({"RewindFiles",a::PermissionBehavior::Allow});}
        auto policy=std::make_shared<a::RulePolicy>(a::PermissionMode::DontAsk,rules);
        a::EngineOptions options;options.sessionsDirectory=root.filePath("sessions");options.projectContext.enabled=false;options.skills.enabled=false;options.compaction.automatic=false;options.toolSearch.enabled=false;
        options.fileCheckpointsEnabled=checkpoints;
        options.toolFilter=[](const a::ToolDefinition& tool){return QStringList{"Read","NotebookEdit"}.contains(tool.name);};
        QJsonArray calls;options.hooks.append([&](const a::HookInput& input,const auto&){if(input.kind==a::HookKind::AfterTool)
            calls.append(QJsonObject{{"name",input.call.name},{"arguments",input.call.arguments},{"result",input.result.data},{"is_error",input.result.isError}});return a::HookResult{};});
        a::Engine engine(model,registry,policy,options);const auto session=engine.createSession(uri,work,"Use real tools to observe the notebook and make the requested edit. Do not guess the secret. Never execute code.");
        a::RunRequest request{session.id,"Use Read to read book.ipynb completely. Find the source field of code cell main. Use NotebookEdit to replace that source, changing only the variable name secret to checked. Copy every other source character exactly, including the two quote characters around the value; do not add a third quote. Leave all other cells unchanged. Finally return only the string assigned to checked, without quote characters. The answer comes from the code cell source, not from outputs, text, or metadata fields."};
        request.maxTurns=6;request.generation.maxTokens=1024;request.generation.temperature=0;
        if(qwenSampling){request.generation.temperature=thinking?0.6:0.7;request.generation.topP=thinking?0.95:0.8;request.generation.topK=20;}
        const auto result=engine.run(request).result.get();std::cerr<<QJsonDocument(a::toJson(result)).toJson().constData()<<std::endl;
        std::cerr<<QJsonDocument(QJsonObject{{"tools",calls},{"notebook",QJsonDocument::fromJson(read(path)).object()}}).toJson().constData()<<std::endl;
        const auto modelCalls=calls;
        const auto after=QJsonDocument::fromJson(read(path)).object();const auto cells=after["cells"].toArray();const auto first=cells[0].toObject();
        bool toolsSucceeded=true;QString backup;
        for(const auto& call:modelCalls){const auto value=call.toObject();toolsSucceeded&=!value["is_error"].toBool();
            if(value["name"]=="NotebookEdit")backup=value["result"].toObject()["backup_path"].toString();}
        const bool backupPreserved=!backup.isEmpty()&&QJsonDocument::fromJson(read(backup)).object()==notebook;
        const auto messageCount=engine.session(session.id).messages.size();const auto hostRead=engine.runNotebookTool(session.id,"Read",{{"notebook_path","book.ipynb"}});
        QJsonObject checks{
            {"exact_answer",result.status==a::RunStatus::Completed&&result.text.trimmed()==marker},
            {"tool_order",modelCalls.size()==2&&modelCalls[0].toObject()["name"]=="Read"&&modelCalls[1].toObject()["name"]=="NotebookEdit"},
            {"tools_succeeded",toolsSucceeded},
            {"exact_source",first["source"].toString().trimmed()=="checked = \""+marker+"\""},
            {"execution_cleared",first["outputs"].toArray().isEmpty()&&first["execution_count"].isNull()},
            {"other_data_preserved",cells.size()==2&&cells[1].toObject()==preserved&&after["metadata"]==notebook["metadata"]},
            {"original_preserved_in_backup",backupPreserved},{"host_read",!hostRead.isError},
            {"host_transcript_unchanged",engine.session(session.id).messages.size()==messageCount}};
        QJsonObject checkpointReport;
        if(checkpoints){
            const auto changedBytes=read(path);const auto originalId=engine.session(session.id).messages.first().id;
            const auto saved=engine.checkpointFiles(session.id);const auto preview=engine.rewindFiles(session.id,originalId,true);
            checks["rewind_preview_unchanged"]=!preview.isError&&read(path)==changedBytes&&preview.data["filesChanged"].toArray().size()==1;
            const auto undo=engine.rewindFiles(session.id,originalId);checks["rewind_exact_original_bytes"]=!undo.isError&&read(path)==originalBytes;
            const auto redo=engine.rewindFiles(session.id,saved["message_id"].toString());checks["rewind_exact_edited_bytes"]=!redo.isError&&read(path)==changedBytes;
            checks["rewind_transcript_unchanged"]=engine.session(session.id).messages.size()==messageCount;
            checkpointReport={{"preview",preview.data},{"undo",undo.data},{"redo",redo.data},{"history",engine.fileCheckpoints(session.id)}};
        }
        bool passed=true;for(const auto& value:checks)passed&=value.toBool();
        const QJsonObject report{{"passed",passed},{"checks",checks},{"file_checkpoints",checkpointReport},{"model",uri},{"model_requests_unmodified",true},{"marker",marker},{"answer",a::toJson(result)},
            {"tools",modelCalls},{"notebook",after},{"context_tokens",4096},{"cached_contexts",1},{"tool_grammar",toolGrammar},{"original_preserved_in_backup",backupPreserved},
            {"thinking",thinking},{"sampling",thinking?"qwen-thinking":qwenSampling?"qwen-nonthinking":"greedy"},{"temperature",request.generation.temperature},{"top_p",request.generation.topP},
            {"top_k",request.generation.topK},{"min_p",request.generation.minP},{"seed",qint64(request.generation.seed)},
            {"host_transcript_unchanged",checks["host_transcript_unchanged"]}};
        std::cout<<QJsonDocument(report).toJson(QJsonDocument::Compact).constData()<<std::endl;
        require(passed,"Native notebook scenario failed; see the individual checks in the JSON report");
    }catch(const std::exception& error){std::cerr<<error.what()<<std::endl;return 1;}return 0;
}
