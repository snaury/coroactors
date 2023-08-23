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
            detail::mailbox<std::coroutine_handle<>> mailbox;

            explicit impl(class actor_scheduler& s)
                : scheduler(s)
            {
                // Change mailbox to initially unlocked
                if (!mailbox.try_unlock()) {
                    assert(false && "Unexpected failure to unlock the mailbox");
                }
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
         * Add a new continuation to this actor context
         *
         * Returns it if this context becomes locked and runnable as the result
         * of this push, which could either be resumed directly or scheduled
         * using a scheduler. Returns nullptr if this context is currently
         * locked and/or running, possibly in another thread.
         */
        std::coroutine_handle<> push(std::coroutine_handle<> c) const {
            assert(c && "Attempt to push a nullptr coroutine handle");
            if (impl_->mailbox.emplace(c)) {
                std::coroutine_handle<> k = impl_->mailbox.pop_default();
                assert(k == c);
                return k;
            } else {
                return nullptr;
            }
        }

        /**
         * Returns the next continuation from this actor context or nullptr
         *
         * This context must be locked and running by the caller, otherwise the
         * behavior is undefined. When nullptr is returned the context is
         * unlocked and no longer runnable, which may become locked by another
         * push possibly in another concurrent thread.
         */
        std::coroutine_handle<> pop() const {
            std::coroutine_handle<> k = impl_->mailbox.pop_default();
            return k;
        }

        /**
         * A placeholder type for `actor_context::operator()`
         */
        struct bind_context_t {
            const actor_context& context;
        };

        /**
         * Will switch to this context before returning
        */
        bind_context_t operator()() const {
            return bind_context_t{ *this };
        }

        /**
         * A placeholder type for `actor_context::operator(awaitable)`
         */
        template<detail::awaitable Awaitable>
        struct bind_awaitable_t {
            Awaitable&& awaitable;
            const actor_context& context;
        };

        /**
         * Returns a wrapped awaitable that will release current context before
         * suspending and will switch actor to this context before returning.
         * It is guaranteed that awaitable's await_ready will be called with
         * actor's current context locked however.
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
             * A placeholder type for `actor_context::caller_context()`
             */
            struct bind_context_t {};

            /**
             * Will switch to caller context before returning when awaited
            */
            bind_context_t operator()() const {
                return bind_context_t{};
            }

            /**
             * A placeholder type for `actor_context::caller_context(awaitable)`
             */
            template<detail::awaitable Awaitable>
            struct bind_awaitable_t {
                Awaitable&& awaitable;
            };

            /**
             * Returns a wrapped awaitable that will release current context
             * before suspending and will switch actor caller context before
             * returning. It is guaranteed that awaitable's await_ready will
             * be called with actor's current context locked however.
             */
            template<detail::awaitable Awaitable>
            bind_awaitable_t<Awaitable> operator()(Awaitable&& awaitable) const {
                return bind_awaitable_t<Awaitable>{
                    std::forward<Awaitable>(awaitable),
                };
            }
        };

        /**
         * Placeholder for the context of the current actor's caller (awaiter)
         */
        static constexpr caller_context_t caller_context;

        /**
         * A placeholder type for `actor_context::current_context`
         */
        struct current_context_t {};

        /**
         * Placeholder for the context of the current actor
         */
        static constexpr current_context_t current_context;

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
