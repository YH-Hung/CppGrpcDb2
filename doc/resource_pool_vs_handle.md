## ResourcePool (shared_ptr) vs ResourceHandle Pool (unique_ptr)

This guide compares the two resource pooling implementations in this codebase and helps you choose the right one for your use case.

- Shared-pointer pool: `src/resource/resource_pool.hpp` (namespace `resource`)
- Handle-based pool: `src/resource/resource_handle.h` (namespace `resource_pool`)

See the detailed docs:
- `doc/resource_pool.md` — `std::shared_ptr<T>` pool
- `doc/resource_handle.md` — `ResourceHandle<T>` + `std::unique_ptr<T>` pool

---

## At a Glance

- Ownership model
  - Shared-ptr pool: shared ownership; resource returns to pool when the last `std::shared_ptr<T>` is destroyed.
  - Handle pool: single-owner, move-only `ResourceHandle<T>`; resource returns on handle destruction (or explicit `release()`).

- Acquire semantics
  - Shared-ptr pool: `acquire()` blocks (throws on shutdown); timed acquires return `nullptr` on timeout; `try_acquire()` returns `nullptr` if not immediately available.
  - Handle pool: `acquire(timeout)` throws `PoolException` on timeout or shutdown; `tryAcquire()` returns `std::optional<ResourceHandle<T>>`.

- Configuration
  - Shared-ptr pool: `create(max_size, factory, validator, warmup_size)`.
  - Handle pool: `PoolConfig` with `initial_size`, `max_size`, `acquire_timeout`, `validate_on_acquire/return`, plus optional custom `destroyer`.

- Shutdown
  - Shared-ptr pool: `shutdown()` stops acquires, destroys idle; in-use are destroyed when their `shared_ptr`s die.
  - Handle pool: `shutdown()`, `shutdownAndWait(timeout)`, and `forceShutdown()` (invalidates in-use handles; use with care).

---

## Pros and Cons

### Shared-ptr ResourcePool (resource/resource_pool.hpp)

Pros:
- Simple to pass the resource around across components via copying `std::shared_ptr<T>`.
- Timed acquires are non-throwing (`nullptr` on timeout) which can simplify control flow.
- Optional warm-up to pre-create resources to avoid first-use latency.
- Optional validator for health checks on borrow/creation/return.

Cons:
- Shared ownership can make it harder to reason about how long a resource remains in use; accidental extra copies may prolong lifetimes.
- `std::shared_ptr` introduces atomic reference-count overhead and potential cache line contention.
- No explicit `shutdownAndWait`; you rely on external ordering or process shutdown to ensure resources are all released.

Best fit:
- When you need to fan-out the resource across layers/components and accept shared ownership.
- When you prefer non-throwing timeouts (`nullptr` return) and a minimal API.

### Handle-based ResourcePool (resource/resource_handle.h)

Pros:
- Strong ownership discipline: move-only `ResourceHandle<T>` prevents accidental sharing/copies.
- Rich lifecycle controls: `shutdownAndWait(timeout)` and `forceShutdown()` for stricter shutdown semantics.
- Configurable validation (`validate_on_acquire/return`) and optional custom `destroyer` hook for special teardown.
- Typically lower overhead than copying `std::shared_ptr` due to move-only handle semantics.

Cons:
- Cannot copy a handle; passing across components requires moves or redesigning interfaces to accept a handle.
- Timed `acquire` throws on timeout, so you need try/catch or use `tryAcquire()`/`std::optional` to avoid exceptions.

Best fit:
- When you want to strictly bound usage lifetimes and avoid shared ownership.
- When you need explicit shutdown coordination (e.g., services that must drain within N seconds).
- When custom destruction is needed (closing sockets, DB handles with special steps).

---

## API Differences (Highlights)

Shared-ptr pool (`resource::ResourcePool<T>`):
- Construction: `auto pool = ResourcePool<T>::create(max_size, factory, validator, warmup_size);`
- Acquire: `acquire()`, `acquire_for(duration)`, `acquire_until(time_point)`, `try_acquire()`
- Shutdown/Stats: `shutdown()`, `max_size()`, `total()`, `idle_size()`, `in_use()`

Handle pool (`resource_pool::ResourcePool<T>` + `ResourceHandle<T>`):
- Construction: `ResourcePool<T> pool(factory, cfg, validator, destroyer);`
- Acquire: `acquire(optional_timeout)`, `tryAcquire()` (returns `std::optional<Handle>`) 
- Shutdown: `shutdown()`, `shutdownAndWait(timeout)`, `forceShutdown()`
- Handle ops: `operator->`, `operator*`, `explicit operator bool`, `get()`, `release()`

Timeout semantics:
- Shared-ptr pool timed acquires return `nullptr` on timeout.
- Handle pool timed acquire throws `PoolException` on timeout; use `tryAcquire()` if you want a non-throwing path.

---

## Performance Considerations

- `std::shared_ptr` involves atomic ref-count updates on copies; if a resource is passed frequently, this overhead can be measurable.
- Move-only handles avoid ref-counting; typical operations are cheaper, but the API enforces single ownership.
- Both pools use LIFO for idle storage to improve cache locality of hot resources.
- Warm-up vs initial_size: the shared-ptr pool uses `warmup_size` at creation; the handle pool uses `initial_size` in `PoolConfig`.

---

## Suggested Usage Scenarios

Choose shared-ptr pool when:
- Multiple layers or async tasks may need to momentarily hold references to the same resource without tight coordination.
- You prefer `nullptr`-based timeout handling rather than exceptions.
- You want a compact API and simple warm-up behavior.

Choose handle-based pool when:
- You want strong guarantees that resources aren’t accidentally copied and retained.
- You need explicit shutdown coordination (`shutdownAndWait`) or emergency teardown (`forceShutdown`).
- You require a custom destroyer for precise cleanup of native resources.

---

## Interoperability and Migration

- The two pools are independent; their types are not interchangeable. Pick one per resource type for consistency.
- If migrating from shared-ptr to handle-based:
  - Replace `std::shared_ptr<T>` parameters with `ResourceHandle<T>` or redesign call sites to acquire and use within a tighter scope.
  - Review timeout handling: switch from `nullptr` checks to `tryAcquire()` or catching `PoolException` on `acquire`.
  - Map `warmup_size` to `PoolConfig.initial_size` accordingly.

---

## File Locations

- Shared-ptr pool: `src/resource/resource_pool.hpp` (docs: `doc/resource_pool.md`)
- Handle-based pool: `src/resource/resource_handle.h` (docs: `doc/resource_handle.md`)
