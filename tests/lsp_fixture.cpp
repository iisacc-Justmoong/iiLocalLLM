#include <QtCore/QCoreApplication>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>
#include <QtCore/QThread>
#include <iostream>
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);const auto args=app.arguments();const auto mode=args.value(1);QFile log(args.value(2));
    if(!log.open(QIODevice::WriteOnly|QIODevice::Append))return 2;
    auto send=[](const QJsonObject& value){const auto bytes=QJsonDocument(value).toJson(QJsonDocument::Compact);std::cout<<"Content-Length: "<<bytes.size()<<"\r\n\r\n";
        // Split Unicode and body bytes across writes to exercise stream framing.
        for(int i=0;i<bytes.size();i+=7){std::cout.write(bytes.data()+i,std::min(7,int(bytes.size())-i));std::cout.flush();}};
    QJsonObject document,lastChange;int opened=0,changed=0,attempts=0;
    for(;;){std::string line;int length=-1;while(std::getline(std::cin,line)){if(line=="\r")break;if(line.starts_with("Content-Length:"))length=std::stoi(line.substr(15));}
        if(!std::cin||length<0)return 0;QByteArray bytes(length,Qt::Uninitialized);std::cin.read(bytes.data(),length);if(!std::cin)return 3;
        const auto message=QJsonDocument::fromJson(bytes).object();log.write(bytes+'\n');log.flush();const auto method=message["method"].toString();const auto p=message["params"].toObject();
        if(method=="exit")return 0;
        if(method=="textDocument/didOpen"){document=p["textDocument"].toObject();++opened;continue;}
        if(method=="textDocument/didChange"){lastChange=p;document["version"]=p["textDocument"].toObject()["version"];++changed;continue;}
        if(!message.contains("id")||method.isEmpty())continue;
        const auto id=message["id"];QJsonValue result;
        if(method=="initialize") {
            if(mode=="init-error"){send({{"jsonrpc","2.0"},{"id",id},{"error",QJsonObject{{"code",-32002},{"message","Initialization rejected"}}}});continue;}
            if(mode=="bad-frame"){std::cout<<"Content-Length: 999999999\r\n\r\n";std::cout.flush();QThread::sleep(5);return 4;}
            QJsonObject caps{{"positionEncoding",mode=="utf8"?"utf-8":"utf-16"},{"textDocumentSync",QJsonObject{{"openClose",true},{"change",2}}}};
            for(const auto& key:{"definitionProvider","referencesProvider","hoverProvider","documentSymbolProvider","workspaceSymbolProvider","implementationProvider","callHierarchyProvider"})caps[key]=mode!="unsupported";
            result=QJsonObject{{"capabilities",caps}};
            send({{"jsonrpc","2.0"},{"id","server-settings"},{"method","workspace/configuration"},{"params",QJsonObject{{"items",QJsonArray{QJsonObject{{"section","fixture"}}}}}}});
        } else if(method=="shutdown")result=QJsonValue::Null;
        else {
            if(mode=="invalid-result"){send({{"jsonrpc","2.0"},{"id",id},{"result","not a protocol result"}});continue;}
            if(mode=="slow"){QThread::sleep(5);continue;}
            if(mode=="crash")return 9;
            if(mode=="retry"&&++attempts<3){send({{"jsonrpc","2.0"},{"id",id},{"error",QJsonObject{{"code",-32801},{"message","Content modified"}}}});continue;}
            const auto uri=document["uri"].toString();const QJsonObject range{{"start",QJsonObject{{"line",0},{"character",0}}},{"end",QJsonObject{{"line",0},{"character",3}}}};
            const QJsonObject item{{"name","fixture_fn"},{"kind",12},{"uri",uri},{"range",range},{"selectionRange",range},{"data","opaque-cookie"}};
            if(method=="textDocument/hover") result=QJsonObject{{"contents",QJsonObject{{"kind","markdown"},{"value",QString("한글 opened=%1 changed=%2 version=%3").arg(opened).arg(changed).arg(document["version"].toInt())}}},
                {"observed_position",p["position"]},{"last_change",lastChange},{"opened_text",document["text"]}};
            else if(method=="textDocument/documentSymbol")result=QJsonArray{item};
            else if(method=="workspace/symbol")result=QJsonArray{QJsonObject{{"name","fixture_fn"},{"kind",12},{"location",QJsonObject{{"uri",uri},{"range",range}}}}};
            else if(method=="textDocument/prepareCallHierarchy")result=QJsonArray{item};
            else if(method.startsWith("callHierarchy/")){if(p["item"].toObject()["data"]!="opaque-cookie")return 10;result=QJsonArray{QJsonObject{{method.endsWith("incomingCalls")?"from":"to",item},{"fromRanges",QJsonArray{range}}}};}
            else result=QJsonArray{QJsonObject{{"uri",uri},{"range",range}},QJsonObject{{"uri","file:///outside/secret.cpp"},{"range",range}}};
            send({{"jsonrpc","2.0"},{"method","textDocument/publishDiagnostics"},{"params",QJsonObject{{"uri",uri},{"version",document["version"]},{"diagnostics",QJsonArray{QJsonObject{{"range",range},{"severity",2},{"message","fixture diagnostic"}}}}}}});
        }
        send({{"jsonrpc","2.0"},{"id",id},{"result",result}});
    }
}
