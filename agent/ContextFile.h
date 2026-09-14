#pragma once
#include "../Types.h"
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#ifdef Q_OS_UNIX
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
namespace iiLocalLLM::agent::detail {
// Callers resolve canonical paths and enforce their own discovery roots first.
inline QByteArray readContextFile(const QString& root, const QString& path, int maxBytes, const CancellationToken& token) {
    token.throwIfCancelled();
    const auto relative = QDir(root).relativeFilePath(path);
    if (relative == ".." || relative.startsWith("../") || QDir::isAbsolutePath(relative))
        throw Error(ErrorCode::InvalidArgument, "Context file escapes its root");
    QFile file(path);
#ifdef Q_OS_UNIX
    int fd = ::open(QFile::encodeName(root).constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) throw Error(ErrorCode::StorageFailure, "Cannot open context root");
    const auto parts = relative.split('/');
    for (qsizetype i = 0; i < parts.size(); ++i) {
        const int next = ::openat(fd, QFile::encodeName(parts[i]).constData(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW
            | (i + 1 < parts.size() ? O_DIRECTORY : O_NONBLOCK));
        ::close(fd); fd = next;
        if (fd < 0) throw Error(ErrorCode::StorageFailure, "Cannot safely open context file");
    }
    struct stat status{};
    if (::fstat(fd, &status) || !S_ISREG(status.st_mode)) { ::close(fd); throw Error(ErrorCode::StorageFailure, "Context is not a regular file"); }
    if (!file.open(fd, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) { ::close(fd); throw Error(ErrorCode::StorageFailure, "Cannot read context file"); }
#else
    if (!QFileInfo(path).isFile() || !file.open(QIODevice::ReadOnly)) throw Error(ErrorCode::StorageFailure, "Cannot read context file");
#endif
    const auto bytes = file.read(qint64(maxBytes) + 1);
    token.throwIfCancelled();
    if (file.error() != QFileDevice::NoError) throw Error(ErrorCode::StorageFailure, "Cannot read context file");
    if (bytes.size() > maxBytes || !file.atEnd()) throw Error(ErrorCode::ResourceLimit, "Context file exceeds byte limit");
    return bytes;
}
}
