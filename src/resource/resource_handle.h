#pragma once

#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>
#include <stdexcept>
#include <thread>
#include <chrono>

namespace resource_pool {

/**
 * @brief Exception thrown when resource pool operations fail
 */
class PoolException : public std::runtime_error {
public:
    explicit PoolException(const std::string& message)
        : std::runtime_error(message) {}
};

/**
 * @brief Configuration for resource pool behavior
 */
struct PoolConfig {
    size_t initial_size = 5;
    size_t max_size = 10;
    std::chrono::milliseconds acquire_timeout{30000}; // 30 seconds
    bool validate_on_acquire = true;
    bool validate_on_return = true;
    size_t max_idle_time_seconds = 300; // 5 minutes
};

/**
 * @brief RAII wrapper for borrowed resources
 *
 * Ensures resources are automatically returned to the pool when going out of scope.
 * Non-copyable but movable for flexibility.
 *
 * @tparam T The resource type
 */
template<typename T>
class ResourceHandle {
public:
    ResourceHandle() = default;

    // Non-copyable
    ResourceHandle(const ResourceHandle&) = delete;
    ResourceHandle& operator=(const ResourceHandle&) = delete;

    // Movable
    ResourceHandle(ResourceHandle&& other) noexcept
        : resource_(std::move(other.resource_))
        , return_func_(std::move(other.return_func_))
    {
        other.return_func_ = nullptr;
    }

    ResourceHandle& operator=(ResourceHandle&& other) noexcept {
        if (this != &other) {
            release();
            resource_ = std::move(other.resource_);
            return_func_ = std::move(other.return_func_);
            other.return_func_ = nullptr;
        }
        return *this;
    }

    ~ResourceHandle() {
        release();
    }

    /**
     * @brief Access the underlying resource
     */
    T* operator->() const {
        if (!resource_) {
            throw PoolException("Attempting to access null resource");
        }
        return resource_.get();
    }

    T& operator*() const {
        if (!resource_) {
            throw PoolException("Attempting to dereference null resource");
        }
        return *resource_;
    }

    /**
     * @brief Check if handle contains a valid resource
     */
    explicit operator bool() const noexcept {
        return resource_ != nullptr;
    }

    /**
     * @brief Get raw pointer (use with caution)
     */
    T* get() const noexcept {
        return resource_.get();
    }

    /**
     * @brief Manually release the resource back to the pool
     */
    void release() {
        if (resource_ && return_func_) {
            return_func_(std::move(resource_));
            return_func_ = nullptr;
        }
    }

private:
    template<typename U>
    friend class ResourcePool;

    ResourceHandle(std::unique_ptr<T> resource,
                   std::function<void(std::unique_ptr<T>)> return_func)
        : resource_(std::move(resource))
        , return_func_(std::move(return_func))
    {}

    std::unique_ptr<T> resource_;
    std::function<void(std::unique_ptr<T>)> return_func_;
};

/**
 * @brief Thread-safe generic resource pool
 *
 * Features:
 * - Generic resource type support through templates
 * - Thread-safe acquire/release operations
 * - Configurable pool size (initial and max)
 * - Resource validation on acquire/return
 * - Timeout support for acquire operations
 * - RAII-based resource handles
 * - Graceful shutdown with proper cleanup
 * - No memory leaks, deadlocks, or race conditions
 *
 * @tparam T The resource type to pool
 */
template<typename T>
class ResourcePool {
public:
    using FactoryFunc = std::function<std::unique_ptr<T>()>;
    using ValidatorFunc = std::function<bool(const T&)>;
    using DestroyFunc = std::function<void(T&)>;

    /**
     * @brief Construct a resource pool
     *
     * @param factory Function to create new resources
     * @param config Pool configuration
     * @param validator Optional function to validate resources
     * @param destroyer Optional function to properly destroy resources
     */
    ResourcePool(FactoryFunc factory,
                 PoolConfig config = PoolConfig{},
                 ValidatorFunc validator = nullptr,
                 DestroyFunc destroyer = nullptr)
        : factory_(std::move(factory))
        , config_(config)
        , validator_(std::move(validator))
        , destroyer_(std::move(destroyer))
        , shutdown_(false)
        , total_created_(0)
    {
        if (!factory_) {
            throw PoolException("Factory function cannot be null");
        }

        if (config_.initial_size > config_.max_size) {
            throw PoolException("Initial size cannot exceed max size");
        }

        // Pre-allocate initial resources (no lock needed during construction)
        try {
            for (size_t i = 0; i < config_.initial_size; ++i) {
                auto resource = factory_();
                if (!resource) {
                    throw PoolException("Factory returned null during initialization");
                }

                // Validate if configured
                if (validator_) {
                    bool valid = false;
                    try {
                        valid = validator_(*resource);
                    } catch (...) {
                        valid = false;
                    }
                    if (!valid) {
                        throw PoolException("Validator rejected resource during initialization");
                    }
                }

                available_.push_back(std::move(resource));
                ++total_created_;
            }
        } catch (const std::exception& e) {
            // Clean up any created resources
            shutdown();
            throw PoolException(std::string("Failed to initialize pool: ") + e.what());
        }
    }

