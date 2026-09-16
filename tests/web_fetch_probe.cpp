#include <agent/WebFetch.h>
#include <QtCore/QCoreApplication>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QUrl>
#include <QtNetwork/QSslCertificate>
#include <iostream>
namespace a=iiLocalLLM::agent;
class Echo final:public a::Model {
    a::ModelReply generate(const a::ModelRequest& request,const iiLocalLLM::CancellationToken&,const std::function<bool(const QString&)>&)override {
        return {QJsonDocument::fromJson(request.messages.last().text.toUtf8()).object()["content"].toString(),{},{}};
    }
};
int main(int argc,char** argv){QCoreApplication app(argc,argv);if(argc<3||argc>4)return 2;
    QJsonObject result;try {
        const auto url=QUrl(QString::fromUtf8(argv[1]));a::WebFetchOptions o;o.model="fixture";o.timeoutMs=3000;
        if(argc==4&&QString::fromUtf8(argv[3])=="private")o.privateOrigins={url.adjusted(QUrl::RemovePath|QUrl::RemoveQuery|QUrl::RemoveFragment).toString()};
        if(QString::fromUtf8(argv[2])!="-"){QFile file(QString::fromUtf8(argv[2]));if(!file.open(QIODevice::ReadOnly))return 3;
            const auto ca=QSslCertificate::fromData(file.readAll());if(ca.isEmpty())return 4;o.sslConfiguration.setCaCertificates(ca);}
        a::WebFetch fetch(std::make_shared<Echo>(),o);result=fetch.fetch({{"url",url.toString()},{"prompt","Return the page text"}},{"probe"}).data;result["passed"]=true;
    }catch(const std::exception& e){result={{"passed",false},{"error",QString::fromUtf8(e.what())}};}
    std::cout<<QJsonDocument(result).toJson(QJsonDocument::Compact).constData()<<std::endl;return 0;
}
