#pragma once
#include "Types.h"
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QSaveFile>
#include <QtCore/QSet>

namespace iiLocalLLM::agent::detail {
// Private ownership overrides for host-owned background execution stores.
// The containing store holds its process ownership lock and mutex. A single
// atomic file changes the owner of a batch without rewriting live job records.
class SessionOwners {
public:
    SessionOwners(QString path,int maximum):path_(std::move(path)),maximum_(maximum) {
        if(QFileInfo(path_).isSymLink())throw Error(ErrorCode::StorageFailure,"Session owner table is a symlink");
        QFile file(path_);if(!file.exists())return;
        if(!file.open(QIODevice::ReadOnly))throw Error(ErrorCode::StorageFailure,"Cannot read session owner table");
        const auto bytes=file.read(8*1024*1024+1);QJsonParseError error;const auto doc=QJsonDocument::fromJson(bytes,&error);
        if(bytes.size()>8*1024*1024||!file.atEnd()||error.error!=QJsonParseError::NoError||!doc.isObject()
            ||doc.object().keys()!=QStringList{"owners","schema"}||doc.object()["schema"]!="iisacc.agent.owners/1"||!doc.object()["owners"].isObject())
            throw Error(ErrorCode::ProtocolError,"Invalid session owner table");
        owners_=doc.object()["owners"].toObject();validate(owners_);
    }
    QString owner(const QString& id,const QString& original) const {return owners_.value(id).toString(original);}
    void validateKnown(const QSet<QString>& ids) const {
        for(auto it=owners_.begin();it!=owners_.end();++it)if(!ids.contains(it.key()))
            throw Error(ErrorCode::ProtocolError,"Session owner table names an unknown execution");
    }
    void transfer(const QStringList& ids,const QString& owner) {
        if(ids.isEmpty())return;
        auto next=owners_;for(const auto& id:ids)next[id]=owner;validate(next);
        if(QFileInfo(path_).isSymLink())throw Error(ErrorCode::StorageFailure,"Session owner table is a symlink");
        const auto bytes=QJsonDocument(QJsonObject{{"schema","iisacc.agent.owners/1"},{"owners",next}}).toJson(QJsonDocument::Compact)+'\n';
        if(bytes.size()>8*1024*1024)throw Error(ErrorCode::ResourceLimit,"Session owner table exceeds byte limit");
        QSaveFile file(path_);file.setDirectWriteFallback(false);
        if(!file.open(QIODevice::WriteOnly)||!file.setPermissions(QFileDevice::ReadOwner|QFileDevice::WriteOwner)
            ||file.write(bytes)!=bytes.size()||!file.commit())throw Error(ErrorCode::StorageFailure,"Cannot publish session owner table");
        owners_=std::move(next);
    }
private:
    QString path_;int maximum_;QJsonObject owners_;
    void validate(const QJsonObject& value) const {
        if(value.size()>maximum_)throw Error(ErrorCode::ResourceLimit,"Session owner table exceeds execution limit");
        for(auto it=value.begin();it!=value.end();++it)
            if(it.key().isEmpty()||it.key().size()>128||it.key().contains(QChar::Null)||!it.value().isString()
                ||it.value().toString().isEmpty()||it.value().toString().size()>512||it.value().toString().contains(QChar::Null))
                throw Error(ErrorCode::ProtocolError,"Invalid session owner identity");
    }
};
}
