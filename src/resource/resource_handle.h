#pragma once

#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
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

        // Pre-allocate initial resources
        try {
            for (size_t i = 0; i < config_.initial_size; ++i) {
                auto resource = createResource();
                if (resource) {
                    available_.push(std::move(resource));
                }
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

            // Try to get an available resource
            if (!available_.empty()) {
                auto resource = std::move(available_.front());
                available_.pop();

                // Validate if configured
                if (config_.validate_on_acquire && validator_) {
                    if (!validator_(*resource)) {
                        // Resource is invalid, try to create a new one
                        --total_created_;
                        resource = createResourceLocked();
                        if (!resource) {
                            continue; // Try again
                        }
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
                auto resource = createResourceLocked();
                if (resource) {
                    auto return_func = [this](std::unique_ptr<T> res) {
                        this->returnResource(std::move(res));
                    };
                    return ResourceHandle<T>(std::move(resource), std::move(return_func));
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
        std::lock_guard<std::mutex> lock(mutex_);

        if (shutdown_) {
            return std::nullopt;
        }

        if (!available_.empty()) {
            auto resource = std::move(available_.front());
            available_.pop();

            // Validate if configured
            if (config_.validate_on_acquire && validator_ && !validator_(*resource)) {
                --total_created_;
                return std::nullopt;
            }

            auto return_func = [this](std::unique_ptr<T> res) {
                this->returnResource(std::move(res));
            };

            return ResourceHandle<T>(std::move(resource), std::move(return_func));
        }

        // Try to create new resource if under limit
        if (total_created_ < config_.max_size) {
            auto resource = createResourceLocked();
            if (resource) {
                auto return_func = [this](std::unique_ptr<T> res) {
                    this->returnResource(std::move(res));
                };
                return ResourceHandle<T>(std::move(resource), std::move(return_func));
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
     * Waits for all resources to be returned before destroying them.
     * This is called automatically by the destructor.
     */
    void shutdown() {
        std::unique_lock<std::mutex> lock(mutex_);

        if (shutdown_) {
            return;
        }

        shutdown_ = true;
        cv_.notify_all();

        // Wait for all resources to be returned with timeout
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (available_.size() < total_created_) {
            if (cv_.wait_until(lock, deadline) == std::cv_status::timeout) {
                // Force shutdown after timeout
                break;
            }
        }

        // Clean up all available resources
        while (!available_.empty()) {
            auto resource = std::move(available_.front());
            available_.pop();

            if (destroyer_) {
                try {
                    destroyer_(*resource);
                } catch (...) {
                    // Swallow exceptions during cleanup
                }
            }
            // unique_ptr will handle deletion
        }

        total_created_ = 0;
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
     * @brief Create a new resource (must be called with lock held)
     */
    std::unique_ptr<T> createResourceLocked() {
        auto resource = createResource();
        return resource;
    }

    /**
     * @brief Create a new resource (thread-safe)
     */
    std::unique_ptr<T> createResource() {
        try {
            auto resource = factory_();
            if (!resource) {
                throw PoolException("Factory returned null resource");
            }
            ++total_created_;
            return resource;
        } catch (const std::exception& e) {
            throw PoolException(std::string("Failed to create resource: ") + e.what());
        }
    }

    /**
     * @brief Return a resource to the pool
     */
    void returnResource(std::unique_ptr<T> resource) {
        if (!resource) {
            return;
        }

        std::lock_guard<std::mutex> lock(mutex_);

        if (shutdown_) {
            // Pool is shutting down, destroy the resource
            --total_created_;
            if (destroyer_) {
                try {
                    destroyer_(*resource);
                } catch (...) {}
            }
            cv_.notify_all();
            return;
        }

        // Validate if configured
        if (config_.validate_on_return && validator_) {
            if (!validator_(*resource)) {
                // Resource is invalid, don't return it to pool
                --total_created_;
                cv_.notify_all();
                return;
            }
        }

        available_.push(std::move(resource));
        cv_.notify_one();
    }

    FactoryFunc factory_;
    ValidatorFunc validator_;
    DestroyFunc destroyer_;
    PoolConfig config_;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<std::unique_ptr<T>> available_;
    size_t total_created_;
    bool shutdown_;
};

} // namespace resource_pool