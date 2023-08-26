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
         * Post h to resume, likely using a global queue
         *
         * Should be used when tasks fork, e.g. when actor coroutine is
         * starting and switches from an empty context, or when we start a
         * different chain of continuations in parallel to currently executing.
         *
         * Usually slow, corresponds to asio::post
         */
        virtual void post(std::coroutine_handle<> h) = 0;

        /**
         * Defer h to resume, likely using a local queue
         *
         * Could be used when a chain of continuations cannot continue (e.g.
         * returning from an external async operation) and when we are sure
         * we would return to scheduler soon. Care must be taken not to use
         * this when current thread will be busy with other work without
         * returning to scheduler, as h would be stuck in local queue until
         * that time.
         *
         * Usually fast, corresponds to asio::defer
         */
        virtual void defer(std::coroutine_handle<> h) {
            post(h); // default to post
        }

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
