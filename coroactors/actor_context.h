#pragma once
#include <coroactors/actor_scheduler.h>
#include <coroactors/detail/actor_context.h>
#include <coroactors/detail/awaiters.h>
#include <coroactors/detail/mailbox.h>
#include <coroactors/intrusive_ptr.h>
#include <cassert>
#include <coroutine>
#include <stdexcept>
#include <utility>

namespace coroactors {

    class actor_context {
        friend detail::actor_context_frame;
        friend detail::actor_context_manager;

        actor_context(detail::actor_context_state* ptr) noexcept
            : ptr(ptr)
        {}

    public:
        actor_context() noexcept = default;

        actor_context(actor_scheduler& s)
            : ptr(new detail::actor_context_state(s))
        {}

        explicit operator bool() const {
            return bool(ptr);
        }

        friend bool operator<(const actor_context& a, const actor_context& b) {
            return a.ptr < b.ptr;
        }

        friend bool operator==(const actor_context& a, const actor_context& b) {
            return a.ptr == b.ptr;
        }

        actor_scheduler& scheduler() const {
            if (!ptr) [[unlikely]] {
                throw std::invalid_argument("empty context doesn't have a scheduler");
            }
            return ptr->scheduler;
        }

    public:
        /**
         * Returns the context frame management API
         */
        detail::actor_context_manager manager() const;

    public:
        /**
         * Returns an awaiter that resumes at the specified deadline, or when
         * current stop token is cancelled. The result will be true on deadline
         * and false on cancellation.
         */
        auto sleep_until(actor_scheduler::time_point deadline) const {
            return detail::sleep_until_awaiter(
                ptr ? &ptr->scheduler : nullptr, deadline);
        }

        /**
         * Returns an awaiter that resumes after the specified timeout, or when
         * current stop token is cancelled. The result will be true on deadline
         * and false on cancellation.
         */
        auto sleep_for(actor_scheduler::duration timeout) const {
            return sleep_until(actor_scheduler::clock_type::now() + timeout);
        }

        /**
         * Returns a wrapped awaitable that will be cancelled at the deadline
         */
        template<detail::awaitable Awaitable>
        auto with_deadline(actor_scheduler::time_point deadline, Awaitable&& awaitable) const {
            return detail::with_deadline_awaiter<Awaitable>(
                std::forward<Awaitable>(awaitable),
                ptr ? &ptr->scheduler : nullptr, deadline);
        }

        /**
         * Returns a wrapped awaitable that will be cancelled after a timeout
         */
        template<detail::awaitable Awaitable>
        auto with_timeout(actor_scheduler::duration timeout, Awaitable&& awaitable) const {
            return with_deadline(actor_scheduler::clock_type::now() + timeout,
                std::forward<Awaitable>(awaitable));
        }

    public:
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
        static constexpr caller_context_t caller_context{};

        /**
         * A placeholder type for `actor_context::current_context`
         */
        struct current_context_t {};

        /**
         * Placeholder for the context of the current actor
         */
        static constexpr current_context_t current_context{};

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
        static constexpr yield_t yield{};

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
        static constexpr preempt_t preempt{};

        /**
         * A placeholder type for `actor_context::current_stop_token`
         */
        struct current_stop_token_t {};

        /**
         * Returns a stop token associated with the current actor
         */
        static constexpr current_stop_token_t current_stop_token{};

    private:
        intrusive_ptr<detail::actor_context_state> ptr;
    };

    /**
     * A special empty actor context that does not isolate shared state
     */
    inline const actor_context no_actor_context{};

} // namespace coroactors
