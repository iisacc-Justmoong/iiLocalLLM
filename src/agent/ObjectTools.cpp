#include "ObjectTools.h"
#include <QtCore/QCoreApplication>
#include <QtCore/QPointer>
#include <QtCore/QThread>
#include <chrono>
#include <condition_variable>
#include <mutex>

namespace iiLocalLLM::agent {
namespace {
struct BoundObject {
    QPointer<QObject> owner; // Dereferenced only on the application's thread.
    ObjectToolHandler handler;
};
struct Dispatch {
    std::mutex mutex;
    std::condition_variable wake;
    bool abandoned = false, started = false, done = false;
    ToolResult result;
    std::exception_ptr failure;
};
ToolResult invoke(const BoundObject& bound, const QJsonObject& arguments, const ToolContext& context) {
    context.cancellation.throwIfCancelled();
    if (!bound.owner || bound.owner->thread() != QThread::currentThread())
        throw Error(ErrorCode::RuntimeUnavailable, "Application controller is no longer available on the application thread");
    return bound.handler(*bound.owner, arguments, context);
}
}
Tool objectTool(QObject* owner, ToolDefinition definition, ObjectToolHandler handler, int timeoutMs) {
    auto* application = QCoreApplication::instance();
    if (!application || QThread::currentThread() != application->thread() || !owner
        || owner->thread() != application->thread() || !handler || timeoutMs < 1 || timeoutMs > 300000)
        throw Error(ErrorCode::InvalidArgument, "Object tools must be bound on the application thread with a live controller");
    auto bound = std::make_shared<BoundObject>(BoundObject{owner, std::move(handler)});
    Tool tool; tool.definition = std::move(definition);
    tool.execute = [application, bound, timeoutMs](const QJsonObject& arguments, const ToolContext& context) {
        context.cancellation.throwIfCancelled();
        if (QThread::currentThread() == application->thread()) return invoke(*bound, arguments, context);
        auto dispatch = std::make_shared<Dispatch>();
        if (!QMetaObject::invokeMethod(application, [bound, dispatch, arguments, context] {
                { std::lock_guard lock(dispatch->mutex);
                  if (dispatch->abandoned) return;
                  dispatch->started = true; }
                ToolResult result; std::exception_ptr failure;
                try { result = invoke(*bound, arguments, context); }
                catch (...) { failure = std::current_exception(); }
                { std::lock_guard lock(dispatch->mutex);
                  dispatch->result = std::move(result); dispatch->failure = failure; dispatch->done = true; }
                dispatch->wake.notify_all();
            }, Qt::QueuedConnection))
            throw Error(ErrorCode::RuntimeUnavailable, "Could not queue the application tool");
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        std::unique_lock lock(dispatch->mutex);
        while (!dispatch->done) {
            if (context.cancellation.isCancelled()) {
                dispatch->abandoned = true;
                context.cancellation.throwIfCancelled();
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                dispatch->abandoned = true;
                return ToolResult{dispatch->started ? "Application tool timed out after dispatch; its effect may have occurred."
                    : "Application did not respond before the deadline; the queued tool was discarded.",
                    {{"dispatch_started", dispatch->started}}, true};
            }
            dispatch->wake.wait_for(lock, std::chrono::milliseconds(10));
        }
        if (dispatch->failure) std::rethrow_exception(dispatch->failure);
        return dispatch->result;
    };
    return tool;
}
}