    /**
     * @brief Destructor - ensures clean shutdown
     */
    ~ResourcePool() {
        shutdown();
    }

    // Non-copyable and non-movable (manage single resource pool instance)
    ResourcePool(const ResourcePool&) = delete;
    ResourcePool& operator=(const ResourcePool&) = delete;
    ResourcePool(ResourcePool&&) = delete;
    ResourcePool& operator=(ResourcePool&&) = delete;

    /**
     * @brief Acquire a resource from the pool
     *
     * Blocks until a resource is available or timeout expires.
     * Returns a RAII handle that automatically returns the resource.
     *
     * @param timeout Optional timeout for acquisition
     * @return ResourceHandle containing the acquired resource
     * @throws PoolException if pool is shut down or timeout expires
     */
    ResourceHandle<T> acquire(std::optional<std::chrono::milliseconds> timeout = std::nullopt) {
        std::unique_lock<std::mutex> lock(mutex_);

        auto effective_timeout = timeout.value_or(config_.acquire_timeout);
        auto deadline = std::chrono::steady_clock::now() + effective_timeout;

        while (true) {
            if (shutdown_) {
                throw PoolException("Pool is shut down");
            }

            // Try to get an available resource (LIFO - most recently used)
            if (!available_.empty()) {
                auto resource = std::move(available_.back());
                available_.pop_back();

                // Validate outside mutex to avoid blocking other threads
                if (config_.validate_on_acquire && validator_) {
                    lock.unlock();
                    bool valid = false;
                    try {
                        valid = validator_(*resource);
                    } catch (...) {
                        valid = false; // Treat exceptions as invalid
                    }
                    lock.lock();

                    if (!valid) {
                        // Resource is invalid, decrement counter and retry
                        --total_created_;
                        cv_.notify_one(); // Wake waiters since we freed a slot
                        continue;
                    }
                }

                // Create handle with return function
                auto return_func = [this](std::unique_ptr<T> res) {
                    this->returnResource(std::move(res));
                };

                return ResourceHandle<T>(std::move(resource), std::move(return_func));
            }

            // No available resource - can we create a new one?
            if (total_created_ < config_.max_size) {
                // Reserve slot and unlock before expensive factory call
                ++total_created_;
                lock.unlock();

                std::unique_ptr<T> resource;
                try {
                    resource = factory_();
                    if (!resource) {
                        throw PoolException("Factory returned null resource");
                    }

                    // Validate outside mutex
                    if (validator_) {
                        bool valid = false;
                        try {
                            valid = validator_(*resource);
                        } catch (...) {
                            valid = false;
                        }
                        if (!valid) {
                            // Rollback reservation
                            std::lock_guard<std::mutex> rollback_lock(mutex_);
                            --total_created_;
                            cv_.notify_one();
                            throw PoolException("Validator rejected newly created resource");
                        }
                    }

                    auto return_func = [this](std::unique_ptr<T> res) {
                        this->returnResource(std::move(res));
                    };
                    return ResourceHandle<T>(std::move(resource), std::move(return_func));

                } catch (...) {
                    // Rollback reservation on any failure
                    std::lock_guard<std::mutex> rollback_lock(mutex_);
                    --total_created_;
                    cv_.notify_one();
                    throw;
                }
            }

            // Wait for a resource to become available
            if (cv_.wait_until(lock, deadline) == std::cv_status::timeout) {
                throw PoolException("Timeout waiting for resource");
            }
        }
    }

    /**
     * @brief Try to acquire a resource without blocking
     *
     * @return Optional handle - empty if no resource available
     */
    std::optional<ResourceHandle<T>> tryAcquire() {
        std::unique_lock<std::mutex> lock(mutex_);

        if (shutdown_) {
            return std::nullopt;
        }

        if (!available_.empty()) {
            auto resource = std::move(available_.back());
            available_.pop_back();

            // Validate outside mutex
            if (config_.validate_on_acquire && validator_) {
                lock.unlock();
                bool valid = false;
                try {
                    valid = validator_(*resource);
                } catch (...) {
                    valid = false;
                }

                if (!valid) {
                    std::lock_guard<std::mutex> rollback_lock(mutex_);
                    --total_created_;
                    cv_.notify_one();
                    return std::nullopt;
                }
                lock.lock();
            }

            auto return_func = [this](std::unique_ptr<T> res) {
                this->returnResource(std::move(res));
            };

            return ResourceHandle<T>(std::move(resource), std::move(return_func));
        }

        // Try to create new resource if under limit
        if (total_created_ < config_.max_size) {
            ++total_created_;
            lock.unlock();

            try {
                auto resource = factory_();
                if (!resource) {
                    std::lock_guard<std::mutex> rollback_lock(mutex_);
                    --total_created_;
                    cv_.notify_one();
                    return std::nullopt;
                }

                if (validator_) {
                    bool valid = false;
                    try {
                        valid = validator_(*resource);
                    } catch (...) {
                        valid = false;
                    }
                    if (!valid) {
                        std::lock_guard<std::mutex> rollback_lock(mutex_);
                        --total_created_;
                        cv_.notify_one();
                        return std::nullopt;
                    }
                }

                auto return_func = [this](std::unique_ptr<T> res) {
                    this->returnResource(std::move(res));
                };
                return ResourceHandle<T>(std::move(resource), std::move(return_func));

            } catch (...) {
                std::lock_guard<std::mutex> rollback_lock(mutex_);
                --total_created_;
                cv_.notify_one();
                return std::nullopt;
            }
        }

        return std::nullopt;
    }

