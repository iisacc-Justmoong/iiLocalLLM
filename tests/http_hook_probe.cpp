#include <agent/CommandHooks.h>
#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtNetwork/QSslCertificate>
#include <iostream>
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);
    if(argc!=3)return 2;
    try {
        namespace a=iiLocalLLM::agent;
        a::CommandHookOptions options;options.workingDirectory=QDir::currentPath();options.environment={};options.timeoutMs=3000;
        if(QString::fromUtf8(argv[2])!="-") {
            QFile file(QString::fromUtf8(argv[2]));if(!file.open(QIODevice::ReadOnly))return 3;
            const auto certificates=QSslCertificate::fromData(file.readAll());if(certificates.isEmpty())return 4;
            options.httpSslConfiguration.setCaCertificates(certificates);
        }
        const QJsonObject hook{{"type","http"},{"url",QString::fromUtf8(argv[1])}};
        const QJsonObject config{{"hooks",QJsonObject{{"PreToolUse",QJsonArray{QJsonObject{{"hooks",QJsonArray{hook}}}}}}}};
        const auto result=a::CommandHooks(config,options).callback()({a::HookKind::BeforeTool,"tls-session","tls-run",{"call","Write",{{"path","file.txt"},{"content","TLS_INPUT"}}},{},{}},{});
        std::cout<<QJsonDocument(QJsonObject{{"feedback",result.feedback},{"diagnostics",result.diagnostics}}).toJson(QJsonDocument::Compact).constData()<<std::endl;
        return 0;
    } catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
