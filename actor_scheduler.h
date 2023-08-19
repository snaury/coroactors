#pragma once
#include <coroutine>

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
         * Returns scheduler bound to the current thread
         */
        static actor_scheduler* current() {
            return current_;
        }

        /**
         * Sets scheduler bound to the current thread
         */
        static void set(actor_scheduler* s) {
            current_ = s;
        }

    private:
        static inline thread_local actor_scheduler* current_{ nullptr };
    };

} // namespace coroactors
