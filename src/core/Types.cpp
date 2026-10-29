#include "Types.h"

namespace iiLocalLLM {

Error::Error(ErrorCode code, const QString& message)
    : std::runtime_error(message.toStdString()), code_(code) {}
CancellationToken::CancellationToken() : flag_(std::make_shared<std::atomic_bool>(false)) {}
CancellationToken CancellationToken::linkedTo(const CancellationToken& parent) {
    CancellationToken child; child.parent_ = std::make_shared<CancellationToken>(parent); return child;
}
void CancellationToken::cancel() const noexcept { flag_->store(true, std::memory_order_relaxed); }
bool CancellationToken::isCancelled() const noexcept { return flag_->load(std::memory_order_relaxed) || (parent_ && parent_->isCancelled()); }
void CancellationToken::throwIfCancelled() const
{
    if (isCancelled()) throw Error(ErrorCode::Cancelled, QStringLiteral("Request cancelled"));
}
QString enumName(ErrorCode code)
{
    switch (code) {
    case ErrorCode::None: return QStringLiteral("none");
    case ErrorCode::InvalidArgument: return QStringLiteral("invalid_argument");
    case ErrorCode::NotFound: return QStringLiteral("not_found");
    case ErrorCode::AlreadyExists: return QStringLiteral("already_exists");
    case ErrorCode::ModelInUse: return QStringLiteral("model_in_use");
    case ErrorCode::QueueFull: return QStringLiteral("queue_full");
    case ErrorCode::Cancelled: return QStringLiteral("cancelled");
    case ErrorCode::ContextOverflow: return QStringLiteral("context_overflow");
    case ErrorCode::ResourceLimit: return QStringLiteral("resource_limit");
    case ErrorCode::RuntimeUnavailable: return QStringLiteral("runtime_unavailable");
    case ErrorCode::RuntimeFailure: return QStringLiteral("runtime_failure");
    case ErrorCode::Timeout: return QStringLiteral("timeout");
    case ErrorCode::ConsumerFailure: return QStringLiteral("consumer_failure");
    case ErrorCode::ShuttingDown: return QStringLiteral("shutting_down");
    case ErrorCode::ProtocolError: return QStringLiteral("protocol_error");
    case ErrorCode::InvalidManifest: return QStringLiteral("invalid_manifest");
    case ErrorCode::IntegrityFailure: return QStringLiteral("integrity_failure");
    case ErrorCode::StorageFailure: return QStringLiteral("storage_failure");
    case ErrorCode::Unauthorized: return QStringLiteral("unauthorized");
    }
    return QStringLiteral("unknown");
}
QString enumName(Role role)
{
    switch (role) {
    case Role::System: return QStringLiteral("system");
    case Role::User: return QStringLiteral("user");
    case Role::Assistant: return QStringLiteral("assistant");
    }
    return {};
}
QString enumName(FinishReason reason)
{
    switch (reason) {
    case FinishReason::Stop: return QStringLiteral("stop");
    case FinishReason::Length: return QStringLiteral("length");
    case FinishReason::Cancelled: return QStringLiteral("cancelled");
    case FinishReason::Error: return QStringLiteral("error");
    }
    return {};
}
} // namespace iiLocalLLM
