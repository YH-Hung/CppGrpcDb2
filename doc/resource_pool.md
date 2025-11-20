
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

### 2) Warm-up the pool on startup

Warm-up avoids first-request latency by pre-creating a number of resources:

```cpp
#include "src/resource/resource_pool.hpp"

using resource::ResourcePool;

// ... define factory and optional validator ...

auto pool = ResourcePool<Connection>::create(
  /*max_size=*/16,
  factory,
  validator,
  /*warmup_size=*/4 // pre-create 4 connections now
);
```

Notes:
- `warmup_size` is capped at `max_size`.
- If validator rejects during warm-up, creation throws and no partial state is committed.
