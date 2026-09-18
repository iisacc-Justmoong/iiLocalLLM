#pragma once
#include "../Types.h"
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QLockFile>
#include <QtCore/QSaveFile>
#include <QtCore/QUuid>
#include <chrono>
#include <optional>
#include <thread>
#ifdef Q_OS_UNIX
#include <sys/file.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#endif

namespace iiLocalLLM::agent::detail {
// An advisory lock plus an atomic replacement of one settings file. POSIX
// writes stay anchored to the opened directory, including across rename races.
class PermissionSettingsFile {
    QString path_,name_;int maximum_;
    std::optional<QByteArray> before_;
#ifdef Q_OS_UNIX
    struct Descriptor {
        int value=-1;~Descriptor(){if(value>=0)::close(value);}
        void reset(int next){if(value>=0)::close(value);value=next;}
    } directory_,lock_;
#else
    std::unique_ptr<QLockFile> lock_;
#endif
    static void require(bool value,const QString& text) {if(!value)throw Error(ErrorCode::StorageFailure,text);}
    std::optional<QByteArray> read() const {
#ifdef Q_OS_UNIX
        const int fd=::openat(directory_.value,QFile::encodeName(name_).constData(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
        if(fd<0){if(errno==ENOENT)return std::nullopt;throw Error(ErrorCode::StorageFailure,"Cannot safely read permission settings");}
        struct stat status{};
        if(::fstat(fd,&status)||!S_ISREG(status.st_mode)){::close(fd);throw Error(ErrorCode::StorageFailure,"Permission settings must be a regular file");}
        QFile file;if(!file.open(fd,QIODevice::ReadOnly,QFileDevice::AutoCloseHandle)){::close(fd);throw Error(ErrorCode::StorageFailure,"Cannot read permission settings");}
#else
        QFileInfo info(path_);if(!info.exists()&&!info.isSymLink())return std::nullopt;
        require(!info.isSymLink()&&info.isFile(),"Permission settings must be a regular file");
        QFile file(path_);require(file.open(QIODevice::ReadOnly),"Cannot read permission settings");
#endif
        const auto data=file.read(qint64(maximum_)+1);require(file.error()==QFileDevice::NoError,"Cannot read permission settings");
        if(data.size()>maximum_||!file.atEnd())throw Error(ErrorCode::ResourceLimit,"Permission settings exceed file limit");return data;
    }
public:
    PermissionSettingsFile(const QString& root,const QString& path,int maximum,int timeoutMs,const CancellationToken& token)
        :path_(QDir::cleanPath(path)),name_(QFileInfo(path).fileName()),maximum_(maximum) {
        token.throwIfCancelled();const auto relative=QDir(root).relativeFilePath(path_);
        require(relative!=".."&&!relative.startsWith("../")&&!QDir::isAbsolutePath(relative),"Permission settings escape authority root");
        require(QDir().mkpath(root),"Cannot create permission settings authority directory");
#ifdef Q_OS_UNIX
        directory_.reset(::open(QFile::encodeName(QFileInfo(root).canonicalFilePath()).constData(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW));
        require(directory_.value>=0,"Cannot open permission settings authority directory");
        const auto parts=relative.split('/');
        for(qsizetype i=0;i+1<parts.size();++i) {
            token.throwIfCancelled();const auto part=QFile::encodeName(parts[i]);
            if(::mkdirat(directory_.value,part.constData(),0700)&&errno!=EEXIST)throw Error(ErrorCode::StorageFailure,"Cannot create permission settings directory");
            const int next=::openat(directory_.value,part.constData(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
            require(next>=0,"Cannot safely open permission settings directory");directory_.reset(next);
        }
        const auto name=QFile::encodeName("."+name_+".iillm-permissions.lock");
        const auto until=std::chrono::steady_clock::now()+std::chrono::milliseconds(timeoutMs);
        const int flags=O_RDWR|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK;
        for(;;) {
            token.throwIfCancelled();lock_.reset(::openat(directory_.value,name.constData(),flags));
            if(lock_.value>=0)break;
            int error=errno;
            if(error==ENOENT) {
                // Separate creation from reopening. Concurrent O_CREAT opens
                // can report ENOENT on the tested APFS host during first use.
                lock_.reset(::openat(directory_.value,name.constData(),flags|O_CREAT|O_EXCL,0600));
                if(lock_.value>=0)break;error=errno;
            }
            require(error==EEXIST||error==ENOENT||error==EINTR,"Cannot open permission settings lock (errno "+QString::number(error)+")");
            if(std::chrono::steady_clock::now()>=until)throw Error(ErrorCode::Timeout,"Permission settings lock open timeout");
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        struct stat status{};
        require(::fstat(lock_.value,&status)==0&&S_ISREG(status.st_mode)&&status.st_nlink==1,"Invalid permission settings lock");
        while(::flock(lock_.value,LOCK_EX|LOCK_NB)) {
            require(errno==EAGAIN||errno==EWOULDBLOCK||errno==EINTR,"Cannot lock permission settings");token.throwIfCancelled();
            if(std::chrono::steady_clock::now()>=until)throw Error(ErrorCode::Timeout,"Permission settings lock timeout");
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
#else
        auto current=QDir(root).absolutePath();const auto parts=relative.split('/');
        for(qsizetype i=0;i+1<parts.size();++i){current+='/'+parts[i];require(!QFileInfo(current).isSymLink()&&QDir().mkpath(current),"Invalid permission settings directory");}
        lock_=std::make_unique<QLockFile>(QFileInfo(path_).absolutePath()+"/."+name_+".iillm-permissions.lock");
        const auto until=std::chrono::steady_clock::now()+std::chrono::milliseconds(timeoutMs);
        while(!lock_->tryLock(0)){require(lock_->error()==QLockFile::LockFailedError,"Cannot lock permission settings");token.throwIfCancelled();
            if(std::chrono::steady_clock::now()>=until)throw Error(ErrorCode::Timeout,"Permission settings lock timeout");std::this_thread::sleep_for(std::chrono::milliseconds(10));}
#endif
        token.throwIfCancelled();before_=read();
    }
    const std::optional<QByteArray>& bytes()const{return before_;}
    void commit(const QByteArray& data) {
        if(before_&&*before_==data)return;
        require(data.size()<=maximum_,"Updated permission settings exceed file limit");
        require(read()==before_,"Permission settings changed outside the update lock");
#ifdef Q_OS_UNIX
        const auto temporary=QFile::encodeName("."+name_+"."+QUuid::createUuid().toString(QUuid::WithoutBraces)+".tmp");
        struct Cleanup {int directory;QByteArray name;~Cleanup(){if(!name.isEmpty())::unlinkat(directory,name.constData(),0);}} cleanup{directory_.value,temporary};
        Descriptor file;file.reset(::openat(directory_.value,temporary.constData(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600));
        require(file.value>=0,"Cannot create permission settings replacement");qsizetype offset=0;
        while(offset<data.size()) {const auto written=::write(file.value,data.constData()+offset,size_t(data.size()-offset));
            if(written<0&&errno==EINTR)continue;require(written>0,"Cannot write permission settings replacement");offset+=written;}
        require(::fsync(file.value)==0,"Cannot synchronize permission settings replacement");
        require(read()==before_,"Permission settings changed before publication");
        require(::renameat(directory_.value,temporary.constData(),directory_.value,QFile::encodeName(name_).constData())==0,"Cannot publish permission settings replacement");
        cleanup.name.clear();
        // Some filesystems do not support directory fsync. Publication is still
        // atomic there; crash durability depends on that filesystem.
        if(::fsync(directory_.value)&&errno!=EINVAL&&errno!=ENOTSUP)throw Error(ErrorCode::StorageFailure,"Cannot synchronize permission settings directory after publication");
#else
        QSaveFile file(path_);file.setDirectWriteFallback(false);
        require(file.open(QIODevice::WriteOnly),"Cannot create permission settings replacement");
        require(file.setPermissions(QFileDevice::ReadOwner|QFileDevice::WriteOwner)&&file.write(data)==data.size(),"Cannot write permission settings replacement");
        require(read()==before_&&file.commit(),"Cannot publish permission settings replacement");
#endif
        before_=data;
    }
};
}
