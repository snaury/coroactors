#pragma once
#include <coroactors/actor_scheduler.h>
#include <coroactors/detail/awaiters.h>
#include <coroactors/detail/mailbox.h>
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

        /**
         * Returns the next continuation from this actor context
         *
         * May only be used when this context is currently running
         */
        std::coroutine_handle<> pop() const {
            std::coroutine_handle<> k = impl_->mailbox.Pop();
            return k;
        }

        /**
         * A placeholder type for `actor_context::operator()`
         */
        template<detail::awaitable Awaitable>
        struct bind_awaitable_t {
            Awaitable&& awaitable;
            const actor_context& context;
        };

        /**
         * Returns an awaitable that will switch to this context before returning
         */
        template<detail::awaitable Awaitable>
        bind_awaitable_t<Awaitable> operator()(Awaitable&& awaitable) const {
            return bind_awaitable_t<Awaitable>{
                std::forward<Awaitable>(awaitable),
                *this,
            };
        }

        /**
         * A placeholder type for `actor_context::caller_context`
         */
        struct caller_context_t {
            /**
             * A placeholder type for `actor_context::caller_context(...)`
             */
            template<detail::awaitable Awaitable>
            struct bind_awaitable_t {
                Awaitable&& awaitable;
            };

            /**
             * Returns an awaitable that will switch to a caller context before returning
             */
            template<detail::awaitable Awaitable>
            bind_awaitable_t<Awaitable> operator()(Awaitable&& awaitable) const {
                return bind_awaitable_t<Awaitable>{
                    std::forward<Awaitable>(awaitable),
                };
            }
        };

        /**
         * Binds to actor coroutine's caller (awaiter) context when awaited
         */
        static constexpr caller_context_t caller_context;

        /**
         * A placeholder type for `actor_context::yield`
         */
        struct yield_t {};

        /**
         * Yields current actor coroutine when awaited
         *
         * Allows running other activies in the same actor context, and may also
         * switch to other actor contexts subject to scheduler preemption.
         */
        static constexpr yield_t yield;

        /**
         * A placeholder type for `actor_context::preempt`
         */
        struct preempt_t {};

        /**
         * Preempts current actor coroutine when awaited
         *
         * Allows running other activities in other actor contexts, but no other
         * activities in the same actor context will run until it returns.
         */
        static constexpr preempt_t preempt;

    private:
        std::shared_ptr<impl> impl_;
    };

    /**
     * A special empty actor context that does not isolate shared state
     */
    static inline const actor_context no_actor_context;

} // namespace coroactors
