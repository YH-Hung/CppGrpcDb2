# Resource Pool (C++): Design and Usage

This document describes the design, behavior, and usage of the generic, thread-safe `resource::ResourcePool<T>` implemented in `src/resource/resource_pool.hpp`.

The pool is suitable for managing expensive, reusable resources such as database connections (e.g., `db2::Connection`), network clients, or other handles. It provides RAII semantics via `std::shared_ptr` with a custom deleter so that returning resources to the pool is automatic.

---

## Key Features

- Single resource type per pool via C++ templates: `ResourcePool<MyType>`
- Bounded pool size (maximum concurrency) with:
  - Blocking acquire (`acquire()`)
  - Timed acquire (`acquire_for(...)`, `acquire_until(...)`)
  - Non-blocking acquire (`try_acquire()`)
- Automatic return to the pool when the `std::shared_ptr<T>` goes out of scope (custom deleter)
- Optional resource validator to health-check resources on acquire/return
- Safe shutdown to wake waiting threads and drain idle resources
- Thread-safe under heavy concurrency; minimizes lock hold times during validation and construction

## Design Overview

### Ownership and RAII
- The pool must be owned by a `std::shared_ptr` created via `ResourcePool::create(...)`.
- Resources are handed out as `std::shared_ptr<T>` with a custom deleter that holds a `std::weak_ptr` back to the pool.
- When the last `shared_ptr<T>` for a resource is destroyed, the deleter returns the resource to the pool if the pool still exists; otherwise, it deletes the resource.

### Bounded Capacity and Counters
- The pool tracks:
  - `max_size_`: maximum number of resources allowed (in use + idle)
  - `total_`: current number of resources created (in use + idle)
  - `idle_`: vector of idle, reusable resources
- If `max_size` passed to `create()` is 0, it is coerced to 1.

### Factory and Validator
- `Factory`: `std::function<std::unique_ptr<T>()>`, used to create new resources when needed.
- `Validator` (optional): `std::function<bool(const T&)>`, used to check resource health.
  - Validation is invoked in two cases:
    - When taking a resource from the idle list
    - When returning a resource to the pool
  - Validator exceptions are caught and treated as a failed validation.
  - Invalid resources are discarded and not returned to the pool; `total_` is decremented.

### Synchronization Strategy
- Internally uses `std::mutex` and `std::condition_variable`.
- Validation and resource creation are performed outside the lock to avoid long critical sections and deadlocks.
- Waiters block until either an idle resource is available, capacity allows creation, or shutdown occurs.

### Shutdown Semantics
- `shutdown()` marks the pool as shutting down, wakes all waiters, and destroys all idle resources.
- `acquire()`/`acquire_until(...)` throw `std::runtime_error` if attempted while shutting down.
- Resources currently in use are destroyed later when their `shared_ptr<T>` refcounts drop to zero.

---

## API Overview

Header: `src/resource/resource_pool.hpp`
Namespace: `resource`

- `static std::shared_ptr<ResourcePool> create(std::size_t max_size, Factory factory, Validator validator = {})`
  - Constructs the pool; must be used (not a public constructor) so that deleters can reference the pool safely.
- `SharedPtr acquire()`
  - Blocks until a resource is available or can be created; throws if the pool is shutting down.
- `SharedPtr try_acquire()`
  - Non-blocking; returns `nullptr` if no idle resource is available and capacity is fully used.
- `template<class Rep, class Period> SharedPtr acquire_for(std::chrono::duration<Rep, Period> timeout)`
  - Blocks up to a duration; returns `nullptr` on timeout.
- `template<class ClockT, class Dur> SharedPtr acquire_until(std::chrono::time_point<ClockT, Dur> deadline)`
  - Blocks until a deadline; returns `nullptr` on timeout; throws if shutting down.
- `void shutdown()`
  - Wakes waiters and drains idle resources. In-use resources are returned as their `shared_ptr`s naturally destruct.
