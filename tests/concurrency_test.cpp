#include <gtest/gtest.h>
#include "imagine/concurrency/thread_pool.hpp"
#include <chrono>
#include <atomic>

using namespace imagine::concurrency;

TEST(ThreadPoolTest, ZeroThreadDefaultAndProperties) {
    ThreadPool pool(0);
    EXPECT_EQ(pool.size(), 1u);
    EXPECT_FALSE(pool.isStopped());
    EXPECT_EQ(pool.queueSize(), 0u);
    EXPECT_EQ(pool.activeTasks(), 0u);
}

TEST(ThreadPoolTest, EnqueueAndReturnValues) {
    ThreadPool pool(4);
    EXPECT_EQ(pool.size(), 4u);

    auto f1 = pool.enqueue([]() { return 42; });
    auto f2 = pool.enqueue([](int a, int b) { return a + b; }, 10, 20);

    EXPECT_EQ(f1.get(), 42);
    EXPECT_EQ(f2.get(), 30);
}

TEST(ThreadPoolTest, WaitAllAndTaskCompletion) {
    ThreadPool pool(2);
    std::atomic<int> counter{0};

    constexpr int kTotalTasks = 20;
    for (int i = 0; i < kTotalTasks; ++i) {
        pool.enqueue([&counter]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            ++counter;
        });
    }

    pool.waitAll();
    EXPECT_EQ(counter.load(), kTotalTasks);
    EXPECT_EQ(pool.queueSize(), 0u);
    EXPECT_EQ(pool.activeTasks(), 0u);
}

TEST(ThreadPoolTest, TaskExceptionsHandledGracefully) {
    ThreadPool pool(2);

    // Standard exception
    auto f1 = pool.enqueue([]() {
        throw std::runtime_error("Simulated worker failure");
    });

    // Custom non-std exception
    auto f2 = pool.enqueue([]() {
        throw 123;
    });

    // Futures throw when getting result
    EXPECT_THROW(f1.get(), std::runtime_error);
    EXPECT_THROW(f2.get(), int);

    // Pool should still be operational after exceptions
    auto f3 = pool.enqueue([]() { return 100; });
    EXPECT_EQ(f3.get(), 100);
}

TEST(ThreadPoolTest, StopIsIdempotentAndRejectsEnqueue) {
    ThreadPool pool(2);
    EXPECT_FALSE(pool.isStopped());

    pool.stop();
    EXPECT_TRUE(pool.isStopped());
    EXPECT_EQ(pool.size(), 2u);

    // Calling stop again should not crash or error
    pool.stop();
    EXPECT_TRUE(pool.isStopped());

    // Enqueue after stop should throw runtime_error
    EXPECT_THROW(pool.enqueue([]() {}), std::runtime_error);
}

TEST(ThreadPoolTest, WorkerCallingWaitAllThrowsLogicError) {
    ThreadPool pool(2);
    auto fut = pool.enqueue([&pool]() {
        pool.waitAll();
    });
    EXPECT_THROW(fut.get(), std::logic_error);
}

TEST(ThreadPoolTest, WorkerCallingStopThrowsLogicError) {
    ThreadPool pool(2);
    auto fut = pool.enqueue([&pool]() {
        pool.stop();
    });
    EXPECT_THROW(fut.get(), std::logic_error);
}

TEST(ThreadPoolTest, ConcurrentCallsToStop) {
    ThreadPool pool(4);
    for (int i = 0; i < 20; ++i) {
        pool.enqueue([]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        });
    }

    constexpr int kNumStopperThreads = 10;
    std::vector<std::thread> stoppers;
    stoppers.reserve(kNumStopperThreads);
    for (int i = 0; i < kNumStopperThreads; ++i) {
        stoppers.emplace_back([&pool]() {
            pool.stop();
        });
    }

    for (auto& t : stoppers) {
        t.join();
    }

    EXPECT_TRUE(pool.isStopped());
    EXPECT_EQ(pool.size(), 4u);
}

TEST(ThreadPoolTest, EnqueueRacingWithStop) {
    ThreadPool pool(4);
    std::atomic<bool> start{false};
    std::atomic<int> completedTasks{0};
    std::atomic<int> acceptedTasks{0};
    std::atomic<int> rejectedTasks{0};

    std::thread producer([&]() {
        while (!start.load()) {
            std::this_thread::yield();
        }
        for (int i = 0; i < 200; ++i) {
            try {
                pool.enqueue([&completedTasks]() {
                    std::this_thread::sleep_for(std::chrono::microseconds(100));
                    ++completedTasks;
                });
                ++acceptedTasks;
            } catch (const std::runtime_error&) {
                ++rejectedTasks;
            }
        }
    });

    std::thread stopper([&]() {
        while (!start.load()) {
            std::this_thread::yield();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        pool.stop();
    });

    start.store(true);
    producer.join();
    stopper.join();

    EXPECT_TRUE(pool.isStopped());
    EXPECT_EQ(acceptedTasks.load() + rejectedTasks.load(), 200);
    EXPECT_EQ(completedTasks.load(), acceptedTasks.load());
}

TEST(ThreadPoolTest, DestructionWithQueuedTasks) {
    std::atomic<int> executedCount{0};
    {
        ThreadPool pool(2);
        for (int i = 0; i < 20; ++i) {
            pool.enqueue([&executedCount]() {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                ++executedCount;
            });
        }
        // Pool destroyed at end of scope; graceful shutdown must execute all queued tasks
    }
    EXPECT_EQ(executedCount.load(), 20);
}

TEST(ThreadPoolTest, MultipleConcurrentWaitAllCallers) {
    ThreadPool pool(4);
    std::atomic<int> counter{0};
    static constexpr int kTotalTasks = 30;

    for (int i = 0; i < kTotalTasks; ++i) {
        pool.enqueue([&counter]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            ++counter;
        });
    }

    constexpr int kWaiters = 5;
    std::vector<std::thread> waiters;
    waiters.reserve(kWaiters);
    for (int i = 0; i < kWaiters; ++i) {
        waiters.emplace_back([&pool, &counter]() {
            pool.waitAll();
            EXPECT_EQ(counter.load(), kTotalTasks);
        });
    }

    for (auto& w : waiters) {
        w.join();
    }

    EXPECT_EQ(counter.load(), kTotalTasks);
    EXPECT_EQ(pool.queueSize(), 0u);
    EXPECT_EQ(pool.activeTasks(), 0u);
}

TEST(ThreadPoolTest, TaskEnqueuesAnotherTaskWhileWaitAllActive) {
    ThreadPool pool(2);
    std::atomic<bool> nestedFinished{false};

    pool.enqueue([&pool, &nestedFinished]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        pool.enqueue([&nestedFinished]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            nestedFinished.store(true);
        });
    });

    pool.waitAll();
    EXPECT_TRUE(nestedFinished.load());
    EXPECT_EQ(pool.activeTasks(), 0u);
    EXPECT_EQ(pool.queueSize(), 0u);
}

TEST(ThreadPoolTest, MoveOnlyCallableAndArguments) {
    ThreadPool pool(2);

    auto ptr = std::make_unique<int>(123);
    auto fut = pool.enqueue([](std::unique_ptr<int> val) {
        return *val * 2;
    }, std::move(ptr));

    EXPECT_EQ(fut.get(), 246);
}

