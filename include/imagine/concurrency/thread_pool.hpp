#pragma once

#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <future>
#include <atomic>
#include <type_traits>
#include <memory>
#include <concepts>
#include <stdexcept>
#include <algorithm>

namespace imagine::concurrency {

class ThreadPool {
public:
    explicit ThreadPool(size_t threads = std::max(1u, std::thread::hardware_concurrency()));
    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    ThreadPool(ThreadPool&&) = delete;
    ThreadPool& operator=(ThreadPool&&) = delete;

    template <typename F, typename... Args>
    auto enqueue(F&& f, Args&&... args)
        -> std::future<std::invoke_result_t<std::decay_t<F>, std::decay_t<Args>...>> {
        using ReturnType = std::invoke_result_t<std::decay_t<F>, std::decay_t<Args>...>;

        auto task = std::make_shared<std::packaged_task<ReturnType()>>(
            [func = std::forward<F>(f), ...capturedArgs = std::forward<Args>(args)]() mutable {
                return std::invoke(std::move(func), std::move(capturedArgs)...);
            }
        );

        std::future<ReturnType> future = task->get_future();

        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            if (stop_.load()) {
                throw std::runtime_error("Cannot enqueue task to stopped ThreadPool");
            }
            tasks_.emplace([task]() { (*task)(); });
        }

        cv_.notify_one();
        return future;
    }

    void waitAll();
    void stop();

    size_t size() const noexcept;
    size_t activeTasks() const noexcept;
    size_t queueSize() const;
    bool isStopped() const noexcept;

private:
    void workerLoop();

    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> tasks_;

    mutable std::mutex queue_mutex_;
    std::condition_variable cv_;
    std::condition_variable wait_cv_;

    std::atomic<size_t> active_tasks_{0};
    std::atomic<bool> stop_{false};
};

} // namespace imagine::concurrency

namespace imagine {
using concurrency::ThreadPool;
}
