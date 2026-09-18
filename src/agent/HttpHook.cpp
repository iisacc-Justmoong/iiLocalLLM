#include "HttpHook.h"
#include <QtCore/QEventLoop>
#include <QtCore/QRegularExpression>
#include <QtCore/QTimer>
#include <QtNetwork/QHostInfo>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QSslSocket>
#include <chrono>
#include <cstring>

namespace iiLocalLLM::agent::detail {
namespace {
using Clock=std::chrono::steady_clock;
void require(bool value,const QString& text,ErrorCode code=ErrorCode::InvalidArgument) {
    if(!value)throw Error(code,text);
}
QStringList strings(const QJsonValue& value,int maximum=128) {
    require(value.isArray()&&value.toArray().size()<=maximum,"Invalid HTTP hook policy array");
    QStringList result;
    for(const auto& item:value.toArray()) {
        require(item.isString()&&item.toString().size()<=8192&&!item.toString().contains(QChar::Null),"Invalid HTTP hook policy string");
        result.append(item.toString());
    }
    return result;
}
bool patternMatches(const QString& url,const QString& pattern) {
    auto expression=QRegularExpression::escape(pattern);expression.replace("\\*",".*");
    return QRegularExpression("\\A"+expression+"\\z",QRegularExpression::DotMatchesEverythingOption).match(url).hasMatch();
}
QString headerValue(QString value,const HttpHook& hook,const QProcessEnvironment& environment) {
    static const QRegularExpression variables("\\$\\{([A-Z_][A-Z0-9_]*)\\}|\\$([A-Z_][A-Z0-9_]*)");
    QString result;qsizetype end=0;
    for(auto it=variables.globalMatch(value);it.hasNext();) {
        const auto match=it.next();result+=value.sliced(end,match.capturedStart()-end);
        const auto name=match.captured(1).isEmpty()?match.captured(2):match.captured(1);
        if(hook.allowedEnvVars.contains(name))result+=environment.value(name);
        end=match.capturedEnd();
        require(result.size()<=65536,"HTTP hook headers exceed limit",ErrorCode::ResourceLimit);
    }
    result+=value.sliced(end);result.remove('\r');result.remove('\n');result.remove(QChar::Null);
    require(result.toUtf8().size()<=65536,"HTTP hook headers exceed limit",ErrorCode::ResourceLimit);
    for(const auto ch:result)require((ch.unicode()>=0x20&&ch.unicode()!=0x7f)||ch=='\t',"Invalid HTTP hook header value");
    return result;
}
QString firstEnvironment(const QProcessEnvironment& env,const QStringList& keys) {
    for(const auto& key:keys)if(!env.value(key).isEmpty())return env.value(key);
    return {};
}
bool bypassProxy(const QUrl& url,const QString& value) {
    if(value=="*")return true;
    auto host=url.host().toLower();if(host.contains(':'))host='['+host+']';
    const auto withPort=host+':'+QString::number(url.port(url.scheme()=="https"?443:80));
    for(const auto& item:value.toLower().split(QRegularExpression("[,\\s]+"),Qt::SkipEmptyParts)) {
        if(item.contains(':')) {if(item==withPort)return true;}
        else if(item.startsWith('.')) {if(host==item.sliced(1)||host.endsWith(item))return true;}
        else if(item==host)return true;
    }
    return false;
}
QNetworkProxy proxyFor(const QUrl& url,const CommandHookOptions& options) {
    if(options.httpProxy) {
        const auto proxy=*options.httpProxy;
        require(proxy.type()==QNetworkProxy::NoProxy||proxy.type()==QNetworkProxy::HttpProxy
            ||proxy.type()==QNetworkProxy::HttpCachingProxy||proxy.type()==QNetworkProxy::Socks5Proxy,"Invalid HTTP hook host proxy");
        return proxy;
    }
    const auto value=firstEnvironment(options.environment,{"https_proxy","HTTPS_PROXY","http_proxy","HTTP_PROXY"});
    if(value.isEmpty()||bypassProxy(url,firstEnvironment(options.environment,{"no_proxy","NO_PROXY"})))return QNetworkProxy(QNetworkProxy::NoProxy);
    const QUrl proxy(value,QUrl::StrictMode);
    require(proxy.isValid()&&!proxy.host().isEmpty()&&!proxy.hasQuery()&&!proxy.hasFragment()
        &&(proxy.path().isEmpty()||proxy.path()=="/")&&proxy.port(80)>0,"Invalid HTTP hook environment proxy");
    require(proxy.scheme()=="http","Qt HTTP hooks require an HTTP proxy transport",ErrorCode::RuntimeUnavailable);
    return QNetworkProxy(QNetworkProxy::HttpProxy,proxy.host(),quint16(proxy.port(80)),proxy.userName(),proxy.password());
}
QList<QHostAddress> resolve(const QString& host,Clock::time_point deadline,const CancellationToken& token) {
    QHostAddress literal;if(literal.setAddress(host))return {literal};
    QEventLoop loop;QTimer timer;timer.setInterval(10);QHostInfo result;bool finished=false;
    QObject::connect(&timer,&QTimer::timeout,&loop,[&]{if(token.isCancelled()||Clock::now()>=deadline)loop.quit();});
    const auto lookup=QHostInfo::lookupHost(host,&loop,[&](const QHostInfo& info){result=info;finished=true;loop.quit();});
    timer.start();loop.exec();if(!finished)QHostInfo::abortHostLookup(lookup);
    token.throwIfCancelled();require(Clock::now()<deadline,"HTTP hook timed out",ErrorCode::Timeout);
    require(finished&&result.error()==QHostInfo::NoError&&!result.addresses().isEmpty(),"HTTP hook DNS resolution failed",ErrorCode::RuntimeFailure);
    return result.addresses();
}
class Upload final:public QIODevice {
    const QByteArray& bytes;qsizetype offset=0;
public:
    explicit Upload(const QByteArray& value):bytes(value){open(QIODevice::ReadOnly);}
    bool isSequential() const override{return true;}
    qint64 size() const override{return bytes.size();}
    qint64 bytesAvailable() const override{return bytes.size()-offset+QIODevice::bytesAvailable();}
    bool atEnd() const override{return offset==bytes.size();}
protected:
    qint64 readData(char* target,qint64 maximum) override {
        const auto count=std::min<qint64>(maximum,bytes.size()-offset);if(count<=0)return -1;
        std::memcpy(target,bytes.constData()+offset,size_t(count));offset+=count;return count;
    }
    qint64 writeData(const char*,qint64) override{return -1;}
};
}
void validateHttpHookSettings(const QJsonObject& settings) {
    for(const auto& key:{"allowedHttpHookUrls","httpHookAllowedEnvVars"})if(settings.contains(key))strings(settings[key]);
}
HttpHook parseHttpHook(const QJsonObject& object,const QJsonObject& settings) {
    HttpHook hook;require(object["url"].isString(),"HTTP hook requires a URL");hook.url=object["url"].toString();
    const QUrl url(hook.url,QUrl::StrictMode);
    require(hook.url.size()<=8192&&!hook.url.contains(QChar::Null)&&url.isValid()&&(url.scheme()=="http"||url.scheme()=="https")
        &&!url.host().isEmpty()&&url.userInfo().isEmpty()&&!url.hasFragment()&&url.port(80)>0,"Invalid HTTP hook URL");
    if(settings.contains("allowedHttpHookUrls")) {
        const auto patterns=strings(settings["allowedHttpHookUrls"]);hook.permitted=false;
        for(const auto& pattern:patterns)hook.permitted|=patternMatches(hook.url,pattern);
    }
    if(object.contains("allowedEnvVars"))for(const auto& name:strings(object["allowedEnvVars"]))hook.allowedEnvVars.insert(name);
    if(settings.contains("httpHookAllowedEnvVars")) {
        const auto names=strings(settings["httpHookAllowedEnvVars"]);QSet<QString> allowed(names.begin(),names.end());hook.allowedEnvVars.intersect(allowed);
    }
    if(object.contains("headers")) {
        require(object["headers"].isObject()&&object["headers"].toObject().size()<=128,"Invalid HTTP hook headers");
        static const QRegularExpression name("\\A[!#$%&'*+.^_`|~0-9A-Za-z-]+\\z");
        const auto headers=object["headers"].toObject();qsizetype bytes=0;
        for(auto it=headers.begin();it!=headers.end();++it) {
            const auto key=it.key().toLatin1().toLower();
            require(it.key().size()<=128&&name.match(it.key()).hasMatch()&&it->isString()&&!hook.headers.contains(key),"Invalid HTTP hook header");
            require(!QList<QByteArray>{"host","content-length","transfer-encoding","connection","proxy-authorization","proxy-connection","upgrade","te","trailer"}.contains(key),"HTTP hook transport header is reserved");
            bytes+=key.size()+it->toString().toUtf8().size();require(bytes<=65536,"HTTP hook headers exceed limit",ErrorCode::ResourceLimit);
            hook.headers.insert(key,it->toString());
        }
    }
    return hook;
}
bool blockedHookAddress(const QHostAddress& address) {
    bool isV4=false;const auto v4=address.toIPv4Address(&isV4);
    if(isV4) {
        const auto first=v4>>24,second=(v4>>16)&255;
        return first==0||first==10||(first==100&&second>=64&&second<=127)||(first==169&&second==254)
            ||(first==172&&second>=16&&second<=31)||(first==192&&second==168);
    }
    if(address.protocol()!=QAbstractSocket::IPv6Protocol)return true;
    const auto v6=address.toIPv6Address();
    return address==QHostAddress(QHostAddress::AnyIPv6)||(v6[0]&0xfe)==0xfc||(v6[0]==0xfe&&(v6[1]&0xc0)==0x80);
}
HttpHookResult postHttpHook(const HttpHook& hook,const QByteArray& payload,const CommandHookOptions& options,
    int timeoutMs,const CancellationToken& token,const std::function<void()>& started) {
    token.throwIfCancelled();require(hook.permitted,"HTTP hook URL is not allowed",ErrorCode::Unauthorized);
    const auto deadline=Clock::now()+std::chrono::milliseconds(timeoutMs);
    const QUrl original(hook.url,QUrl::StrictMode);auto destination=original;
    const auto proxy=proxyFor(original,options);QMap<QByteArray,QByteArray> headers;qsizetype bytes=0;
    for(auto it=hook.headers.begin();it!=hook.headers.end();++it) {
        const auto value=headerValue(it.value(),hook,options.environment).toUtf8();bytes+=it.key().size()+value.size();
        require(bytes<=65536,"HTTP hook headers exceed limit",ErrorCode::ResourceLimit);headers.insert(it.key(),value);
    }
    if(proxy.type()==QNetworkProxy::NoProxy) {
        const auto addresses=resolve(original.host(),deadline,token);
        for(const auto& address:addresses)require(!blockedHookAddress(address),"HTTP hook resolved to a private or link-local address",ErrorCode::Unauthorized);
        auto chosen=addresses.first();for(const auto& address:addresses)if(address.protocol()==QAbstractSocket::IPv4Protocol){chosen=address;break;}
        // Pin the validated address. Host and TLS identity still use the URL's
        // original name; there is no second hostname lookup at connect time.
        destination.setHost(chosen.toString());
    }
    token.throwIfCancelled();require(Clock::now()<deadline,"HTTP hook timed out",ErrorCode::Timeout);
    QNetworkAccessManager manager;manager.setProxy(proxy);
    QNetworkRequest request(destination);request.setPeerVerifyName(original.host());
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,QNetworkRequest::ManualRedirectPolicy);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute,QNetworkRequest::AlwaysNetwork);
    request.setAttribute(QNetworkRequest::CacheSaveControlAttribute,false);
    request.setAttribute(QNetworkRequest::CookieLoadControlAttribute,QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CookieSaveControlAttribute,QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::DoNotBufferUploadDataAttribute,true);
    request.setAttribute(QNetworkRequest::Http2AllowedAttribute,false);
    auto ssl=options.httpSslConfiguration;ssl.setPeerVerifyMode(QSslSocket::VerifyPeer);request.setSslConfiguration(ssl);
    request.setRawHeader("Host",original.authority(QUrl::FullyEncoded).toUtf8());
    request.setRawHeader("Content-Type","application/json");request.setRawHeader("Accept","application/json");
    for(auto it=headers.begin();it!=headers.end();++it)request.setRawHeader(it.key(),it.value());
    request.setHeader(QNetworkRequest::ContentLengthHeader,payload.size());
    Upload upload(payload);started();std::unique_ptr<QNetworkReply> reply(manager.post(request,&upload));
    reply->setReadBufferSize(std::min(options.maxOutputBytes+1,65536));
    QEventLoop loop;QTimer timer;timer.setInterval(10);HttpHookResult result;bool overflow=false;
    auto drain=[&] {
        if(!reply->isOpen())return;
        result.body+=reply->read(options.maxOutputBytes+1-result.body.size());
        if(result.body.size()>options.maxOutputBytes){overflow=true;reply->abort();loop.quit();}
    };
    QObject::connect(reply.get(),&QNetworkReply::readyRead,&loop,drain);
    QObject::connect(reply.get(),&QNetworkReply::finished,&loop,&QEventLoop::quit);
    QObject::connect(&timer,&QTimer::timeout,&loop,[&]{if(token.isCancelled()||Clock::now()>=deadline){reply->abort();loop.quit();}});
    timer.start();if(!reply->isFinished())loop.exec();drain();
    token.throwIfCancelled();require(!overflow,"HTTP hook response exceeds byte limit",ErrorCode::ResourceLimit);
    require(Clock::now()<deadline,"HTTP hook timed out",ErrorCode::Timeout);
    result.status=reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    require(result.status!=0&&reply->error()!=QNetworkReply::SslHandshakeFailedError,"HTTP hook network or TLS request failed",ErrorCode::RuntimeFailure);
    require(reply->error()==QNetworkReply::NoError||result.status<200||result.status>=300,"HTTP hook response was interrupted",ErrorCode::RuntimeFailure);
    return result;
}
}
