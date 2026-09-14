#pragma once
#include "Types.h"
#include <QtCore/QProcess>
#include <chrono>
#include <exception>
#ifdef Q_OS_UNIX
#include <signal.h>
#include <unistd.h>
#endif

namespace iiLocalLLM::agent::detail {
struct ShellExit { int code; bool crashed; };
// QProcess and its pipes remain on the calling worker thread. Both foreground
// and background execution use the same process-group cleanup and input EOF.
inline ShellExit shellProcess(const QString& workspace, const QString& command, int timeoutMs,
    const CancellationToken& token, const std::function<void()>& started,
    const std::function<void(const QByteArray&, bool)>& output) {
    token.throwIfCancelled(); QProcess process; process.setWorkingDirectory(workspace);
#ifdef Q_OS_UNIX
    process.setUnixProcessParameters(QProcess::UnixProcessFlag::CreateNewSession | QProcess::UnixProcessFlag::CloseFileDescriptors);
    process.start("/bin/bash", {"--noprofile", "--norc", "-c", command});
#else
    process.start("cmd.exe", {"/D", "/S", "/C", command});
#endif
    if (!process.waitForStarted(5000)) throw Error(ErrorCode::RuntimeFailure, "Could not start shell: " + process.errorString());
    const auto pid = process.processId();
    struct Cleanup {
        QProcess& process; qint64 pid; bool stopped = false;
        void stop() noexcept {
            if (stopped) return;
            stopped = true;
#ifdef Q_OS_UNIX
            ::kill(-pid_t(pid), SIGTERM);
#endif
            if (process.state() != QProcess::NotRunning) { process.terminate(); process.waitForFinished(200); }
#ifdef Q_OS_UNIX
            ::kill(-pid_t(pid), SIGKILL);
#endif
            if (process.state() != QProcess::NotRunning) { process.kill(); process.waitForFinished(1000); }
        }
        ~Cleanup() { stop(); }
    } cleanup{process, pid};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    auto drain = [&](bool checkDeadline) {
        for (const auto channel : {QProcess::StandardOutput, QProcess::StandardError}) {
            process.setReadChannel(channel);
            while (process.bytesAvailable()) {
                if (checkDeadline) {
                    token.throwIfCancelled();
                    if (std::chrono::steady_clock::now() >= deadline) throw Error(ErrorCode::Timeout, "Shell command timed out");
                }
                output(process.read(65536), channel == QProcess::StandardError);
            }
        }
    };
    try {
        process.closeWriteChannel(); token.throwIfCancelled(); if (started) started();
        for (;;) {
            process.waitForReadyRead(20); drain(true);
            token.throwIfCancelled();
            if (process.state() == QProcess::NotRunning) break;
            if (std::chrono::steady_clock::now() >= deadline) throw Error(ErrorCode::Timeout, "Shell command timed out");
        }
        const ShellExit result{process.exitCode(), process.exitStatus() == QProcess::CrashExit};
        cleanup.stop(); drain(false); return result;
    } catch (...) {
        const auto error = std::current_exception(); cleanup.stop();
        // TERM handlers can write while cleanup waits. Preserve those bytes up
        // to the output sink's limit without replacing the original failure.
        try { drain(false); } catch (...) {}
        std::rethrow_exception(error);
    }
}
}