- Observability (approximate under concurrency):
  - `std::size_t max_size() const noexcept`
  - `std::size_t total() const noexcept`
  - `std::size_t idle_size() const noexcept`
  - `std::size_t in_use() const noexcept`

---

## Usage Examples

### 1) Pool of DB2 connections

```cpp
#include <memory>
#include "include/db2/db2.hpp"                 // db2::Connection
#include "src/resource/resource_pool.hpp"      // resource::ResourcePool

using resource::ResourcePool;

int main() {
  using db2::Connection;

  // Factory: how to create a new DB2 connection
  auto factory = []() {
    auto conn = std::make_unique<Connection>();
    conn->connect_with_conn_str(
      "DATABASE=mydb;HOSTNAME=host;PORT=50000;PROTOCOL=TCPIP;UID=user;PWD=pass;"
    );
    return conn;
  };

  // Optional validator: ensure the connection is still alive
  auto validator = [](const Connection& c) noexcept {
    return c.is_connected();
  };

  // Create a pool with up to 8 concurrent connections
  auto pool = ResourcePool<Connection>::create(8, factory, validator);

  // Acquire a connection (blocking). Returned as shared_ptr<Connection>.
  auto conn = pool->acquire();
  conn->execute("CREATE TABLE IF NOT EXISTS t(id INT)");

  // When 'conn' goes out of scope, it is automatically returned to the pool.
}
```

Notes:
- Include paths may differ based on your build; adjust `#include` accordingly (e.g., add `src/` to your include directories).
- If the validator fails, the resource is discarded and the pool may create a replacement (subject to `max_size`).

### 2) Timed acquire with fallback

```cpp
using ConnPool = resource::ResourcePool<db2::Connection>;

auto conn = pool->acquire_for(std::chrono::milliseconds(200));
if (!conn) {
  // Timeout: either retry later, log, or degrade functionality
}
```

### 3) Non-blocking try_acquire

```cpp
auto maybe = pool->try_acquire();
if (!maybe) {
  // No resource available now and capacity is full
}
```

### 4) Graceful shutdown

```cpp
// Stop accepting new acquires and wake all waiting threads:
pool->shutdown();
// After this, 'acquire' will throw and idle resources are destroyed.
// In-use resources will be destroyed as their shared_ptrs are released.
```

---

## Behavior and Error Handling Details

- `create(...)` throws `std::invalid_argument` if the factory is empty.
- `acquire()` and `acquire_until(...)` throw `std::runtime_error` if the pool is shutting down.
- `acquire_for(...)`/`acquire_until(...)` return `nullptr` on timeout (do not throw).
- `try_acquire()` returns `nullptr` if no resource can be provided immediately.
- If the factory throws or returns `nullptr`, the pool rolls back its internal counters and rethrows (or throws a `std::runtime_error` for `nullptr`).
- Validator exceptions are caught and treated as validation failures.

---

## Best Practices

- Choose `max_size` based on the true concurrency you expect (e.g., number of threads issuing work) and the backend’s capacity.
- Keep the validator fast and side-effect free (e.g., inexpensive checks like `is_connected()`), or use it sparingly to avoid latency spikes.
- Avoid holding on to acquired resources longer than necessary; let RAII return them promptly.
- Ensure the pool outlives all outstanding `shared_ptr<T>` resources (prefer process-wide/shared lifetime or clear shutdown ordering).
- Handle `nullptr` returns from timed/non-blocking acquires gracefully (retry, backoff, log, or degrade).

---

## Frequently Asked Questions

- Why must I use `ResourcePool::create()` instead of the constructor?
  - The pool uses a custom deleter on `shared_ptr<T>` that needs to reference the pool instance safely. `create()` ensures the pool is owned by a `std::shared_ptr`, enabling a `std::weak_ptr` in the deleter.

- What happens if the validator rejects a resource on return?
  - The resource is discarded and `total_` is decremented. Future acquires may create a fresh resource if under `max_size`.

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

If you have questions or find edge cases not covered here, please open an issue or add notes to this document.
