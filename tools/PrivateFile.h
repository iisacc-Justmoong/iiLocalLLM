#pragma once
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <stdexcept>
#ifdef Q_OS_UNIX
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace iiLocalLLMClient {
// Credentials stay out of argv, RPC parameters, model input and diagnostics.
inline QByteArray readPrivateFile(const QString& path, qint64 maximum = 65536) {
    QFile file(path); const QFileInfo info(path);
    if (info.isSymLink() || !info.isFile() || !file.open(QIODevice::ReadOnly))
        throw std::runtime_error("Credential file must be an existing regular file, not a symlink");
#ifdef Q_OS_UNIX
    struct stat state{};
    if (::fstat(file.handle(), &state) || !S_ISREG(state.st_mode) || state.st_uid != ::geteuid() || (state.st_mode & 0077))
        throw std::runtime_error("Credential file must be owned by the current user and private (chmod 600)");
#endif
    const auto result = file.read(maximum + 1);
    if (result.isEmpty() || result.size() > maximum || file.error() != QFileDevice::NoError)
        throw std::runtime_error("Credential file is empty, unreadable or too large");
    return result;
}
}
