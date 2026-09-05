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

/**
 * @brief Thread-safe fixed-size worker thread pool for asynchronous task execution.
 *
 * Concurrency & Lifecycle Model:
 * - Thread pool creates a fixed number of worker threads on initialization.
 * - Tasks are enqueued via enqueue(), which returns a std::future for result retrieval.
 * - Calling stop() or destroying the pool initiates a graceful shutdown: queued tasks
 *   are executed to completion by worker threads before joining, but new tasks are rejected.
 * - Calling waitAll() blocks until all queued and executing tasks have finished.
 * - Calling waitAll() or stop() from inside a task running on a pool worker thread
 *   is prohibited and throws std::logic_error to prevent self-deadlock.
 */
class ThreadPool {
public:
    /**
     * @brief Constructs a ThreadPool with the specified number of worker threads.
     * @param threads Number of worker threads (defaults to hardware concurrency, at least 1).
     */
    explicit ThreadPool(size_t threads = std::max(1u, std::thread::hardware_concurrency()));

    /**
     * @brief Destructor. Initiates graceful shutdown and waits for queued tasks to finish.
     */
    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    ThreadPool(ThreadPool&&) = delete;
    ThreadPool& operator=(ThreadPool&&) = delete;

    /**
     * @brief Enqueues a callable task and its arguments for asynchronous execution.
     *
     * Semantics & Ownership:
     * - The callable and arguments are forwarded and stored. When executed, they are
     *   moved into std::invoke to transfer ownership into the task invocation, supporting
     *   move-only arguments (e.g. std::unique_ptr) and avoiding copies.
     *
     * Shutdown Behavior:
     * - Synchronized via queue_mutex_.
     * - If enqueue acquires the mutex before stop() has initiated, the task is accepted
     *   and will be processed even if shutdown commences right after.
     * - If stop() has already initiated, enqueue throws std::runtime_error.
     *
     * @throws std::runtime_error if the ThreadPool has been stopped.
     * @return std::future holding the return value or exception of the task.
     */
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
            std::lock_guard<std::mutex> lock(queue_mutex_);
            if (stop_.load(std::memory_order_relaxed)) {
                throw std::runtime_error("Cannot enqueue task to stopped ThreadPool");
            }
            tasks_.emplace([task]() { (*task)(); });
        }

        cv_.notify_one();
        return future;
    }

    /**
     * @brief Blocks until all currently queued and executing tasks have completed.
     *
     * If stop() has been initiated, waitAll() waits for any remaining queued tasks
     * to drain. Multiple threads may safely call waitAll() concurrently.
     *
     * @note Must NOT be called from a worker thread belonging to this ThreadPool.
     * @throws std::logic_error if called from a worker thread in this pool (deadlock prevention).
     */
    void waitAll();

    /**
     * @brief Initiates graceful shutdown and blocks until all worker threads join.
     *
     * Already queued tasks will be executed to completion before workers terminate.
     * Subsequent calls to enqueue() will throw std::runtime_error.
     * Calling stop() multiple times or concurrently from multiple threads is safe and idempotent.
     *
     * @note Must NOT be called from a worker thread belonging to this ThreadPool.
     * @throws std::logic_error if called from a worker thread in this pool (deadlock prevention).
     */
    void stop();

    /**
     * @brief Returns the configured number of worker threads in the pool.
     * Safe to call concurrently at any time, including during or after stop()/destruction.
     */
    size_t size() const noexcept;

    /**
     * @brief Returns the number of tasks currently actively executing on worker threads.
     */
    size_t activeTasks() const;

    /**
     * @brief Returns the number of tasks waiting in the queue.
     */
    size_t queueSize() const;

    /**
     * @brief Returns true if stop() has been initiated.
     */
    bool isStopped() const noexcept;

private:
    void workerLoop();
    bool isWorkerThread() const noexcept;

    const size_t worker_count_;
    std::vector<std::thread> workers_;
    std::vector<std::thread::id> worker_ids_;
    std::queue<std::function<void()>> tasks_;

    mutable std::mutex queue_mutex_;
    std::condition_variable cv_;
    std::condition_variable wait_cv_;

    size_t active_tasks_{0};
    std::atomic<bool> stop_{false};
    std::once_flag stop_flag_;
};

} // namespace imagine::concurrency

namespace imagine {
using concurrency::ThreadPool;
}
