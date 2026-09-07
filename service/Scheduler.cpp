#include "Scheduler.h"

namespace iiLocalLLM::detail {

Scheduler::Scheduler(int capacity, std::function<void()> onExit, std::function<void()> maintenance)
    : capacity_(capacity), onExit_(std::move(onExit)), maintenance_(std::move(maintenance)), worker_([this] { loop(); }) {}
Scheduler::~Scheduler() { shutdown(); }
void Scheduler::shutdown()
{
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
        active_.cancel();
        for (auto& item : queue_) item.token.cancel();
    }
    ready_.notify_all();
    if (worker_.joinable()) worker_.join();
}
void Scheduler::enqueue(CancellationToken token, std::function<void()> run, std::function<void(Error)> reject)
{
    ErrorCode code = ErrorCode::None;
    {
        std::lock_guard lock(mutex_);
        if (stopping_) code = ErrorCode::ShuttingDown;
        else if (queue_.size() >= static_cast<size_t>(capacity_)) code = ErrorCode::QueueFull;
        else queue_.push_back({std::move(token), std::move(run), reject});
    }
    if (code != ErrorCode::None) reject(Error(code, enumName(code)));
    else ready_.notify_one();
}
void Scheduler::loop()
{
    for (;;) {
        Item item;
        {
            std::unique_lock lock(mutex_);
            ready_.wait_for(lock, std::chrono::milliseconds(100), [this] { return stopping_ || !queue_.empty(); });
            if (queue_.empty() && stopping_) break;
            if (queue_.empty()) {
                lock.unlock();
                if (maintenance_) maintenance_();
                continue;
            }
            item = std::move(queue_.front());
            queue_.pop_front();
            active_ = item.token;
        }
        if (item.token.isCancelled()) item.reject(Error(ErrorCode::Cancelled, QStringLiteral("Request cancelled before execution")));
        else item.run();
        if (maintenance_) maintenance_();
    }
    if (onExit_) onExit_();
}
} // namespace iiLocalLLM::detail
