#include "imagine/concurrency/thread_pool.hpp"
#include "imagine/common/logger.hpp"

namespace imagine::concurrency {

namespace {
thread_local const ThreadPool* t_active_worker_pool{nullptr};

struct WorkerScopeGuard {
    explicit WorkerScopeGuard(const ThreadPool* pool) noexcept {
        t_active_worker_pool = pool;
    }
    ~WorkerScopeGuard() noexcept {
        t_active_worker_pool = nullptr;
    }
    WorkerScopeGuard(const WorkerScopeGuard&) = delete;
    WorkerScopeGuard& operator=(const WorkerScopeGuard&) = delete;
};
} // namespace

ThreadPool::ThreadPool(size_t threads)
    : worker_count_(threads == 0 ? 1 : threads) {
    workers_.reserve(worker_count_);
    try {
        for (size_t i = 0; i < worker_count_; ++i) {
            workers_.emplace_back([this]() {
                workerLoop();
            });
        }
    } catch (...) {
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            stop_.store(true, std::memory_order_release);
        }
        cv_.notify_all();
        for (auto& worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        workers_.clear();
        throw;
    }
}

ThreadPool::~ThreadPool() {
    try {
        stop();
    } catch (...) {
    }
}

bool ThreadPool::isWorkerThread() const noexcept {
    return t_active_worker_pool == this;
}

void ThreadPool::stop() {
    if (isWorkerThread()) {
        throw std::logic_error("stop() cannot be called from a worker thread in the same ThreadPool");
    }

    std::call_once(stop_flag_, [this]() {
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            stop_.store(true, std::memory_order_release);
        }
        cv_.notify_all();

        for (auto& worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        workers_.clear();

        wait_cv_.notify_all();
    });
}

void ThreadPool::waitAll() {
    if (isWorkerThread()) {
        throw std::logic_error("waitAll() cannot be called from a worker thread in the same ThreadPool");
    }

    std::unique_lock<std::mutex> lock(queue_mutex_);
    wait_cv_.wait(lock, [this]() {
        return tasks_.empty() && active_tasks_ == 0;
    });
}

size_t ThreadPool::size() const noexcept {
    return worker_count_;
}

size_t ThreadPool::activeTasks() const {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    return active_tasks_;
}

size_t ThreadPool::queueSize() const {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    return tasks_.size();
}

bool ThreadPool::isStopped() const noexcept {
    return stop_.load(std::memory_order_acquire);
}

void ThreadPool::workerLoop() {
    WorkerScopeGuard scopeGuard(this);
    while (true) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            cv_.wait(lock, [this]() {
                return stop_.load(std::memory_order_relaxed) || !tasks_.empty();
            });

            if (stop_.load(std::memory_order_relaxed) && tasks_.empty()) {
                return;
            }

            task = std::move(tasks_.front());
            tasks_.pop();
            ++active_tasks_;
        }

        try {
            task();
        } catch (const std::exception& ex) {
            IMAGINE_LOG_ERROR(std::string("Exception in ThreadPool task: ") + ex.what());
        } catch (...) {
            IMAGINE_LOG_ERROR("Unknown exception in ThreadPool task");
        }

        task = nullptr; // Explicitly release task callable and captured resources before decrementing active_tasks_

        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            --active_tasks_;
            if (tasks_.empty() && active_tasks_ == 0) {
                wait_cv_.notify_all();
            }
        }
    }
}

} // namespace imagine::concurrency
