#pragma once
#include <coroutine>
#include <stdexcept>

namespace coroactors {

    /**
     * A generic actor scheduler interface
     */
    class actor_scheduler {
    protected:
        ~actor_scheduler() = default;

    public:
        /**
         * Schedules h to run sometime in the future
         */
        virtual void schedule(std::coroutine_handle<> h) = 0;

        /**
         * Returns true when task switch should preempt
         */
        virtual bool preempt() const {
            // By default we preempt on every context switch
            return true;
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
