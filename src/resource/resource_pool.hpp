// Generic, thread-safe resource pool for shared, expensive-to-create resources.
//
// Features:
// - Single resource type per pool; supports any type via C++ templates
// - Bounded pool size with blocking acquire and timed acquire
// - Return-to-pool via std::shared_ptr custom deleter (RAII)
// - Optional validator to health-check resources on return/acquire
// - Safe shutdown to wake waiters and drain idle resources
//
// Typical usage with db2::Connection:
//
//   using db2::Connection;
//   auto pool = resource::ResourcePool<Connection>::create(
//       /*max_size=*/8,
//       // Factory to create a new Connection
//       []() {
//         auto c = std::make_unique<Connection>();
//         // c->connect_with_dsn("MYDSN", "user", "pwd"); // or conn string
//         return c;
//       },
//       // Optional validator (can be omitted)
//       [](const Connection& c){ return c.is_connected(); }
//   );
//
//   // Acquire a connection (blocks until available). When the shared_ptr
//   // is destroyed, the resource is returned to the pool automatically.
//   auto conn = pool->acquire();
//   conn->execute("CREATE TABLE ...");
//
// Note: For the RAII deleter to safely return resources to the pool, the pool
// must be owned by a std::shared_ptr (created via ResourcePool::create()).

#pragma once

#include <condition_variable>
#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

namespace resource {

template <class T>
class ResourcePool : public std::enable_shared_from_this<ResourcePool<T>> {
public:
  using value_type = T;
  using UniquePtr = std::unique_ptr<T>;
  using SharedPtr = std::shared_ptr<T>;
  using Factory   = std::function<UniquePtr()>;                   // create new T
  using Validator = std::function<bool(const T&)>;                // check health

  // Create a pool wrapped in std::shared_ptr to enable RAII return via deleter
  static std::shared_ptr<ResourcePool> create(std::size_t max_size,
                                              Factory factory,
                                              Validator validator = {}) {
    if (!factory) {
      throw std::invalid_argument("ResourcePool: factory must not be empty");
    }
    return std::shared_ptr<ResourcePool>(new ResourcePool(max_size, std::move(factory), std::move(validator)));
  }

  // Non-copyable / non-movable (pool should have stable address for deleters)
  ResourcePool(const ResourcePool&) = delete;
  ResourcePool& operator=(const ResourcePool&) = delete;

  ~ResourcePool() {
    shutdown();
  }

  // Acquire a resource (blocking). Throws std::runtime_error if pool is shutting down
  SharedPtr acquire() {
    return acquire_until(Clock::time_point::max());
  }

  // Try to acquire within a duration; returns nullptr on timeout.
  template <class Rep, class Period>
  SharedPtr acquire_for(const std::chrono::duration<Rep, Period>& timeout) {
    return acquire_until(Clock::now() + std::chrono::duration_cast<Clock::duration>(timeout));
  }

  // Try to acquire until a specific time_point; returns nullptr on timeout.
  template <class ClockT, class Dur>
  SharedPtr acquire_until(const std::chrono::time_point<ClockT, Dur>& deadline) {
    // Convert an arbitrary clock deadline to this pool's steady_clock deadline
    using OtherTP = std::chrono::time_point<ClockT, Dur>;
    if (deadline == OtherTP::max()) {
      return acquire_until(Clock::time_point::max());
    }

    const auto now_other = ClockT::now();
    // remaining may be negative; treat as zero waiting time
    const auto remaining_other = deadline - now_other;
    if (remaining_other <= typename OtherTP::duration::zero()) {
      // No time left: perform an immediate attempt without waiting
      return acquire_until(Clock::now());
    }

    const auto remaining = std::chrono::duration_cast<Clock::duration>(remaining_other);
    return acquire_until(Clock::now() + remaining);
  }

  // Non-throwing immediate try_acquire; returns nullptr if not available and cannot create.
  SharedPtr try_acquire() {
    std::unique_lock<std::mutex> lk(mtx_);
    if (shutting_down_) return nullptr;

    if (!idle_.empty()) {
      return make_shared_from_idle_locked(lk);
    }
    if (total_ < max_size_) {
      // will create outside lock
      ++total_;
      lk.unlock();
      return make_shared_from_factory();
    }
    return nullptr;
  }

  // Stop the pool: wake all waiters and destroy all idle resources.
  // In-use resources will be destroyed when their shared_ptrs go out of scope.
  void shutdown() {
    std::vector<UniquePtr> to_destroy;
    {
      std::lock_guard<std::mutex> lk(mtx_);
      if (shutting_down_) return;
      shutting_down_ = true;
      to_destroy.swap(idle_);
    }
    cv_.notify_all();
    // Destroy outside lock
    to_destroy.clear();
  }

  // Observability helpers (approximate under concurrency)
  std::size_t max_size() const noexcept { return max_size_; }
  std::size_t total() const noexcept { std::lock_guard<std::mutex> lk(mtx_); return total_; }
  std::size_t idle_size() const noexcept { std::lock_guard<std::mutex> lk(mtx_); return idle_.size(); }
  std::size_t in_use() const noexcept { std::lock_guard<std::mutex> lk(mtx_); return total_ - idle_.size(); }

private:
  using Clock = std::chrono::steady_clock;

  explicit ResourcePool(std::size_t max_size, Factory factory, Validator validator)
    : max_size_(max_size ? max_size : 1),
      factory_(std::move(factory)),
      validator_(std::move(validator)) {}

