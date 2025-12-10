# WorkerPool — A modern C++20 worker thread pool

This document describes the design and usage of `worker::WorkerPool`, a header‑only, C++20 worker (thread) pool used across this project.

It provides:
- A fixed set of worker threads
- A FIFO task queue (optionally bounded)
- A pool‑wide parallelism cap (throttling concurrently running tasks)
- Fire‑and‑forget submission (`post`), non‑blocking try (`try_post`), and `submit` returning `std::future`
- Graceful or fast shutdown semantics
- Optional per‑thread naming on Apple/Linux

Source: `src/worker/WorkerPool.h`

Note: Include paths may differ depending on your build. In this repository, tests include it as `#include "worker/WorkerPool.h"`. If your include dirs don’t map `src/`, use the full path.

---

## Quick Start

```cpp
#include "worker/WorkerPool.h"
using worker::WorkerPool;

int main() {
  WorkerPool pool({
      .thread_count = 4,         // create 4 worker threads
      .parallelism = 4,          // allow up to 4 tasks to run concurrently
      .max_queue = 0,            // unbounded queue; use >0 to bound
      .drain_on_shutdown = true, // process queued tasks on shutdown
      .name = "app-pool"         // optional OS thread names
  });

  // Fire-and-forget:
  pool.post([] { /* do some work */ });

  // Submit work and await a result:
  auto fut = pool.submit([](int a, int b) { return a + b; }, 2, 3);
  int sum = fut.get(); // 5

  // Graceful shutdown (drains the queue):
  pool.shutdown(true);
}
```

---

## Concepts & Design

- Threads vs parallelism
  - `thread_count` = number of worker threads created.
  - `parallelism` = maximum number of tasks allowed to run concurrently across the pool. Implemented via a `std::counting_semaphore`. Can be:
    - `<= thread_count` to intentionally throttle CPU/IO pressure.
    - `> thread_count` to allow all threads to run whenever available (effectively capped by `thread_count`).
  - If `parallelism` is `0`, it defaults to `thread_count`.

- Bounded queue (backpressure)
  - `max_queue = 0` means unbounded queue.
  - If `max_queue > 0` and the queue is full:
    - `try_post(...)` returns `false` immediately.
    - `post(...)` blocks until space becomes available (or the pool starts stopping).

- Executor view
  - `Executor` is a lightweight view obtained via `get_executor()` and provides the same `try_post`, `post`, and `submit` API.
  - Lifetime: do not keep an `Executor` beyond the lifetime of the `WorkerPool` it came from.

- Shutdown modes
  - `shutdown(true)` (drain): stop accepting new tasks; queued tasks continue to run (subject to `parallelism`), then threads join.
  - `shutdown(false)` (drop): queued but not yet running tasks are discarded; producers blocked in `post()` are woken; threads exit once current tasks finish.
  - Destructor calls `shutdown(options.drain_on_shutdown)` in a no‑throw manner.

- Exceptions in tasks
  - Exceptions thrown by task bodies are caught and swallowed inside workers to keep the pool alive. Use `submit()` if you need to propagate exceptions to the caller via `std::future`.

- Thread naming (Apple/Linux)
  - If `Options.name` is non‑empty, worker threads are named `name-<index>` using `pthread_setname_np` (subject to per‑OS length limits).

---

## API Reference

Header: `src/worker/WorkerPool.h`

### `struct worker::WorkerPool::Options`

- `std::size_t thread_count`
  - Default: `std::thread::hardware_concurrency()` (or 1 if unavailable)
- `std::size_t parallelism`
  - Max concurrently running tasks across the pool. `0` means “use `thread_count`”.
- `std::size_t max_queue`
  - `0` = unbounded. Otherwise queue holds at most this many enqueued tasks (not counting currently running tasks).
- `bool drain_on_shutdown`
  - If `true`, destructor performs graceful drain. If `false`, it drops pending work.
- `std::string name`
  - Optional base name for worker threads (Apple/Linux only).

### `explicit WorkerPool(Options options)`
Creates worker threads immediately and prepares synchronization primitives.

### `bool try_post(F&& f)`
Non‑blocking enqueue. Returns `false` if:
- The pool is stopping, or
- The queue is bounded and currently full.

### `bool post(F&& f)`
Blocking enqueue if the queue is bounded and full. Returns `false` only if the pool is stopping. If the queue is unbounded or has room, it enqueues immediately and returns `true`.

### `template<class F, class... Args> auto submit(F&& f, Args&&... args) -> std::future<R>`
Packages the callable and arguments into a `std::packaged_task` and enqueues it. Returns a `std::future<R>` for the callable’s result `R = std::invoke_result_t<F, Args...>`.

Notes:
- Arguments are copied/moved at submission time. To pass by reference, wrap with `std::ref`/`std::cref`.
- If the pool is stopping, `submit(...)` throws `std::runtime_error`.
- If a task throws, that exception is captured by the `std::future` and will rethrow on `future::get()`.

### `Executor get_executor() noexcept`
Returns a lightweight view that exposes the same `try_post`/`post`/`submit` API. Lifetime must not exceed the pool’s lifetime.

### Shutdown
`void shutdown(bool drain) noexcept`
- Idempotent: subsequent calls are no‑ops.
- Wakes any producers waiting for queue space; notifies workers to exit once done per the chosen mode.

### Stats (best‑effort, thread‑safe)
- `std::size_t thread_count() const noexcept`
- `std::size_t parallelism() const noexcept`
- `std::size_t queued_estimate() const noexcept` — current queue size (under lock)
- `std::size_t active() const noexcept` — number of tasks currently running (approximate)

---

## Usage Examples

### 1) Fire‑and‑forget and `submit` with results