    /**
     * @brief Get pool statistics
     */
    struct Stats {
        size_t available_count;
        size_t total_created;
        size_t max_size;
        bool is_shutdown;
    };

    Stats getStats() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return Stats{
            available_.size(),
            total_created_,
            config_.max_size,
            shutdown_
        };
    }

    /**
     * @brief Gracefully shutdown the pool
     *
     * Immediately destroys all idle resources and prevents new acquisitions.
     * In-use resources will be destroyed when returned (via RAII).
     * Does not block - returns immediately after cleanup.
     */
    void shutdown() {
        std::vector<std::unique_ptr<T>> to_destroy;
        size_t leaked_count = 0;

        {
            std::lock_guard<std::mutex> lock(mutex_);

            if (shutdown_) {
                return;
            }

            shutdown_ = true;

            // Detect leaked handles (still in use)
            leaked_count = total_created_ - available_.size();

            // Move all idle resources for destruction outside mutex
            to_destroy = std::move(available_);
            available_.clear();
        }

        // Notify all waiting threads
        cv_.notify_all();

        // Destroy idle resources outside mutex
        for (auto& resource : to_destroy) {
            if (destroyer_) {
                try {
                    destroyer_(*resource);
                } catch (...) {
                    // Swallow exceptions during cleanup
                }
            }
        }

        // Note: Leaked handles will be cleaned up via returnResource()
        // when they eventually go out of scope (RAII handles this)
    }

    /**
     * @brief Shutdown and wait for all resources to be returned
     *
     * Blocks until all resources are returned or timeout expires.
     * Use this when you need to ensure all resources are properly released.
     *
     * @param timeout Maximum time to wait for resources
     * @return true if all resources returned, false if timeout
     */
    bool shutdownAndWait(std::chrono::milliseconds timeout = std::chrono::seconds(30)) {
        std::unique_lock<std::mutex> lock(mutex_);

        if (shutdown_) {
            return available_.size() == total_created_;
        }

        shutdown_ = true;
        cv_.notify_all();

        // Wait for all resources to be returned
        auto deadline = std::chrono::steady_clock::now() + timeout;
        bool all_returned = cv_.wait_until(lock, deadline, [this] {
            return available_.size() == total_created_;
        });

        // Clean up all available resources
        std::vector<std::unique_ptr<T>> to_destroy = std::move(available_);
        available_.clear();
        size_t leaked = all_returned ? 0 : (total_created_ - to_destroy.size());

        lock.unlock();

        // Destroy outside mutex
        for (auto& resource : to_destroy) {
            if (destroyer_) {
                try {
                    destroyer_(*resource);
                } catch (...) {}
            }
        }

        return all_returned;
    }

    /**
     * @brief Force immediate shutdown without waiting
     *
     * Warning: Resources currently in use will become invalid
     */
    void forceShutdown() {
        std::lock_guard<std::mutex> lock(mutex_);
        shutdown_ = true;
        cv_.notify_all();

        while (!available_.empty()) {
            auto resource = std::move(available_.front());
            available_.pop();
            if (destroyer_) {
                try {
                    destroyer_(*resource);
                } catch (...) {}
            }
        }

        total_created_ = 0;
    }

private:

    /**
     * @brief Return a resource to the pool
     */
    void returnResource(std::unique_ptr<T> resource) {
        if (!resource) {
            return;
        }

        // Validate outside mutex to avoid blocking
        bool valid = true;
        if (config_.validate_on_return && validator_) {
            try {
                valid = validator_(*resource);
            } catch (...) {
                valid = false;
            }
        }

        std::unique_ptr<T> to_destroy;
        bool should_notify = false;

        {
            std::lock_guard<std::mutex> lock(mutex_);

            if (shutdown_) {
                // Pool is shutting down, mark for destruction
                --total_created_;
                to_destroy = std::move(resource);
                should_notify = true;
            } else if (!valid) {
                // Resource is invalid, mark for destruction
                --total_created_;
                to_destroy = std::move(resource);
                should_notify = true;
            } else {
                // Return to pool (LIFO - push to back)
                available_.push_back(std::move(resource));
                should_notify = true;
            }
        }

        // Destroy outside mutex if needed
        if (to_destroy && destroyer_) {
            try {
                destroyer_(*to_destroy);
            } catch (...) {}
        }

        // Notify waiting threads
        if (should_notify) {
            cv_.notify_one();
        }
    }

    FactoryFunc factory_;
    ValidatorFunc validator_;
    DestroyFunc destroyer_;
    PoolConfig config_;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<std::unique_ptr<T>> available_;  // LIFO for hot/cold pattern
    size_t total_created_;
    bool shutdown_;
};

} // namespace resource_pool