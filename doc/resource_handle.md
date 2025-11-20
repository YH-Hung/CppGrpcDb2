# ResourceHandle and Lightweight ResourcePool (unique_ptr-based)

This document explains the `resource_pool::ResourceHandle<T>` RAII wrapper and the accompanying `resource_pool::ResourcePool<T>` implemented in `src/resource/resource_handle.h`.

This pool variant manages resources as `std::unique_ptr<T>` and returns them wrapped in a movable, non-copyable `ResourceHandle<T>`. When a handle goes out of scope, the resource is automatically returned to the pool.

Note: This is a separate implementation from `src/resource/resource_pool.hpp` (which returns `std::shared_ptr<T>`). Choose the one that best fits your ownership model.

---

## Key Types

- `resource_pool::PoolException` — thrown on pool/handle errors.
- `resource_pool::PoolConfig` — configures initial size, max size, timeouts, and validation flags.
- `resource_pool::ResourceHandle<T>` — RAII wrapper over a `std::unique_ptr<T>` borrowed from a pool.
- `resource_pool::ResourcePool<T>` — thread-safe pool managing `T` instances and producing `ResourceHandle<T>`.

---

## Quick Start

```cpp
#include <memory>
#include "src/resource/resource_handle.h"

using namespace resource_pool;

struct Connection {
  void exec(const char* sql) {/* ... */}
  bool is_ok() const { return true; }
};

int main() {
  // Configure the pool
  PoolConfig cfg;
  cfg.initial_size = 2;
  cfg.max_size = 8;
  cfg.validate_on_acquire = true;  // optional
  cfg.validate_on_return  = true;  // optional

  // Factory and optional validator/destroyer
  auto factory   = [](){ return std::make_unique<Connection>(); };
  auto validator = [](const Connection& c){ return c.is_ok(); };
  auto destroyer = [](Connection& c){ /* close/free */ };

  // Create the pool instance
  ResourcePool<Connection> pool(factory, cfg, validator, destroyer);

  // Acquire (blocking, with default timeout from cfg)
  auto h = pool.acquire();
  h->exec("CREATE TABLE IF NOT EXISTS t(id INT)");
  // When 'h' is destroyed (scope exit), the Connection is returned to the pool
}
```

---

## ResourceHandle<T> API

`ResourceHandle<T>` is non-copyable but movable.

- Construction: only produced by `ResourcePool<T>` methods.
- Move operations: transferring a handle transfers ownership of the resource; the moved-from handle becomes empty.
- Destruction: automatically returns the resource to the originating pool.

Operations:

- `T* operator->() const` — access the underlying resource; throws `PoolException` if empty.
- `T& operator*() const` — dereference; throws `PoolException` if empty.
- `explicit operator bool() const` — true if it contains a valid resource.
- `T* get() const noexcept` — raw pointer access (do not delete it).
- `void release()` — manually return the resource to the pool early; the handle becomes empty.

Typical usage pattern is to simply rely on RAII and avoid calling `release()` explicitly.

---

## ResourcePool<T> API (from resource_handle.h)

Constructor:

```cpp
ResourcePool(FactoryFunc factory,
             PoolConfig config = {},
             ValidatorFunc validator = nullptr,
             DestroyFunc destroyer = nullptr);
```

- `FactoryFunc` — `std::function<std::unique_ptr<T>()>`
- `ValidatorFunc` — `std::function<bool(const T&)>` (optional)
- `DestroyFunc` — `std::function<void(T&)>` (optional)

Acquire methods:

- `ResourceHandle<T> acquire(std::optional<std::chrono::milliseconds> timeout = std::nullopt)`
  - Blocks until a resource is available or the timeout expires.
  - Uses `config.acquire_timeout` as default when `timeout` is not provided.
  - Throws `PoolException` on shutdown or on timeout.

- `std::optional<ResourceHandle<T>> tryAcquire()`
  - Non-blocking: returns an empty `std::optional` if no resource is immediately available and creation is not possible.

Other operations:

