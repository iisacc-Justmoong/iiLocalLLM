#include "ClientTransport.h"
#include "Protocol.h"
#include <QtCore/QProcess>
#include <chrono>
#include <thread>
#ifdef Q_OS_UNIX
#include <signal.h>
#endif

namespace iiLocalLLM::mcp::detail {
namespace {
class StdioTransport final : public ClientTransport {
    StdioOptions options;
    std::unique_ptr<QProcess> process;
    QByteArray input, stderrBytes;
    qint64 processGroup = 0;
public:
    explicit StdioTransport(StdioOptions o) : options(std::move(o)) {
        require(!options.program.isEmpty() && !options.program.contains(QChar::Null)
            && options.maxStderrBytes >= 0, "Invalid MCP process", ErrorCode::InvalidArgument);
        for (const auto& arg : options.arguments)
            require(!arg.contains(QChar::Null), "NUL in MCP process argument", ErrorCode::InvalidArgument);
    }
    void start() override {
        process = std::make_unique<QProcess>();
        process->setProcessEnvironment(options.environment);
        process->setWorkingDirectory(options.workingDirectory);
        process->setProcessChannelMode(QProcess::SeparateChannels);
#ifdef Q_OS_UNIX
        process->setUnixProcessParameters(QProcess::UnixProcessFlag::CreateNewSession | QProcess::UnixProcessFlag::CloseFileDescriptors);
#endif
        process->start(options.program, options.arguments);
        require(process->waitForStarted(options.initializeTimeoutMs), "Cannot start MCP server: " + process->errorString(), ErrorCode::RuntimeUnavailable);
        processGroup = process->processId();
    }
    void send(const QByteArray& frame) override {
        require(process->bytesToWrite() + frame.size() <= options.maxQueuedBytes,
            "MCP subprocess stopped consuming its input", ErrorCode::ResourceLimit);
        qint64 offset = 0;
        while (offset < frame.size()) {
            const auto size = process->write(frame.constData() + offset, frame.size() - offset);
            require(size > 0, "MCP process write failed", ErrorCode::RuntimeFailure); offset += size;
        }
    }
    QList<TransportEvent> poll() override {
        process->waitForReadyRead(10);
        stderrBytes = (stderrBytes + process->readAllStandardError()).right(options.maxStderrBytes);
        QList<TransportEvent> result; qsizetype bytes = 0;
        while (process->bytesAvailable() > 0) {
            input += process->read(65536);
            qsizetype newline;
            while ((newline = input.indexOf('\n')) >= 0) {
                require(newline <= options.maxMessageBytes, "MCP frame exceeds limit", ErrorCode::ResourceLimit);
                bytes += newline;
                require(bytes <= options.maxQueuedBytes, "MCP input exceeds queue limit", ErrorCode::ResourceLimit);
                result.append({input.first(newline), {}, {}, false}); input.remove(0, newline + 1);
            }
            require(input.size() <= options.maxMessageBytes, "MCP frame exceeds limit", ErrorCode::ResourceLimit);
        }
        if (process->state() == QProcess::NotRunning) {
            const auto error = std::make_exception_ptr(Error(input.isEmpty() ? ErrorCode::RuntimeFailure : ErrorCode::ProtocolError,
                input.isEmpty() ? "MCP server exited (code " + QString::number(process->exitCode()) + ")" : "MCP server exited with an unterminated frame"));
            result.append({{}, {}, error, false});
        }
        return result;
    }
    QByteArray diagnostics() const override { return stderrBytes; }
    void close() override {
        if (!process) return;
        if (process->state() != QProcess::NotRunning) {
            process->closeWriteChannel();
            if (!process->waitForFinished(options.shutdownTimeoutMs)) {
#ifdef Q_OS_UNIX
                if (processGroup > 0) ::kill(-pid_t(processGroup), SIGTERM);
#else
                process->terminate();
#endif
                if (!process->waitForFinished(options.shutdownTimeoutMs)) {
#ifdef Q_OS_UNIX
                    if (processGroup > 0) ::kill(-pid_t(processGroup), SIGKILL);
#else
                    process->kill();
#endif
                    process->waitForFinished(1000);
                }
            }
        }
#ifdef Q_OS_UNIX
        if (processGroup > 0 && ::kill(-pid_t(processGroup), SIGTERM) == 0) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(options.shutdownTimeoutMs);
            while (::kill(-pid_t(processGroup), 0) == 0 && std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            ::kill(-pid_t(processGroup), SIGKILL);
        }
#endif
        process.reset();
    }
};
}
std::unique_ptr<ClientTransport> stdioTransport(StdioOptions options) { return std::make_unique<StdioTransport>(std::move(options)); }
}
