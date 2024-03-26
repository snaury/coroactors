#pragma once
#include <coroactors/detail/actor_context_base.h>
#include <coroactors/actor_scheduler.h>

namespace coroactors {

    /**
     * Instances of this class are used to isolate shared state
     *
     * When multiple tasks are bound to the same context, only one task will
     * be allowed to run at a time, with additional runnable tasks waiting in
     * the actor context mailbox.
     */
    class actor_context {
        friend detail::async_context_manager;

        explicit actor_context(detail::actor_context_state* ptr) noexcept
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
         * Returns an awaitable that resumes at the specified deadline, or when
         * current stop token is cancelled. The result will be true on deadline
         * and false on cancellation.
         */
        auto sleep_until(actor_scheduler::time_point deadline) const;

        /**
         * Returns an awaitable that resumes after the specified timeout, or when
         * current stop token is cancelled. The result will be true on deadline
         * and false on cancellation.
         */
        auto sleep_for(actor_scheduler::duration timeout) const;

        /**
         * Returns a wrapped awaitable that will be cancelled at the deadline
         */
        template<detail::awaitable Awaitable>
        auto with_deadline(actor_scheduler::time_point deadline, Awaitable&& awaitable) const;

        /**
         * Returns a wrapped awaitable that will be cancelled after a timeout
         */
        template<detail::awaitable Awaitable>
        auto with_timeout(actor_scheduler::duration timeout, Awaitable&& awaitable) const;

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
         * Placeholder for the context of the current actor's caller (awaiter)
         */
        static constexpr struct caller_context_t {
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
        } caller_context{};

        /**
         * Yields current actor coroutine when awaited
         *
         * Allows running other activies in the same actor context, and may also
         * switch to other actor contexts subject to scheduler preemption.
         */
        static constexpr struct yield_t {} yield{};

        /**
         * Preempts current actor coroutine when awaited
         *
         * Allows running other activities in other actor contexts, but no other
         * activities in the same actor context will run until it returns.
         */
        static constexpr struct preempt_t {} preempt{};

    private:
        intrusive_ptr<detail::actor_context_state> ptr;
    };

    /**
     * A special empty actor context that does not isolate shared state
     */
    inline const actor_context no_actor_context{};

} // namespace coroactors
