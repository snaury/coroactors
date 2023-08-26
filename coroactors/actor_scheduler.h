#pragma once
#include <coroactors/stop_token.h>
#include <chrono>
#include <coroutine>
#include <functional>
#include <stdexcept>

namespace coroactors {

    /**
     * A generic actor scheduler interface
     */
    class actor_scheduler {
    protected:
        ~actor_scheduler() = default;

    public:
        using clock_type = std::chrono::steady_clock;
        using time_point = clock_type::time_point;
        using duration = clock_type::duration;

        using schedule_callback_type = std::function<void(bool)>;

        /**
         * Returns true when task switch should preempt
         */
        virtual bool preempt() const {
            // By default we preempt on every context switch
            return true;
        }

        /**
         * Schedules h to run as soon as possible, but not recursively
         */
        virtual void schedule(std::coroutine_handle<> h) = 0;

        /**
         * Schedules a callback c to run at deadline d with a stop token t
         *
         * Callback will be called with a single argument of true when the
         * requested deadline is reached, or false if the request was cancelled
         * or failed with an error. Callback may also be called with the false
         * argument immediately if scheduler does not support timers.
         */
        virtual void schedule(schedule_callback_type c, time_point d, stop_token t) {
            c(false); // don't support timers by default
        }

    public:
        /**
         * Returns scheduler bound to the current thread, or throws an exception
         */
        static actor_scheduler& current() {
            if (current_) {
                [[likely]]
                return *current_;
            }
            throw std::runtime_error("current thread does not have an actor_scheduler");
        }

        /**
         * Returns scheduler bound to the current thread, or nullptr
         */
        static actor_scheduler* current_ptr() noexcept {
            return current_;
        }

        /**
         * Sets scheduler bound to the current thread
         */
        static void set_current_ptr(actor_scheduler* s) noexcept {
            current_ = s;
        }

    private:
        static inline thread_local actor_scheduler* current_{ nullptr };
    };

} // namespace coroactors
