// WorkerPool.h
// A modern C++20 worker pool with configurable pool size and parallelism.
// Header-only, no external dependencies.

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <semaphore>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>


namespace worker {

class WorkerPool {
public:
    struct Options {
        std::size_t thread_count = std::thread::hardware_concurrency() ?
                                   std::thread::hardware_concurrency() : 1;
        // Max number of tasks allowed to run concurrently across the pool.
        // Can be <= thread_count to throttle CPU pressure, or > thread_count to allow all threads to always run.
        // Defaults to thread_count.
        std::size_t parallelism = 0; // 0 => use thread_count

        // 0 => unbounded queue; otherwise bounded to this many enqueued tasks
        std::size_t max_queue = 0;

        // If true, let the pool drain remaining queued work on shutdown(). If false, pending work is dropped.
        bool drain_on_shutdown = true;

        // Optional name for debugging/diagnostics.
        std::string name;
    };

    // Lightweight executor view that delegates to the parent pool.
    class Executor {
    public:
        explicit Executor(WorkerPool& pool) noexcept : pool_{&pool} {}

        template <class F>
        bool try_post(F&& f) {
            return pool_->try_post(std::forward<F>(f));
        }

        template <class F>
        bool post(F&& f) {
            return pool_->post(std::forward<F>(f));
        }

        template <class F, class... Args>
        auto submit(F&& f, Args&&... args) {
            return pool_->submit(std::forward<F>(f), std::forward<Args>(args)...);
        }

    private:
        WorkerPool* pool_;
    };

    using Task = std::function<void()>;

    explicit WorkerPool(Options options)
        : options_{normalize(std::move(options))}
        , permits_{static_cast<std::ptrdiff_t>(options_.parallelism)}
    {
        start_threads();
    }

    WorkerPool(const WorkerPool&) = delete;
    WorkerPool& operator=(const WorkerPool&) = delete;
    WorkerPool(WorkerPool&&) = delete;
    WorkerPool& operator=(WorkerPool&&) = delete;

    ~WorkerPool() {
        try { shutdown(options_.drain_on_shutdown); }
        catch (...) { /* no-throw dtor */ }
    }

    // Non-blocking enqueue. Returns false if pool is stopping or the queue is full.
    template <class F>
    bool try_post(F&& f) {
        if (is_stopping_.load(std::memory_order_acquire)) return false;
        auto task = make_task(std::forward<F>(f));
        return try_enqueue(std::move(task));
    }

    // Blocking enqueue if queue is bounded. Returns false if pool is stopping.
    template <class F>
    bool post(F&& f) {
        if (is_stopping_.load(std::memory_order_acquire)) return false;
        auto task = make_task(std::forward<F>(f));
        return enqueue_blocking(std::move(task));
    }

    // Submit callable and get a future to its result.
    template <class F, class... Args>
    auto submit(F&& f, Args&&... args) -> std::future<std::invoke_result_t<F, Args...>> {
        using R = std::invoke_result_t<F, Args...>;
        if (is_stopping_.load(std::memory_order_acquire)) {
            throw std::runtime_error("WorkerPool is stopping");
        }

        auto bound = std::bind(std::forward<F>(f), std::forward<Args>(args)...);
        auto ptask = std::make_shared<std::packaged_task<R()>>(std::move(bound));
        std::future<R> fut = ptask->get_future();

        Task task = [ptask]() mutable {
            (*ptask)();
        };

        if (!enqueue_blocking(std::move(task))) {
            throw std::runtime_error("WorkerPool rejected task (stopping)");
        }
        return fut;
    }

    // Request shutdown. If drain == true, existing queued tasks are processed before exit.
    // If drain == false, pending queued tasks are discarded.
    void shutdown(bool drain) noexcept {
        bool expected = false;
        if (!is_stopping_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            // already stopping
            return;
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            drain_on_shutdown_ = drain;
            if (!drain_on_shutdown_) {
                queue_.clear();
                // free up any producers blocked on queue space
                space_cv_.notify_all();
            }
        }

        // wake workers
        task_cv_.notify_all();

        // Request cooperative stop on all threads and join them by clearing the container
        for (auto& t : threads_) {
            t.request_stop();
        }
        // Clearing the vector joins all jthreads (joining happens in ~jthread)
        threads_.clear();
    }

