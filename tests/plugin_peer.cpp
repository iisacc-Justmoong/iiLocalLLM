// Independent stdio peer for plugin process, argument and environment tests.
#include <QtCore/QCoreApplication>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <iostream>
int main(int argc,char** argv){
    QCoreApplication app(argc,argv);std::string line;
    while(std::getline(std::cin,line)){
        const auto request=QJsonDocument::fromJson(QByteArray::fromStdString(line)).object();
        if(!request.contains("id"))continue;
        const auto method=request["method"].toString();QJsonObject result;
        if(method=="initialize")result={{"protocolVersion","2025-11-25"},{"capabilities",QJsonObject{{"tools",QJsonObject{}}}},{"serverInfo",QJsonObject{{"name","plugin-peer"},{"version","1"}}}};
        else if(method=="tools/list")result={{"tools",QJsonArray{QJsonObject{{"name","echo"},{"description","Return the independently observed plugin process configuration."},
            {"inputSchema",QJsonObject{{"type","object"},{"properties",QJsonObject{}}}},{"annotations",QJsonObject{{"readOnlyHint",true}}}}}}};
        else if(method=="tools/call"){
            const QJsonObject observed{{"root",QString::fromUtf8(qgetenv("CLAUDE_PLUGIN_ROOT"))},{"data",QString::fromUtf8(qgetenv("CLAUDE_PLUGIN_DATA"))},
                {"argument",app.arguments().value(1)},{"secret",QString::fromUtf8(qgetenv("PLUGIN_SECRET"))}};
            result={{"content",QJsonArray{QJsonObject{{"type","text"},{"text",QString::fromUtf8(QJsonDocument(observed).toJson(QJsonDocument::Compact))}}}},{"structuredContent",observed}};
        }else if(method=="ping")result={};
        else {std::cout<<QJsonDocument(QJsonObject{{"jsonrpc","2.0"},{"id",request["id"]},{"error",QJsonObject{{"code",-32601},{"message","Unknown method"}}}}).toJson(QJsonDocument::Compact).constData()<<std::endl;continue;}
        std::cout<<QJsonDocument(QJsonObject{{"jsonrpc","2.0"},{"id",request["id"]},{"result",result}}).toJson(QJsonDocument::Compact).constData()<<std::endl;
    }
}
