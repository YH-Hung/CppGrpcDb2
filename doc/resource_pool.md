## ResourcePool (shared_ptr-based)

Thread-safe pool for expensive-to-create resources that are borrowed as `std::shared_ptr<T>` and automatically returned to the pool when the last `shared_ptr` goes out of scope.

Implementation: `src/resource/resource_pool.hpp`

This pool is separate from the unique_ptr/handle-based pool in `src/resource/resource_handle.h`. See `doc/resource_pool_vs_handle.md` for a comparison and guidance.

---

## Key Ideas

- Factory-supplied creation: `std::function<std::unique_ptr<T>()>`
- Optional validator: `std::function<bool(const T&)>`
- Bounded capacity with blocking acquire and timed acquire
- RAII return via a `std::shared_ptr` custom deleter
- Optional warm-up to pre-create resources

Idle storage is LIFO (most recently used is returned first), which tends to improve cache locality.

---

## Quick Start

```cpp
#include <memory>
#include "src/resource/resource_pool.hpp"

using resource::ResourcePool;

struct Connection {
  void execute(const char* sql) {/* ... */}
  bool is_connected() const { return true; }
};

int main() {
  auto pool = ResourcePool<Connection>::create(
    /*max_size=*/8,
    [](){
      auto c = std::make_unique<Connection>();
      // c->connect(...);
      return c;
    },
    // optional validator
    [](const Connection& c){ return c.is_connected(); },
    // optional warm-up
    /*warmup_size=*/2
  );

  // Acquire a connection (blocking). Returned as shared_ptr<Connection>.
  auto conn = pool->acquire();
  conn->execute("CREATE TABLE IF NOT EXISTS t(id INT)");
  // When 'conn' is destroyed, the resource is returned to the pool automatically.
}
```

Notes:
- Adjust include paths if needed (e.g., add `src/` to include directories).
- If validation fails, the resource is discarded and the pool may create a replacement (subject to `max_size`).

---

## API Overview

Creation (static):

```cpp
// Wraps the pool in a std::shared_ptr so the custom deleter can return to it
static std::shared_ptr<ResourcePool<T>> create(std::size_t max_size,
                                               Factory factory,
                                               Validator validator = {},
                                               std::size_t warmup_size = 0);
```

Acquire/Release:

- `SharedPtr acquire()`
  - Blocks until a resource is available. Throws `std::runtime_error` if shutting down.
- `SharedPtr acquire_for(duration)` / `SharedPtr acquire_until(time_point)`
  - Time-bounded acquire; return `nullptr` on timeout; throw if shutting down.
- `SharedPtr try_acquire()`
  - Non-blocking; returns `nullptr` if nothing is immediately available and capacity is full.

Shutdown & Observability:

- `void shutdown()` — stop accepting new acquires, wake waiters, destroy idle resources. In-use resources are destroyed when their `shared_ptr`s are released.
- `std::size_t max_size() const noexcept`
- `std::size_t total() const noexcept` — total created (idle + in-use)
- `std::size_t idle_size() const noexcept`
- `std::size_t in_use() const noexcept`

---

## Examples

### 1) Timed acquire with fallback

```cpp
auto conn = pool->acquire_for(std::chrono::milliseconds(200));
if (!conn) {
  // Timeout: retry, log, or degrade functionality
}
```

### 2) Non-blocking try_acquire

```cpp
auto maybe = pool->try_acquire();
if (!maybe) {
  // No resource available now and capacity is full
}
```

### 3) Warm-up on startup

Warm-up avoids first-request latency by pre-creating a number of resources:

```cpp
auto pool = resource::ResourcePool<Connection>::create(
  /*max_size=*/16,
  factory,
  validator,
  /*warmup_size=*/4 // pre-create 4 connections now
);
```

Notes:
- `warmup_size` is capped at `max_size`.
- If validator rejects during warm-up, creation throws and no partial state is committed.

### 4) Graceful shutdown

```cpp
pool->shutdown();
// After this, acquire/acquire_until will throw; timed acquires return nullptr on timeout.
// Idle resources are destroyed; in-use resources are destroyed when their shared_ptrs are released.
```

---

## Behavior and Error Handling

- `create(...)` throws `std::invalid_argument` if the factory is empty.
- `acquire()`/`acquire_until(...)` throw `std::runtime_error` if the pool is shutting down.
- `acquire_for(...)`/`acquire_until(...)` return `nullptr` on timeout (do not throw on timeout).
- `try_acquire()` returns `nullptr` if no resource can be provided immediately.
- If the factory throws or returns `nullptr`, the pool rolls back its internal counters and rethrows (or throws `std::runtime_error` for `nullptr`).
- Validator exceptions are caught and treated as validation failures.
- Validation is applied when borrowing from idle, on newly created resources, and upon return (via the custom deleter). Failed validation discards the resource and decrements `total`.

---

## Best Practices

- Choose `max_size` based on true concurrency and backend limits.
- Keep validators fast and side-effect free (e.g., `is_connected()`), or use sparingly to avoid latency spikes.
- Avoid holding on to acquired resources longer than necessary; let RAII return them promptly.
- Ensure the pool outlives all outstanding `shared_ptr<T>` resources (prefer process-wide/shared lifetime or clear shutdown ordering).
- Handle `nullptr` returns from timed/non-blocking acquires gracefully (retry/backoff/log/degrade).

---

## Frequently Asked Questions

- Why must I use `ResourcePool::create()` instead of directly constructing the pool?
  - The pool installs a custom deleter on `shared_ptr<T>` which needs to reference the pool safely. `create()` ensures the pool is managed by a `std::shared_ptr`, enabling a `std::weak_ptr` inside the deleter.

- What happens if the validator rejects a resource on return?
  - The resource is discarded and `total` is decremented. Future acquires may create a fresh resource if under `max_size`.

- Can `acquire()` return `nullptr`?
  - No. `acquire()` either returns a valid resource or throws if the pool is shutting down. For timeouts or non-blocking semantics, use `acquire_for`/`acquire_until`/`try_acquire`.

---

## Minimal Example With a Plain Type

```cpp
#include <iostream>
#include <memory>
#include "src/resource/resource_pool.hpp"

struct Worker {
  void do_work() const { std::cout << "work done\n"; }
};

int main() {
  auto pool = resource::ResourcePool<Worker>::create(
    /*max_size=*/4,
    [](){ return std::make_unique<Worker>(); }
  );

  auto w = pool->acquire();
  w->do_work();
}
```

---

## File Location

- Implementation: `src/resource/resource_pool.hpp`
- This document: `doc/resource_pool.md`

See also: `doc/resource_handle.md` and `doc/resource_pool_vs_handle.md`.
