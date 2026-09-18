#include "Server.h"
#include "Protocol.h"
#include <QtCore/QStringDecoder>
#include <cerrno>
#ifdef Q_OS_UNIX
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#endif

namespace iiLocalLLM::mcp {
#ifdef Q_OS_UNIX
namespace {
class NonblockingFd {
public:
    explicit NonblockingFd(int fd) : fd_(fd), flags_(::fcntl(fd, F_GETFL)) {
        detail::require(flags_ >= 0 && ::fcntl(fd_, F_SETFL, flags_ | O_NONBLOCK) == 0,
            "Cannot configure MCP stdio descriptor", ErrorCode::RuntimeUnavailable);
    }
    ~NonblockingFd() { ::fcntl(fd_, F_SETFL, flags_); }
private:
    int fd_, flags_;
};
class PipeSignalMask {
public:
    PipeSignalMask() {
        sigemptyset(&set_); sigaddset(&set_, SIGPIPE);
        sigset_t pending; ::sigpending(&pending); hadPending_ = sigismember(&pending, SIGPIPE);
        valid_ = ::pthread_sigmask(SIG_BLOCK, &set_, &before_) == 0;
        detail::require(valid_, "Cannot block stdio SIGPIPE", ErrorCode::RuntimeFailure);
    }
    ~PipeSignalMask() {
        if (!valid_) return;
        if (!hadPending_ && !sigismember(&before_, SIGPIPE)) {
            sigset_t pending; ::sigpending(&pending);
            if (sigismember(&pending, SIGPIPE)) { int signal; ::sigwait(&set_, &signal); }
        }
        ::pthread_sigmask(SIG_SETMASK, &before_, nullptr);
    }
private:
    sigset_t set_{}, before_{};
    bool hadPending_ = false, valid_ = false;
};
}
#endif
void serveStdio(ServerOptions options, CancellationToken cancellation, int inputFd, int outputFd) {
#ifdef Q_OS_UNIX
    detail::require(inputFd >= 0 && outputFd >= 0 && inputFd != outputFd, "Invalid MCP stdio descriptors", ErrorCode::InvalidArgument);
    const int maxFrame = options.maxMessageBytes, maxQueued = options.maxQueuedBytes;
    NonblockingFd input(inputFd), output(outputFd);
    ServerSession session(std::move(options)); QByteArray incoming, outgoing; qsizetype written = 0;
    while (!cancellation.isCancelled()) {
        if (outgoing.isEmpty()) {
            for (const auto& message : session.takeMessages()) outgoing += detail::encodeValue(message, maxFrame);
            written = 0;
        }
        if (session.isClosed()) { session.takeMessages(); break; }
        if (outgoing.isEmpty()) {
            // Darwin does not report a closed pipe reader when events is zero.
            // Probe separately so an idle writable pipe cannot spin the main poll.
            pollfd outputState{outputFd, POLLOUT, 0};
            const int ready = ::poll(&outputState, 1, 0);
            if (ready < 0 && errno == EINTR) continue;
            detail::require(ready >= 0, "MCP stdout health check failed", ErrorCode::RuntimeFailure);
            if (outputState.revents & (POLLERR | POLLHUP)) break;
            detail::require(!(outputState.revents & POLLNVAL), "MCP output descriptor failed", ErrorCode::RuntimeFailure);
        }
        pollfd descriptors[]{{inputFd, POLLIN, 0}, {outputFd, short(outgoing.isEmpty() ? 0 : POLLOUT), 0}};
        const int ready = ::poll(descriptors, 2, 10);
        if (ready < 0 && errno == EINTR) continue;
        detail::require(ready >= 0, "MCP stdio poll failed", ErrorCode::RuntimeFailure);
        if (descriptors[0].revents & (POLLIN | POLLHUP)) {
            char buffer[65536]; const auto count = ::read(inputFd, buffer, sizeof(buffer));
            if (count == 0) {
                detail::require(incoming.isEmpty(), "MCP stdin ended inside a frame", ErrorCode::ProtocolError); break;
            }
            if (count < 0) detail::require(errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR, "MCP stdin read failed", ErrorCode::RuntimeFailure);
            else incoming.append(buffer, count);
            qsizetype newline;
            while ((newline = incoming.indexOf('\n')) >= 0) {
                detail::require(newline <= maxFrame, "MCP input frame exceeds limit", ErrorCode::ResourceLimit);
                const auto frame = incoming.first(newline); incoming.remove(0, newline + 1);
                QStringDecoder decoder(QStringDecoder::Utf8); const QString utf8 = decoder(frame); Q_UNUSED(utf8);
                QJsonParseError error; const auto document = QJsonDocument::fromJson(frame, &error);
                if (decoder.hasError() || error.error != QJsonParseError::NoError) {
                    outgoing += detail::encode(detail::rpcError(QJsonValue::Null, -32700, "Invalid UTF-8 JSON frame"), maxFrame);
                } else if (document.isArray()) session.receiveBatch(document.array());
                else if (!document.isObject()) {
                    outgoing += detail::encode(detail::rpcError(QJsonValue::Null, -32600, "MCP expects a JSON-RPC object"), maxFrame);
                } else session.receive(document.object());
                detail::require(outgoing.size() <= maxQueued, "MCP stdio output exceeds limit", ErrorCode::ResourceLimit);
            }
            detail::require(incoming.size() <= maxFrame, "MCP input frame exceeds limit", ErrorCode::ResourceLimit);
        }
        detail::require(!(descriptors[0].revents & (POLLERR | POLLNVAL)), "MCP input descriptor failed", ErrorCode::RuntimeFailure);
        if (descriptors[1].revents & (POLLERR | POLLHUP)) break;
        detail::require(!(descriptors[1].revents & POLLNVAL), "MCP output descriptor failed", ErrorCode::RuntimeFailure);
        if (!outgoing.isEmpty() && (descriptors[1].revents & POLLOUT)) {
            ssize_t count; int writeError = 0;
            {
                // Only the writing thread needs this mask; workers and tools
                // must retain their inherited signal behavior.
                PipeSignalMask pipeSignals;
                count = ::write(outputFd, outgoing.constData() + written, size_t(outgoing.size() - written));
                if (count < 0) writeError = errno;
            }
            if (count < 0 && writeError == EPIPE) break;
            if (count < 0) detail::require(writeError == EAGAIN || writeError == EWOULDBLOCK || writeError == EINTR, "MCP stdout write failed", ErrorCode::RuntimeFailure);
            else written += count;
            if (written == outgoing.size()) { outgoing.clear(); written = 0; }
        }
    }
    session.close();
#else
    Q_UNUSED(options); Q_UNUSED(cancellation); Q_UNUSED(inputFd); Q_UNUSED(outputFd);
    throw Error(ErrorCode::RuntimeUnavailable, "This platform requires a ServerSession transport adapter; POSIX stdio is unavailable");
#endif
}
}
