#include "imagine/concurrency/thread_pool.hpp"
#include "imagine/common/logger.hpp"

namespace imagine::concurrency {

ThreadPool::ThreadPool(size_t threads) {
    if (threads == 0) {
        threads = 1;
    }
    workers_.reserve(threads);
    for (size_t i = 0; i < threads; ++i) {
        workers_.emplace_back([this]() {
            workerLoop();
        });
    }
}

ThreadPool::~ThreadPool() {
    stop();
}

void ThreadPool::stop() {
    {
        std::unique_lock<std::mutex> lock(queue_mutex_);
        if (stop_.load()) {
            return;
        }
        stop_.store(true);
    }
    cv_.notify_all();
    wait_cv_.notify_all();

    for (std::thread& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    workers_.clear();
}

void ThreadPool::waitAll() {
    std::unique_lock<std::mutex> lock(queue_mutex_);
    wait_cv_.wait(lock, [this]() {
        return tasks_.empty() && active_tasks_.load() == 0;
    });
}

size_t ThreadPool::size() const noexcept {
    return workers_.size();
}

size_t ThreadPool::activeTasks() const noexcept {
    return active_tasks_.load();
}

size_t ThreadPool::queueSize() const {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    return tasks_.size();
}

bool ThreadPool::isStopped() const noexcept {
    return stop_.load();
}

void ThreadPool::workerLoop() {
    while (true) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            cv_.wait(lock, [this]() {
                return stop_.load() || !tasks_.empty();
            });

            if (stop_.load() && tasks_.empty()) {
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

        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            --active_tasks_;
            if (tasks_.empty() && active_tasks_.load() == 0) {
                wait_cv_.notify_all();
            }
        }
    }
}

} // namespace imagine::concurrency
