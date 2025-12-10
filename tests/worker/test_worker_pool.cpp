// Copyright (c) 2025
// Unit tests for worker::WorkerPool

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>
#include <semaphore>

#include "worker/WorkerPool.h"

using namespace std::chrono_literals;

namespace {

// Helper: wait until predicate becomes true or timeout elapses
template <class Pred>
bool wait_until(Pred&& pred, std::chrono::milliseconds timeout = 2000ms, std::chrono::milliseconds step = 5ms) {
    auto start = std::chrono::steady_clock::now();
    while (!pred()) {
        if (std::chrono::steady_clock::now() - start > timeout) return false;
        std::this_thread::sleep_for(step);
    }
    return true;
}

} // namespace

TEST(WorkerPool, BasicSubmitAndExecutor) {
    worker::WorkerPool pool({
        .thread_count = 2,
        .parallelism = 2,
        .max_queue = 0,
        .drain_on_shutdown = true,
        .name = "wp-basic"
    });

    // submit with return value
    auto fut = pool.submit([](int a, int b) { return a + b; }, 2, 3);
    EXPECT_EQ(fut.get(), 5);

    // submit void task through executor
    std::atomic<int> executed{0};
    auto exec = pool.get_executor();
    auto fut2 = exec.submit([&executed]() { executed.fetch_add(1, std::memory_order_relaxed); });
    fut2.get();
    EXPECT_EQ(executed.load(), 1);

    // try_post/post should succeed while running
    EXPECT_TRUE(exec.try_post([&executed]() { executed.fetch_add(1, std::memory_order_relaxed); }));
    EXPECT_TRUE(exec.post([&executed]() { executed.fetch_add(1, std::memory_order_relaxed); }));

    ASSERT_TRUE(wait_until([&] { return executed.load() == 3; }));

    pool.shutdown(true);
}

TEST(WorkerPool, RespectsParallelismCap) {
    constexpr std::size_t threads = 4;
    constexpr std::size_t par = 2;
    constexpr int tasks = 16;

    worker::WorkerPool pool({
        .thread_count = threads,
        .parallelism = par,
        .max_queue = 0,
        .drain_on_shutdown = true,
        .name = "wp-par"
    });

    std::atomic<int> running{0};
    std::atomic<int> max_running{0};
    std::counting_semaphore<1024> release_sem(0);

    auto record_start = [&](int) {
        int now = running.fetch_add(1) + 1;
        // update max_running
        int prev = max_running.load();
        while (now > prev && !max_running.compare_exchange_weak(prev, now)) {
            // prev reloaded by CAS
        }
        // hold until main thread releases
        release_sem.acquire();
        running.fetch_sub(1);
    };

    // Enqueue many tasks that block until we release them
    for (int i = 0; i < tasks; ++i) {
        EXPECT_TRUE(pool.try_post([=]() mutable { record_start(i); }));
    }

    // Wait until the first wave of tasks reaches the parallelism limit
    ASSERT_TRUE(wait_until([&] { return running.load() == static_cast<int>(par); }));
    EXPECT_EQ(max_running.load(), static_cast<int>(par));

    // Let all tasks complete, releasing one permit per pending task
    for (int i = 0; i < tasks; ++i) release_sem.release();
    pool.shutdown(true);

    // Ensure we never exceeded the parallelism
    EXPECT_LE(max_running.load(), static_cast<int>(par));
}

TEST(WorkerPool, BoundedQueueTryPostAndBlockingPost) {
    // One worker, one running at a time, small queue
    worker::WorkerPool pool({
        .thread_count = 1,
        .parallelism = 1,
        .max_queue = 2,
        .drain_on_shutdown = true,
        .name = "wp-queue"
    });

    std::counting_semaphore<16> sem_block(0);
    std::counting_semaphore<1> sem_started(0);
    std::atomic<int> ran{0};

    // Keep worker busy
    ASSERT_TRUE(pool.try_post([&] {
        ran.fetch_add(1);
        sem_started.release(); // signal we started running (dequeued)
        sem_block.acquire();   // block until released
    }));

    // Ensure the long-running task has started (queue is empty now)
    sem_started.acquire();

    // Fill the bounded queue with 2 tasks (should succeed)
    EXPECT_TRUE(pool.try_post([&] { ran.fetch_add(1); }));
    EXPECT_TRUE(pool.try_post([&] { ran.fetch_add(1); }));
    // Now it should be full; next try_post should fail
    EXPECT_FALSE(pool.try_post([&] { ran.fetch_add(1); }));

    // Start a thread that will block in post() until there is space
    std::atomic<bool> post_returned{false};
    std::thread producer([&] {
        bool ok = pool.post([&] { ran.fetch_add(1); });
        post_returned.store(true);
        EXPECT_TRUE(ok);
    });

    // Give a short time to ensure producer is blocked
    std::this_thread::sleep_for(50ms);
    EXPECT_FALSE(post_returned.load());

    // Free the worker to drain one slot, unblocking producer
    sem_block.release();

    ASSERT_TRUE(wait_until([&] { return post_returned.load(); }));

    // Allow remaining tasks to finish
    pool.shutdown(true);
    if (producer.joinable()) producer.join();

    // We had 1 running + 2 queued + 1 blocked post = 4 total executions
    EXPECT_EQ(ran.load(), 4);
}

TEST(WorkerPool, ShutdownDrainProcessesAll) {
    worker::WorkerPool pool({
        .thread_count = 3,
        .parallelism = 3,
        .max_queue = 0,
        .drain_on_shutdown = true,
        .name = "wp-drain"
    });

    std::atomic<int> count{0};
    constexpr int N = 20;
    for (int i = 0; i < N; ++i) {
        ASSERT_TRUE(pool.try_post([&] { count.fetch_add(1, std::memory_order_relaxed); }));
    }

    pool.shutdown(true);
    EXPECT_EQ(count.load(), N);
}

TEST(WorkerPool, ShutdownDropClearsQueueAndRejectsNewWork) {
    worker::WorkerPool pool({
        .thread_count = 1, // single worker prevents another thread from popping a task before shutdown
        .parallelism = 1,
        .max_queue = 0,
        .drain_on_shutdown = true,
        .name = "wp-drop"
    });

    std::counting_semaphore<32> sem_block(0);
    std::atomic<int> count{0};

    // Long running head task so that following tasks remain queued
    ASSERT_TRUE(pool.try_post([&] {
        count.fetch_add(1);
        sem_block.acquire();
    }));

    // Enqueue many tasks that should be dropped
    for (int i = 0; i < 20; ++i) {
        ASSERT_TRUE(pool.try_post([&] { count.fetch_add(1); }));
    }

    // Initiate shutdown with drain=false on a separate thread to avoid deadlock,
    // since the worker is blocked inside the running task.
    std::thread shutdown_thread([&]{ pool.shutdown(false); });

    // Give shutdown a brief moment to start and clear the queue
    std::this_thread::sleep_for(20ms);

    // Release the head task so the worker can finish and join
    sem_block.release();
    if (shutdown_thread.joinable()) shutdown_thread.join();

    // After shutdown(false), pending tasks were cleared; only the head ran
    EXPECT_EQ(count.load(), 1);

    // New work should be rejected
    EXPECT_FALSE(pool.try_post([] {}));
    EXPECT_FALSE(pool.post([] {}));
    EXPECT_THROW({ (void)pool.submit([] { return 42; }); }, std::runtime_error);
}
