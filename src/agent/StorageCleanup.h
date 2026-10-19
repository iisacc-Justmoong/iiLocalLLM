#pragma once
#include <iiFileProviderDevice.h>
#include <iiFileProviderPresentation.h>

namespace iiLocalLLM::agent::detail {
// Rollback owns these private directories. Stop derivative publication before
// removing them so an active preview cannot recreate a partially removed tree.
inline bool removeOwnedDirectory(const QString &path) noexcept {
    try {
        iiFileProvider::native::Presentation::cancelPending(std::filesystem::path(path.toStdU16String()));
        return iiFileProvider::Directory(path).removeRecursively();
    } catch (...) {
        return false;
    }
}
}
