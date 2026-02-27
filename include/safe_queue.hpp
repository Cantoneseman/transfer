#pragma once

#include <queue>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <optional>
#include <atomic>

// ============================================================================
// HYPER-TRANSFER v2.0 - Thread-Safe Queue
// ============================================================================
// A blocking queue for producer-consumer patterns in multi-threaded pipelines.
// Supports graceful shutdown via stop() and timeout-based operations.
// ============================================================================

namespace hyper {

/**
 * @brief Thread-safe blocking queue for producer-consumer patterns.
 * 
 * @tparam T Type of elements stored in the queue
 * 
 * Features:
 * - Blocking push/pop with condition variable signaling
 * - Graceful shutdown support via stop()
 * - Timeout-based pop for responsive shutdown
 * - Move semantics for efficient large object handling
 */
template<typename T>
class SafeQueue {
public:
    SafeQueue() : stopped_(false) {}
    
    ~SafeQueue() {
        stop();
    }
    
    // Non-copyable
    SafeQueue(const SafeQueue&) = delete;
    SafeQueue& operator=(const SafeQueue&) = delete;
    
    // Movable
    SafeQueue(SafeQueue&& other) noexcept {
        std::lock_guard<std::mutex> lock(other.mutex_);
        queue_ = std::move(other.queue_);
        stopped_ = other.stopped_.load();
    }
    
    SafeQueue& operator=(SafeQueue&& other) noexcept {
        if (this != &other) {
            std::scoped_lock lock(mutex_, other.mutex_);
            queue_ = std::move(other.queue_);
            stopped_ = other.stopped_.load();
        }
        return *this;
    }
    
    /**
     * @brief Push an item to the queue (copy version).
     * 
     * Thread-safe. Notifies one waiting consumer.
     * 
     * @param value Item to push
     * @return true if pushed successfully, false if queue is stopped
     */
    bool push(const T& value) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopped_) {
                return false;
            }
            queue_.push(value);
        }
        cond_.notify_one();
        return true;
    }
    
    /**
     * @brief Push an item to the queue (move version).
     * 
     * Thread-safe. Notifies one waiting consumer.
     * Preferred for large objects to avoid copying.
     * 
     * @param value Item to push (will be moved)
     * @return true if pushed successfully, false if queue is stopped
     */
    bool push(T&& value) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopped_) {
                return false;
            }
            queue_.push(std::move(value));
        }
        cond_.notify_one();
        return true;
    }
    
    /**
     * @brief Construct an item in-place in the queue.
     * 
     * @tparam Args Constructor argument types
     * @param args Arguments forwarded to T's constructor
     * @return true if emplaced successfully, false if queue is stopped
     */
    template<typename... Args>
    bool emplace(Args&&... args) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopped_) {
                return false;
            }
            queue_.emplace(std::forward<Args>(args)...);
        }
        cond_.notify_one();
        return true;
    }
    
    /**
     * @brief Pop an item from the queue (blocking).
     * 
     * Blocks until an item is available or the queue is stopped.
     * 
     * @param value Output parameter for the popped item
     * @return true if an item was popped, false if queue is stopped and empty
     */
    bool pop(T& value) {
        std::unique_lock<std::mutex> lock(mutex_);
        
        cond_.wait(lock, [this] {
            return !queue_.empty() || stopped_;
        });
        
        if (queue_.empty()) {
            // Queue is stopped and empty
            return false;
        }
        
        value = std::move(queue_.front());
        queue_.pop();
        return true;
    }
    
    /**
     * @brief Pop an item with timeout.
     * 
     * Blocks until an item is available, timeout expires, or queue is stopped.
     * 
     * @param value Output parameter for the popped item
     * @param timeout_ms Timeout in milliseconds
     * @return true if an item was popped, false on timeout or if stopped
     */
    bool pop_with_timeout(T& value, int timeout_ms) {
        std::unique_lock<std::mutex> lock(mutex_);
        
        bool success = cond_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [this] {
            return !queue_.empty() || stopped_;
        });
        
        if (!success || queue_.empty()) {
            // Timeout or stopped and empty
            return false;
        }
        
        value = std::move(queue_.front());
        queue_.pop();
        return true;
    }
    
    /**
     * @brief Try to pop an item without blocking.
     * 
     * @param value Output parameter for the popped item
     * @return true if an item was popped, false if queue is empty
     */
    bool try_pop(T& value) {
        std::lock_guard<std::mutex> lock(mutex_);
        
        if (queue_.empty()) {
            return false;
        }
        
        value = std::move(queue_.front());
        queue_.pop();
        return true;
    }
    
    /**
     * @brief Try to pop, returning optional.
     * 
     * @return std::optional<T> containing the item, or std::nullopt if empty
     */
    std::optional<T> try_pop() {
        std::lock_guard<std::mutex> lock(mutex_);
        
        if (queue_.empty()) {
            return std::nullopt;
        }
        
        T value = std::move(queue_.front());
        queue_.pop();
        return value;
    }
    
    /**
     * @brief Signal all waiting threads to stop.
     * 
     * After calling stop():
     * - All blocking pop() calls will return false once queue is empty
     * - push() calls will return false
     * - Remaining items can still be popped with try_pop()
     */
    void stop() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopped_ = true;
        }
        cond_.notify_all();
    }
    
    /**
     * @brief Check if the queue has been stopped.
     */
    bool is_stopped() const {
        return stopped_;
    }
    
    /**
     * @brief Check if the queue is empty.
     * 
     * Note: Result may be stale by the time caller acts on it.
     */
    bool empty() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.empty();
    }
    
    /**
     * @brief Get the current size of the queue.
     * 
     * Note: Result may be stale by the time caller acts on it.
     */
    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }
    
    /**
     * @brief Clear all items from the queue.
     */
    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        std::queue<T> empty;
        std::swap(queue_, empty);
    }
    
    /**
     * @brief Reset the queue for reuse after stop().
     * 
     * Clears all items and resets the stopped flag.
     */
    void reset() {
        std::lock_guard<std::mutex> lock(mutex_);
        std::queue<T> empty;
        std::swap(queue_, empty);
        stopped_ = false;
    }

private:
    std::queue<T> queue_;
    mutable std::mutex mutex_;
    std::condition_variable cond_;
    std::atomic<bool> stopped_;
};

} // namespace hyper
