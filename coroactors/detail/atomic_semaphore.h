#pragma once
#include <atomic>
#include <chrono>
#include <thread>

namespace coroactors::detail {

#if __cpp_lib_atomic_lock_free_type_aliases	>= 201907L
    using semaphore_atomic_t = std::atomic_signed_lock_free;
#elif defined(__linux__)
    // Linux uses int for a futex
    using semaphore_atomic_t = std::atomic<int>;
#else
    // Asume other OSes use int64 for now
    using semaphore_atomic_t = std::atomic<int64_t>;
#endif

    /**
     * A synchronous semaphore based on std::atomic
     */
    class sync_semaphore {
        using clock = std::chrono::high_resolution_clock;

    public:
        using value_type = typename semaphore_atomic_t::value_type;

        /**
         * Initializes semaphore with the specified count or 0 by default
         */
        constexpr sync_semaphore(value_type count = 0) noexcept
            : value_(count * ValueIncrement)
        {}

        /**
         * Increments counter and unblocks one or more acquire waiters
         */
        void release(value_type count = 1) noexcept {
            value_type current = value_.fetch_add(count * ValueIncrement, std::memory_order_release);
            if (current & WaitersMask) {
                if (count > 1) {
                    value_.notify_all();
                } else {
                    value_.notify_one();
                }
            }
        }

        /**
         * Decrements counter and waits until it is large enough
         */
        void acquire(value_type count = 1) noexcept {
            value_type increment = count * ValueIncrement;

            // Fast path: assume no other waiters and notify was called already
            value_type current = increment;
            if (value_.compare_exchange_strong(current, 0, std::memory_order_acquire)) {
                return;
            }

            // Fast path: spin assuming notify will be called very soon
            for (int i = 0; i < 64; ++i) {
                if (current >= increment) {
                    do {
                        if (value_.compare_exchange_weak(current, current - increment, std::memory_order_acquire)) {
                            return;
                        }
                    } while (current >= increment);
                } else {
                    // Note: we don't technically need acquire here, but
                    // spinning without acquire may be optimized away
                    current = value_.load(std::memory_order_acquire);
                }
            }

            // Spin for 64µs, yielding thread after 4µs
            auto start = clock::now();
            for (;;) {
                if (current >= increment) {
                    do {
                        if (value_.compare_exchange_weak(current, current - increment, std::memory_order_acquire)) {
                            return;
                        }
                    } while (current >= increment);
                }
                auto elapsed = clock::now() - start;
                if (elapsed >= std::chrono::microseconds(64)) {
                    break;
                }
                if (elapsed >= std::chrono::microseconds(4)) {
                    std::this_thread::yield();
                }
                // Note: we don't technically need acquire here, but see above
                current = value_.load(std::memory_order_acquire);
            }

            // Increment the number of waiters so notify() calls notify_one
            while (!increment_waiters(current, increment)) {
                do {
                    if (value_.compare_exchange_weak(current, current - increment, std::memory_order_acquire)) {
                        return;
                    }
                } while (current >= increment);
            }

            // Slow path: use atomic's wait method
            for (;;) {
                while (current >= increment) {
                    if (value_.compare_exchange_weak(
                        current,
                        current - increment - decremented_waiters(current),
                        std::memory_order_acquire))
                    {
                        return;
                    }
                }
                // Note: we don't technically need acquire here, but see above
                value_.wait(current, std::memory_order_acquire);
                current = value_.load(std::memory_order_acquire);
            }
        }

    private:
        bool increment_waiters(value_type& current, value_type increment) noexcept {
            if (current >= increment) {
                return false;
            }
            // Increment as long as there are not many waiters
            while ((current & WaitersMask) != WaitersMask) {
                if (value_.compare_exchange_weak(current, current + 1, std::memory_order_relaxed)) {
                    current += 1;
                    return true;
                }
                if (current >= increment) {
                    return false;
                }
            }
            // There are too many waiters, saturate
            return true;
        }

        value_type decremented_waiters(value_type current) noexcept {
            // One we reach too many waiters we don't decrement
            return (current & WaitersMask) == WaitersMask ? 0 : 1;
        }

    private:
        // Lower bits are used for a number of sleeping waiters
        static constexpr value_type WaitersMask = 3;
        static constexpr value_type ValueIncrement = 4;

        semaphore_atomic_t value_;
    };

    /**
     * A synchronous wait group for waiting on multiple tasks
     */
    class sync_wait_group {
    public:
        using value_type = typename semaphore_atomic_t::value_type;

        constexpr sync_wait_group(value_type count = 0) noexcept
            : value_(count)
        {}

        void add(value_type count = 1) noexcept {
            value_.fetch_add(count, std::memory_order_relaxed);
        }

        void done(value_type count = 1) noexcept {
            value_type prev = value_.fetch_sub(count, std::memory_order_release);
            if (prev == count) {
                value_.notify_all();
            }
        }

        void wait() noexcept {
            value_type current = value_.load(std::memory_order_acquire);
            while (current != 0) {
                value_.wait(current, std::memory_order_acquire);
                current = value_.load(std::memory_order_acquire);
            }
        }

    private:
        semaphore_atomic_t value_;
    };

} // namespace coroactors::detail