```cpp
worker::WorkerPool pool({ .thread_count = 2, .parallelism = 2, .name = "wp-basic" });

// Fire-and-forget
pool.post([]{ /* do some work */ });

// Return a value
auto fut = pool.submit([](int a, int b) { return a + b; }, 2, 3);
int sum = fut.get(); // 5

// Return void via Executor
auto exec = pool.get_executor();
auto fut2 = exec.submit([]{ /* side effects */ });
fut2.get();

pool.shutdown(true);
```

### 2) Enforcing a parallelism cap

```cpp
constexpr std::size_t threads = 4;
constexpr std::size_t par = 2; // allow only 2 running at a time
worker::WorkerPool pool({ .thread_count = threads, .parallelism = par, .name = "wp-par" });

std::atomic<int> running{0};
std::atomic<int> max_running{0};
std::counting_semaphore<1024> release_sem(0);

auto task = [&]{
  int now = running.fetch_add(1) + 1;
  int prev = max_running.load();
  while (now > prev && !max_running.compare_exchange_weak(prev, now)) {}
  release_sem.acquire(); // block until main thread releases
  running.fetch_sub(1);
};

for (int i = 0; i < 16; ++i) pool.try_post(task);

// ... wait until running == par ...
for (int i = 0; i < 16; ++i) release_sem.release();
pool.shutdown(true);
```

### 3) Bounded queue: non‑blocking try vs blocking post

```cpp
worker::WorkerPool pool({ .thread_count = 1, .parallelism = 1, .max_queue = 2, .name = "wp-queue" });

std::counting_semaphore<16> sem_block(0);
std::counting_semaphore<1> sem_started(0);
std::atomic<int> ran{0};

// Keep worker busy so the next tasks go to the queue
pool.try_post([&] {
  ran.fetch_add(1);
  sem_started.release();
  sem_block.acquire();
});

sem_started.acquire();

// Fill queue
bool ok1 = pool.try_post([&]{ ran.fetch_add(1); }); // true
bool ok2 = pool.try_post([&]{ ran.fetch_add(1); }); // true
bool ok3 = pool.try_post([&]{ ran.fetch_add(1); }); // false (queue full)

// This call will block until one slot frees
std::atomic<bool> posted{false};
std::thread producer([&]{ posted = pool.post([&]{ ran.fetch_add(1); }); });

// Free one slot
sem_block.release();

producer.join();
pool.shutdown(true);
```

### 4) Shutdown: drain vs drop

```cpp
worker::WorkerPool pool({ .thread_count = 2, .parallelism = 2, .name = "wp-shutdown" });

// Enqueue a long-running head task so others remain queued
std::counting_semaphore<32> sem_block(0);
pool.post([&]{ sem_block.acquire(); /* work */ });

for (int i = 0; i < 20; ++i) pool.post([]{ /* short work */ });

// Drop mode: queued (not running) tasks are discarded
std::thread t([&]{ pool.shutdown(false); });
// Let shutdown start and clear the queue, then release the head task
sem_block.release();
t.join();
```

---

## Behavior & Error Handling

- Posting while stopping
  - `try_post(...)` and `post(...)` return `false` if the pool has begun stopping.
  - `submit(...)` throws `std::runtime_error` if the pool is stopping, or if enqueue ultimately fails due to stop.

- Timed operations
  - Not provided. Use bounded queues (`max_queue`) and `try_post`/`post` patterns if you need backpressure. Add your own timeouts around `post` by using a producer thread/future if necessary.

- Exceptions in tasks
  - Tasks run via `post`/`try_post` have their exceptions swallowed by the worker thread (to keep the pool alive).
  - `submit` captures exceptions and rethrows them from `future::get()`.

- Ordering
  - FIFO queueing is used for pending tasks, subject to scheduling and the `parallelism` semaphore.

- Resource cleanup
  - `shutdown(true)` drains the queue, then joins all `std::jthread`s.
  - `shutdown(false)` clears the queue, wakes any producers blocking in `post`, then joins.

---

## Best Practices

- Choose `thread_count` based on available cores and workload (CPU‑bound vs IO‑bound).
- Use `parallelism` to cap concurrent work below `thread_count` when you need to limit pressure on shared resources (DB, network, disk) or reduce context switching.
- Prefer small bounded queues in latency‑sensitive services to provide natural backpressure.
- Avoid long blocking operations inside tasks if you aim for throughput; consider using async IO or staging.
- Do not keep `Executor` instances beyond the pool lifetime.
- Use `std::ref`/`std::cref` to pass references to `submit`.
- Always call `shutdown(...)` during orderly shutdown to make behavior explicit.

---

## FAQ

- Why can `submit` throw while `post` returns `bool`?
  - `submit` creates a `std::future` contract; rejecting work after promising a future is best signaled via exception. `post`/`try_post` are fire‑and‑forget convenience APIs that signal failure via `false`.

- Can tasks outlive the pool?
  - No. All tasks run on the pool’s threads. On `shutdown(false)` pending tasks are dropped. On `shutdown(true)` tasks complete before threads join.

- Do tasks run concurrently more than `thread_count`?
  - No. The concurrency is ultimately bounded by threads. The `parallelism` cap is an additional throttle that can be ≤ threads.

- Is there per‑task cancellation?
  - Not built in. You can implement cooperative cancellation with your own flags/tokens checked inside tasks.

---

## File Locations

- Implementation: `src/worker/WorkerPool.h`
- Tests: `tests/worker/test_worker_pool.cpp` (demonstrates basic usage, parallelism cap, bounded queue behavior, and shutdown modes)
- This document: `doc/worker_pool.md`

If you find gaps or have questions, please open an issue or extend this document.
