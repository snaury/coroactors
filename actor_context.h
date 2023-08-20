#pragma once
#include "actor_scheduler.h"
#include "detail/mailbox.h"
#include <cassert>
#include <coroutine>

namespace coroactors {

    class actor_context {
        struct impl {
            actor_scheduler& scheduler;
            detail::TMailbox<std::coroutine_handle<>> mailbox;

            explicit impl(class actor_scheduler& s)
                : scheduler(s)
            {
                // Mailbox must be initially unlocked
                mailbox.TryUnlock();
            }
        };

    public:
        actor_context() noexcept = default;

        actor_context(actor_scheduler& s)
            : impl_(std::make_shared<impl>(s))
        {}

        static actor_context create() {
            auto* s = actor_scheduler::current();
            if (!s) {
                throw std::runtime_error("current thread is missing a scheduler");
            }
            return actor_context(*s);
        }

        explicit operator bool() const {
            return bool(impl_);
        }

        friend bool operator==(const actor_context& a, const actor_context& b) {
            return a.impl_.get() == b.impl_.get();
        }

        friend bool operator!=(const actor_context& a, const actor_context& b) {
            return a.impl_.get() != b.impl_.get();
        }

        actor_scheduler& scheduler() const {
            return impl_->scheduler;
        }

        /**
         * Add continuation to this actor context
         *
         * Returns the next continuation that is runnable on
         * this context (could be scheduled using a scheduler),
         * or nullptr if context may be running somewhere else.
         */
        std::coroutine_handle<> push(std::coroutine_handle<> c) const {
            if (impl_->mailbox.Push(c)) {
                std::coroutine_handle<> k = impl_->mailbox.Pop();
                assert(k == c);
                return k;
            } else {
                return nullptr;
            }
        }

        std::coroutine_handle<> pop() const {
            std::coroutine_handle<> k = impl_->mailbox.Pop();
            return k;
        }

        struct inherit_t {};
        struct reschedule_t {};
        struct reschedule_locked_t {};

        // When awaited will inherit context of the caller
        static constexpr inherit_t inherit;

        // When awaited will reschedule current coroutine allowing other code in the same context to run
        static constexpr reschedule_t reschedule;

        // When awaited will reschedule current coroutine without allowing other code in the same context to run
        static constexpr reschedule_locked_t reschedule_locked;

    private:
        std::shared_ptr<impl> impl_;
    };

    /**
     * A special empty actor context that does not isolate shared state
     */
    static inline const actor_context no_actor_context;

} // namespace coroactors
