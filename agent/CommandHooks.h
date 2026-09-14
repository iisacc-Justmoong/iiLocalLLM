#pragma once
#include "Tools.h"
#include <QtCore/QProcessEnvironment>

namespace iiLocalLLM::agent {
struct CommandHookOptions {
    QString workingDirectory;
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    int timeoutMs = 600000;
    int maxHooks = 128;
    int maxConcurrentProcesses = 4;
    int maxInputBytes = 1024 * 1024;
    int maxOutputBytes = 1024 * 1024;
    int maxOnceEntries = 32768;
};
// Explicit host configuration, frozen at construction. Commands receive JSON
// on stdin; model data is never substituted into the shell command string.
class IILOCALLLM_EXPORT CommandHooks {
public:
    CommandHooks(QJsonObject settings, CommandHookOptions);
    Hook callback() const;
    QJsonObject describe() const; // Event/matcher metadata; no commands or environment values.
private:
    class Impl;
    std::shared_ptr<Impl> d;
};
}