    // Stats (best-effort, thread-safe)
    std::size_t thread_count() const noexcept { return options_.thread_count; }
    std::size_t parallelism() const noexcept { return options_.parallelism; }
    std::size_t queued_estimate() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }
    std::size_t active() const noexcept { return active_.load(std::memory_order_relaxed); }

    Executor get_executor() noexcept { return Executor{*this}; }

private:
    static Options normalize(Options opts) {
        if (opts.thread_count == 0) opts.thread_count = 1;
        if (opts.parallelism == 0) opts.parallelism = opts.thread_count;
        if (opts.parallelism == 0) opts.parallelism = 1; // in case thread_count became 0
        return opts;
    }

    template <class F>
    static Task make_task(F&& f) {
        using Fn = std::decay_t<F>;
        auto sp = std::make_shared<Fn>(std::forward<F>(f));
        return [sp]() mutable { (*sp)(); };
    }

    bool try_enqueue(Task&& t) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (is_stopping_.load(std::memory_order_acquire)) return false;
        if (options_.max_queue != 0 && queue_.size() >= options_.max_queue) {
            return false;
        }
        queue_.emplace_back(std::move(t));
        task_cv_.notify_one();
        return true;
    }

    bool enqueue_blocking(Task&& t) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (is_stopping_.load(std::memory_order_acquire)) return false;
        if (options_.max_queue != 0) {
            space_cv_.wait(lock, [&]{
                return is_stopping_.load(std::memory_order_acquire) || queue_.size() < options_.max_queue;
            });
            if (is_stopping_.load(std::memory_order_acquire)) return false;
        }
        queue_.emplace_back(std::move(t));
        task_cv_.notify_one();
        return true;
    }

    void start_threads() {
        threads_.reserve(options_.thread_count);
        for (std::size_t i = 0; i < options_.thread_count; ++i) {
            threads_.emplace_back([this](std::stop_token st){ this->worker_loop(std::move(st)); });
        }
    }

    void worker_loop(std::stop_token st) noexcept {
        for (;;) {
            Task task;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                task_cv_.wait(lock, [&]{ return !queue_.empty() || is_stopping_.load(std::memory_order_acquire); });

                if (queue_.empty()) {
                    // If stopping and either not draining or nothing left, exit.
                    if (is_stopping_.load(std::memory_order_acquire)) {
                        break;
                    }
                    // else spurious wakeup; continue waiting
                    continue;
                }

                task = std::move(queue_.front());
                queue_.pop_front();
                // Notify a waiting producer that we freed a slot
                if (options_.max_queue != 0) {
                    space_cv_.notify_one();
                }
            }

            // Execute outside the lock and under concurrency throttle
            permits_.acquire();
            active_.fetch_add(1, std::memory_order_relaxed);
            try {
                task();
            } catch (...) {
                // Swallow exceptions to keep the worker alive.
                // Consider adding logging if a logging facility is present.
            }
            active_.fetch_sub(1, std::memory_order_relaxed);
            permits_.release();

            if (st.stop_requested() && is_stopping_.load(std::memory_order_acquire)) {
                // Cooperative early-exit point after finishing current work
                // The main condition above will end the loop once the queue drains
                continue;
            }
        }
    }

private:
    Options options_{};
    std::vector<std::jthread> threads_;

    // Global pool-wide throttling of concurrently running tasks
    std::counting_semaphore<> permits_;

    // Work queue and synchronization
    mutable std::mutex mutex_;
    std::condition_variable task_cv_;
    std::condition_variable space_cv_;
    std::deque<Task> queue_;

    std::atomic<bool> is_stopping_{false};
    bool drain_on_shutdown_{true};
    std::atomic<std::size_t> active_{0};
};

} // namespace worker

/*
Example usage:

    #include "src/worker/WorkerPool.h"

    void demo() {
        worker::WorkerPool pool({
            .thread_count = 8,
            .parallelism = 4,     // at most 4 tasks run concurrently
            .max_queue = 1024,    // bound the queue
            .drain_on_shutdown = true,
            .name = "server-pool"
        });

        auto exec = pool.get_executor();
        exec.post([]{
            // fire-and-forget
        });

        auto fut = exec.submit([](int a, int b){ return a + b; }, 2, 3);
        int sum = fut.get();

        pool.shutdown(true); // graceful
    }
*/
