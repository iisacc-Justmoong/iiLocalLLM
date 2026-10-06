#pragma once
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#ifdef Q_OS_WIN
#include <windows.h>
#endif
inline bool createNativeTestLink(const QString &target, const QString &link) {
#ifdef Q_OS_WIN
    const DWORD type = QFileInfo(target).isDir() ? SYMBOLIC_LINK_FLAG_DIRECTORY : 0;
    return CreateSymbolicLinkW(reinterpret_cast<LPCWSTR>(link.utf16()),
        reinterpret_cast<LPCWSTR>(target.utf16()), type | 2) != 0;
#else
    return QFile::link(target, link);
#endif
}

inline bool removeNativeTestLink(const QString &link) {
#ifdef Q_OS_WIN
    return QFileInfo(link).isDir() ? RemoveDirectoryW(reinterpret_cast<LPCWSTR>(link.utf16())) != 0
        : DeleteFileW(reinterpret_cast<LPCWSTR>(link.utf16())) != 0;
#else
    return QFile::remove(link);
#endif
}
