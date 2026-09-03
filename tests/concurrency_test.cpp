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

    // Calling stop again should not crash or error
    pool.stop();
    EXPECT_TRUE(pool.isStopped());

    // Enqueue after stop should throw runtime_error
    EXPECT_THROW(pool.enqueue([]() {}), std::runtime_error);
}
