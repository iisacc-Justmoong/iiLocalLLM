#pragma once
#include "Types.h"
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace iiLocalLLM::detail {

class Scheduler {
public:
    explicit Scheduler(int capacity, std::function<void()> onExit = {}, std::function<void()> maintenance = {});
    ~Scheduler();
    void shutdown();
    // Always settles work: rejected/cancelled items receive an Error in reject().
    void enqueue(CancellationToken cancellation, std::function<void()> run,
                 std::function<void(Error)> reject);
    template<class F> auto submit(F fn) -> std::future<std::invoke_result_t<F, const CancellationToken&>>
    {
        using T = std::invoke_result_t<F, const CancellationToken&>;
        auto promise = std::make_shared<std::promise<T>>();
        auto future = promise->get_future();
        CancellationToken token;
        enqueue(token, [fn = std::move(fn), promise, token]() mutable {
            try {
                token.throwIfCancelled();
                if constexpr (std::is_void_v<T>) { fn(token); promise->set_value(); }
                else { promise->set_value(fn(token)); }
            } catch (...) { promise->set_exception(std::current_exception()); }
        }, [promise](Error error) { promise->set_exception(std::make_exception_ptr(error)); });
        return future;
    }
private:
    struct Item { CancellationToken token; std::function<void()> run; std::function<void(Error)> reject; };
    int capacity_;
    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<Item> queue_;
    CancellationToken active_;
    bool stopping_ = false;
    std::function<void()> onExit_;
    std::function<void()> maintenance_;
    std::thread worker_;
    void loop();
};

} // namespace iiLocalLLM::detail
