#pragma once
#include "Types.h"
#include <QtCore/QProcess>
#include <QtCore/QStandardPaths>
#include <QtCore/QProcessEnvironment>
#include <chrono>
#include <exception>
#ifdef Q_OS_WIN
#include <windows.h>
#include <tlhelp32.h>
#endif
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
    const std::function<void(const QByteArray&, bool)>& output,
    const QByteArray& input = {}, const QProcessEnvironment* environment = nullptr,
    const QString& program = {}, const QStringList& arguments = {}, const std::function<void()>& inputClosed = {}) {
    token.throwIfCancelled(); QProcess process; process.setWorkingDirectory(workspace);
    if(environment)process.setProcessEnvironment(*environment);
#ifdef Q_OS_WIN
    process.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *args) {
        args->flags |= CREATE_SUSPENDED;
    });
#endif
#ifdef Q_OS_UNIX
    process.setUnixProcessParameters(QProcess::UnixProcessFlag::CreateNewSession | QProcess::UnixProcessFlag::CloseFileDescriptors);
    process.start(program.isEmpty()?QString("/bin/bash"):program,
        program.isEmpty()?QStringList{"--noprofile", "--norc", "-c", command}:arguments);
#else
    const auto bash = program.isEmpty() ? QStandardPaths::findExecutable("bash") : QString{};
    if (program.isEmpty() && bash.isEmpty())
        throw Error(ErrorCode::RuntimeFailure, "Bash is required on Windows; install Git for Windows and add its bin directory to PATH");
    process.start(program.isEmpty()?bash:program,
        program.isEmpty()?QStringList{"--noprofile", "--norc", "-c", command}:arguments);
#endif
    if (!process.waitForStarted(5000)) throw Error(ErrorCode::RuntimeFailure, "Could not start shell: " + process.errorString());
    const auto pid = process.processId();
    struct Cleanup {
        QProcess& process; qint64 pid; bool stopped = false;
#ifdef Q_OS_WIN
        HANDLE job = nullptr;
#endif
        void stop() noexcept {
            if (stopped) return;
            stopped = true;
#ifdef Q_OS_WIN
            if (job) { TerminateJobObject(job, 1); CloseHandle(job); job = nullptr; }
#endif
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
#ifdef Q_OS_WIN
    cleanup.job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    const HANDLE child = OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE, DWORD(pid));
    const bool confined = cleanup.job && child && SetInformationJobObject(cleanup.job,
        JobObjectExtendedLimitInformation, &limits, sizeof(limits)) && AssignProcessToJobObject(cleanup.job, child);
    if (child) CloseHandle(child);
    if (!confined) throw Error(ErrorCode::RuntimeFailure, "Cannot confine shell child processes in a Windows job");
    // Assign the suspended root before any shell child can be created.
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 entry{}; entry.dwSize = sizeof(entry); bool resumed = false;
    if (snapshot != INVALID_HANDLE_VALUE && Thread32First(snapshot, &entry)) {
        do {
            if (entry.th32OwnerProcessID != DWORD(pid)) continue;
            HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME, FALSE, entry.th32ThreadID);
            if (thread) { resumed = ResumeThread(thread) != DWORD(-1); CloseHandle(thread); }
            if (resumed) break;
        } while (Thread32Next(snapshot, &entry));
    }
    if (snapshot != INVALID_HANDLE_VALUE) CloseHandle(snapshot);
    if (!resumed) throw Error(ErrorCode::RuntimeFailure, "Cannot resume the confined Windows shell");
#endif
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
        if(started)started();
        if(!input.isEmpty()&&process.write(input)!=input.size())throw Error(ErrorCode::RuntimeFailure,"Could not write process input");
        process.closeWriteChannel(); token.throwIfCancelled();if(inputClosed)inputClosed();
        for (;;) {
            process.waitForReadyRead(20); drain(true);
            token.throwIfCancelled();
            if (process.state() == QProcess::NotRunning) break;
            if (std::chrono::steady_clock::now() >= deadline) throw Error(ErrorCode::Timeout, "Shell command timed out");
        }
        bool crashed = process.exitStatus() == QProcess::CrashExit;
#ifdef Q_OS_WIN
        const auto code = quint32(process.exitCode());
        // MSYS reports a self-directed POSIX signal as a shifted wait status.
        crashed = crashed || (program.isEmpty() && code >= 256 && code <= 0x7f00 && (code & 0xff) == 0)
            || (code >= 0x80000000);
#endif
        const ShellExit result{process.exitCode(), crashed};
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