  struct Deleter {
    std::weak_ptr<ResourcePool> pool;
    void operator()(T* p) const noexcept {
      if (!p) return;
      if (auto sp = pool.lock()) {
        sp->release_raw(p);
      } else {
        // Pool no longer exists; just delete the resource
        delete p;
      }
    }
  };

  SharedPtr make_shared_from_unique_unlocked(std::unique_ptr<T> u, std::unique_lock<std::mutex>& lk) {
    T* raw = u.release();
    // Create shared_ptr with custom deleter referencing this pool
    auto self = this->shared_from_this();
    // Unlock before returning in case caller does work immediately
    auto ptr = SharedPtr(raw, Deleter{self});
    lk.unlock();
    return ptr;
  }

  SharedPtr make_shared_from_idle_locked(std::unique_lock<std::mutex>& lk) {
    // Pre-condition: lk holds the mutex and idle_ is non-empty
    // Iterate to skip any invalid idle resources without risking deep recursion
    while (true) {
      std::unique_ptr<T> u = std::move(idle_.back());
      idle_.pop_back();

      if (!validator_) {
        return make_shared_from_unique_unlocked(std::move(u), lk);
      }

      // Validate outside the mutex to avoid potential deadlocks and long holds
      auto validator_copy = validator_;
      T* raw = u.get();
      lk.unlock();
      bool ok = false;
      try {
        ok = validator_copy ? validator_copy(*raw) : true;
      } catch (...) {
        ok = false; // Treat validator exceptions as invalid resource
      }
      lk.lock();

      if (shutting_down_) {
        // Pool is shutting down: discard resource
        // Decrement total_ and destroy outside the lock
        --total_;
        std::unique_ptr<T> to_destroy = std::move(u);
        lk.unlock();
        to_destroy.reset();
        cv_.notify_one();
        return nullptr;
      }

      if (ok) {
        return make_shared_from_unique_unlocked(std::move(u), lk);
      }

      // Not ok: discard and adjust counters, then try next option
      --total_;
      std::unique_ptr<T> to_destroy = std::move(u);
      lk.unlock();
      to_destroy.reset();
      cv_.notify_one();
      lk.lock();

      if (!idle_.empty()) {
        continue; // check next idle resource
      }
      if (total_ < max_size_) {
        ++total_;
        lk.unlock();
        return make_shared_from_factory();
      }
      return nullptr; // nothing to return now
    }
  }

  SharedPtr make_shared_from_factory() {
    UniquePtr u;
    try {
      u = factory_();
    } catch (...) {
      // Roll back the total_ count and rethrow
      {
        std::lock_guard<std::mutex> lk(mtx_);
        --total_;
      }
      cv_.notify_one();
      throw;
    }
    if (!u) {
      // Factory returned null; roll back and throw
      {
        std::lock_guard<std::mutex> lk(mtx_);
        --total_;
      }
      cv_.notify_one();
      throw std::runtime_error("ResourcePool factory returned null");
    }
    if (validator_) {
      bool ok = false;
      try {
        ok = validator_(*u);
      } catch (...) {
        ok = false; // Treat exception as invalid resource
      }
      if (!ok) {
        // Created resource is invalid; destroy and roll back, then throw
        u.reset();
        {
          std::lock_guard<std::mutex> lk(mtx_);
          --total_;
        }
        cv_.notify_one();
        throw std::runtime_error("ResourcePool validator rejected created resource");
      }
    }

    auto self = this->shared_from_this();
    return SharedPtr(u.release(), Deleter{self});
  }

  SharedPtr acquire_until(const Clock::time_point& deadline) {
    std::unique_lock<std::mutex> lk(mtx_);

    while (true) {
      if (shutting_down_) {
        throw std::runtime_error("ResourcePool is shutting down");
      }

      if (!idle_.empty()) {
        return make_shared_from_idle_locked(lk);
      }
      if (total_ < max_size_) {
        ++total_;
        lk.unlock();
        return make_shared_from_factory();
      }

      if (deadline == Clock::time_point::max()) {
        cv_.wait(lk, [&]{ return shutting_down_ || !idle_.empty() || total_ < max_size_; });
      } else {
        if (cv_.wait_until(lk, deadline, [&]{ return shutting_down_ || !idle_.empty() || total_ < max_size_; })) {
          continue; // condition satisfied; loop will handle
        } else {
          // timeout
          return nullptr;
        }
      }
    }
  }

  // Return raw pointer back to pool (called by SharedPtr deleter)
  void release_raw(T* p) noexcept {
    std::unique_ptr<T> u(p);
    bool valid = true;
    // Validate outside the mutex to avoid deadlocks; catch exceptions
    auto validator_copy = validator_;
    if (validator_copy) {
      try {
        valid = validator_copy(*u);
      } catch (...) {
        valid = false;
      }
    }

    bool notify = false;
    {
      std::lock_guard<std::mutex> lk(mtx_);
      if (shutting_down_) {
        // Drop resource, reduce total
        --total_;
        notify = true;
      } else if (!valid) {
        // Discard invalid; reduce total
        --total_;
        notify = true;
      } else {
        idle_.push_back(std::move(u));
        notify = true;
      }
    }
    if (notify) cv_.notify_one();
  }

private:
  std::size_t max_size_{};
  Factory factory_{};
  Validator validator_{}; // optional

  mutable std::mutex mtx_;
  std::condition_variable cv_;
  std::vector<UniquePtr> idle_;
  std::size_t total_{0};
  bool shutting_down_{false};
};

} // namespace resource
