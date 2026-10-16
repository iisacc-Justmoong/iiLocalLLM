#include <agent/Engine.h>
#include <MediaContent.h>
#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QSaveFile>
#include <QtCore/QTemporaryDir>
#include <iostream>
using namespace iiLocalLLM;
namespace a=iiLocalLLM::agent;
namespace {
void require(bool ok,const char* text){if(!ok)throw std::runtime_error(text);}
QJsonObject block(QString path,QString type,QString mime) {
    QFile file(path);require(file.open(QIODevice::ReadOnly),"Cannot read media fixture");
    return {{"type",type},{"mimeType",mime},{"data",QString::fromLatin1(file.readAll().toBase64())}};
}
QByteArray solidPng() {
    auto be=[](quint32 value){QByteArray bytes;for(int shift:{24,16,8,0})bytes.append(char(value>>shift));return bytes;};
    auto chunk=[&](QByteArray type,const QByteArray& data) {
        const auto payload=type+data;quint32 crc=0xffffffff;
        for(const auto byte:payload){crc^=static_cast<unsigned char>(byte);for(int i=0;i<8;++i)crc=(crc>>1)^(0xedb88320u&quint32(-int(crc&1)));}
        return be(data.size())+payload+be(~crc);
    };
    constexpr int size=2048;
    QByteArray png=QByteArray::fromHex("89504e470d0a1a0a");
    png+=chunk("IHDR",be(size)+be(size)+QByteArray::fromHex("0802000000"));
    png+=chunk("IDAT",qCompress(QByteArray(size*(size*3+1),'\0'),9).mid(4));
    return png+chunk("IEND",{});
}
}
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);const auto args=app.arguments();if(args.size()!=5)return 2;
    try {
        const bool audio=args[1]=="audio";require(audio||args[1]=="vision","Expected audio or vision mode");
        QTemporaryDir root(QDir::current().filePath("multimodal-native-XXXXXX"));require(root.isValid(),"Cannot create native fixture");
        ServiceOptions options;options.modelsDirectory=root.filePath("Models");options.defaultContextTokens=8192;
        Service service(options);auto record=service.installModel(args[2]).get();
        require(service.verifyModel(record.uri).get().valid,"Native model package integrity failed");
        const auto info=service.loadModel({record.uri,8192}).get();
        auto model=std::make_shared<a::ServiceModel>(service);
        auto registry=std::make_shared<a::ToolRegistry>();auto policy=std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass);
        a::EngineOptions engineOptions;engineOptions.sessionsDirectory=root.filePath("sessions");engineOptions.skills.enabled=false;
        engineOptions.projectContext.enabled=false;engineOptions.compaction.automatic=false;
        const auto required=audio?Modalities(5):Modalities(11);
        engineOptions.decision.modelRoutes={{"text",record.uri.toStdString(),1},{"native",record.uri.toStdString(),required}};
        engineOptions.decision.inputs=[](const a::DecisionRequest& request) {
            a::DecisionInput input{"test-value"};for(const auto& candidate:request.candidates)
                input.estimates.push_back({candidate.id,100,10,candidate.name=="model:text"?0.0L:1.0L,0.9L});return input;
        };
        a::Engine engine(model,registry,policy,engineOptions);
        const auto media=block(args[3],audio?"audio":"image",audio?"audio/wav":"image/png");
        auto session=engine.createSession(record.uri,root.path(),audio?"Transcribe the speech accurately.":"Describe the image accurately in one short sentence.");
        a::RunRequest request;request.sessionId=session.id;request.prompt=audio?"What words are spoken?":"What is the main color of this image?";
        request.generation.maxTokens=96;request.generation.temperature=0;request.content={media};
        auto response=engine.run(request).result.get();
        if(response.status!=a::RunStatus::Completed)throw Error(response.errorCode,response.errorMessage);
        require(!response.text.trimmed().isEmpty()&&response.usage.promptTokens>0,"Native media generation produced no answer or budget");
        require(response.routing["selected_candidate_id"]=="native","Native route was not selected");
        require(engine.session(session.id).messages.first().content==request.content,"Transcript lost media bytes");
        a::ModelRequest measured;measured.model=record.uri;measured.messages={a::Message{"measure",a::MessageRole::User,request.prompt}};
        const auto textBudget=model->measure(measured,{});measured.messages.first().content=request.content;
        const auto mediaBudget=model->measure(measured,{});
        require(textBudget&&mediaBudget&&mediaBudget->inputTokens>textBudget->inputTokens,"Native budget omitted media embeddings");
        QJsonObject report{{"mode",args[1]},{"status","completed"},{"runtime",info.execution.runtime},{"backend",enumName(info.execution.device.backend)},
            {"answer",response.text},{"routing",response.routing},{"text_input_tokens",textBudget->inputTokens},{"media_input_tokens",mediaBudget->inputTokens},
            {"run",a::toJson(response)}};
        if(!audio) {
            auto frame=media;frame["timestamp_ms"]=0;
            auto next=block(QDir(QFileInfo(args[3]).absolutePath()).filePath("blue.png"),"image","image/png");next["timestamp_ms"]=1000;
            session=engine.createSession(record.uri,root.path(),"Describe the sequence of frames in one sentence.");request.sessionId=session.id;
            request.prompt="Describe the two frames in time order.";request.content={QJsonObject{{"type","video"},{"frames",QJsonArray{frame,next}}}};
            const auto video=engine.run(request).result.get();
            if(video.status!=a::RunStatus::Completed)throw Error(video.errorCode,video.errorMessage);
            require(!video.text.isEmpty()&&video.routing["required_modalities"]==11,"Native video frames did not reach generation");
            report["video"]=a::toJson(video);
            measured.messages.first().content=request.content;const auto videoBudget=model->measure(measured,{});
            require(videoBudget&&videoBudget->inputTokens>mediaBudget->inputTokens,"Video token budget omitted frames");
            report["video_input_tokens"]=videoBudget->inputTokens;
            // A changed image must not reuse text-only KV prefixes.
            auto repeat=service.converse(ConversationRequest{record.uri,QJsonArray{QJsonObject{{"role","user"},{"content",QJsonArray{media}}}}, {},"auto","media-cache",request.generation}).result.get();
            require(repeat.errorCode==ErrorCode::None&&repeat.usage.cachedTokens==0,"Media cache reused an invalid text prefix");
            report["media_cached_tokens"]=repeat.usage.cachedTokens;
            // Individually valid compressed images must share one decoded-media budget.
            auto excessive=measured;excessive.messages.first().content={};
            const QJsonObject large{{"type","image"},{"mimeType","image/png"},{"data",QString::fromLatin1(solidPng().toBase64())}};
            for(int i=0;i<11;++i)excessive.messages.first().content.append(large);
            bool bounded=false;try{model->measure(excessive,{});}catch(const Error& error){bounded=error.code()==ErrorCode::ResourceLimit;}
            require(bounded,"Multiple decoded images exceeded the request media budget");report["aggregate_media_budget_rejected"]=true;
        }
        auto bad=measured;bad.messages.first().content={audio?QJsonObject{{"type","image"},{"mimeType","image/png"},{"data",media["data"]}}
            :QJsonObject{{"type","audio"},{"mimeType","audio/wav"},{"data",media["data"]}}};
        bool rejected=false;try{model->measure(bad,{});}catch(const Error& error){rejected=error.code()==ErrorCode::RuntimeUnavailable||error.code()==ErrorCode::InvalidArgument;}
        require(rejected,"Unsupported or mismatched modality was silently consumed");report["unsupported_modality_rejected"]=true;
        service.unloadModel(record.uri).get();
        QSaveFile output(args[4]);require(output.open(QIODevice::WriteOnly),"Cannot create native report");
        const auto bytes=QJsonDocument(report).toJson();require(output.write(bytes)==bytes.size()&&output.commit(),"Cannot save native report");
        std::cout<<bytes.constData();return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