- `Stats getStats() const` — returns a snapshot with counts and shutdown flag.
- `void shutdown()` — prevent new acquires; destroy idle resources; in-use resources are returned/destroyed via RAII when handles die.
- `bool shutdownAndWait(std::chrono::milliseconds timeout = std::chrono::seconds(30))` — shutdown and wait for all resources to be returned (up to timeout). Returns true if all returned.
- `void forceShutdown()` — immediate shutdown; existing in-use resources become invalid. Use with caution.

Behavioral notes:

- Idle storage is LIFO (most-recently-used resource is returned first) to improve cache locality.
- The validator is called outside the internal mutex to avoid long lock holds; exceptions in the validator are treated as validation failure.

---

## Examples

### 1) Blocking acquire with default timeout

```cpp
using namespace resource_pool;
ResourcePool<Connection> pool(factory, cfg, validator);

try {
  auto handle = pool.acquire();
  handle->exec("INSERT INTO t VALUES(1)");
} catch (const PoolException& e) {
  // Handle shutdown/timeout/creation/validation errors
}
```

### 2) Timed acquire

```cpp
using namespace std::chrono_literals;

auto handle = pool.acquire(200ms); // throws PoolException on timeout
```

### 3) Non-blocking tryAcquire

```cpp
auto maybe = pool.tryAcquire();
if (!maybe) {
  // No resource available right now (and cannot create new under max_size)
} else {
  maybe->get()->exec("SELECT 1");
}
```

### 4) Manual early release (optional)

```cpp
auto h = pool.acquire();
h->exec("...");
h.release(); // resource goes back to pool now; 'h' becomes empty
```

### 5) Graceful shutdown

```cpp
pool.shutdown();
// After this, acquire() will throw; idle resources are destroyed.
// In-use resources are destroyed once their handles are destroyed.
```

### 6) Shutdown and wait for return

```cpp
if (!pool.shutdownAndWait(std::chrono::seconds(5))) {
  // Not all resources were returned within 5 seconds
}
```

---

## Behavior and Error Handling Details

- Constructor throws `PoolException` if the factory is null, if `initial_size > max_size`, if factory returns null during initialization, or if the validator rejects during initialization.
- `acquire(...)` throws `PoolException` on shutdown or timeout; it may also throw if factory/validator fail while creating or validating a resource.
- `tryAcquire()` returns an empty `std::optional` if no resource is available immediately and creation is not possible or fails.
- During return, if validation-on-return fails or the pool is shutting down, the resource is destroyed instead of being re-queued.
- Validator exceptions are caught and treated as validation failures.

---

## Best Practices

- Keep validators fast and side-effect free; they are called outside the pool lock but still add latency.
- Choose `max_size` based on concurrency and backend capacity.
- Prefer RAII and allow handles to go out of scope naturally; avoid holding a handle longer than necessary.
- Use `shutdownAndWait` during orderly application shutdown to ensure all resources are returned and cleaned.
- If you need shared ownership semantics across threads/components, consider using the `ResourcePool` in `src/resource/resource_pool.hpp` instead.

---

## Frequently Asked Questions

**Q: Can I copy a `ResourceHandle<T>`?**

A: No. It is non-copyable to preserve single ownership of the underlying `unique_ptr<T>`. You can move it.

**Q: What happens if I call `operator->()` on an empty handle?**

A: A `PoolException` is thrown. Check truthiness (`if (handle)`) before dereferencing if you are unsure.

**Q: Do I need to call `release()`?**

A: Normally no. Let RAII return the resource when the handle goes out of scope. `release()` is provided for early return.

**Q: How is validation applied?**

A: Controlled by `PoolConfig.validate_on_acquire` and `validate_on_return`. Validation failures discard the resource.

---

## Minimal Example With a Plain Type

```cpp
#include <iostream>
#include <memory>
#include "src/resource/resource_handle.h"

using namespace resource_pool;

struct Worker { void do_work() const { std::cout << "work done\n"; } };

int main() {
  PoolConfig cfg; cfg.max_size = 4; cfg.initial_size = 1;
  ResourcePool<Worker> pool(
    [](){ return std::make_unique<Worker>(); },
    cfg
  );

  auto w = pool.acquire();
  w->do_work();
}
```

---

## File Location

- Implementation: `src/resource/resource_handle.h`
- This document: `doc/resource_handle.md`

If you find edge cases not covered here, please open an issue or update this document.
