#pragma once
#include "actor_context.h"
#include "actor_result.h"
#include <cassert>
#include <functional>
#include <utility>

namespace coroactors {

    template<class T>
    class actor;

} // namespace coroactors

namespace coroactors::detail {

    template<class T>
    class actor_promise;

    template<class T>
    using actor_continuation = std::coroutine_handle<actor_promise<T>>;

    std::coroutine_handle<> switch_context(
        actor_context&& from,
        const actor_context& to,
        std::coroutine_handle<> c)
    {
        if (from == to) {
            // context is not changing
            return c;
        }

        if (!from) {
            // special case: entering context from outside
            c = to.push(c);
            if (c) {
                // avoid monopolizing unexpected thread or function
                to.scheduler().schedule(c);
            }
            return std::noop_coroutine();
        }

        // keep original context
        auto saved = std::move(from);

        if (to) {
            c = to.push(c);
            if (!c) {
                // new context is currently busy
                if (auto next = saved.pop()) {
                    // run the next available continuation unless preempted
                    if (!saved.scheduler().preempt()) {
                        return next;
                    }
                    saved.scheduler().schedule(next);
                }

                return std::noop_coroutine();
            } else if (to.scheduler().preempt()) {
                // preempted by the new context
                if (auto next = saved.pop()) {
                    // schedule the next available continuation
                    saved.scheduler().schedule(next);
                }

                // schedule current continuation too
                to.scheduler().schedule(c);

                return std::noop_coroutine();
            }
        }

        if (auto next = saved.pop()) {
            // schedule the next available continuation
            saved.scheduler().schedule(next);
        }

        // switch to the new context
        return c;
    }

    template<class T>
    class actor_result_handler_base {
    public:
        actor_result<T> result;
    };

    template<class T>
    class actor_result_handler : public actor_result_handler_base<T> {
    public:
        template<class TArg>
        void return_value(TArg&& arg) {
            this->result.set_value(std::forward<TArg>(arg));
        }
    };

    template<>
    class actor_result_handler<void> : public actor_result_handler_base<void> {
    public:
        void return_void() noexcept {
            this->result.set_value();
        }
    };

    template<class T>
    class actor_promise final : public actor_result_handler<T> {
    public:
        [[nodiscard]] actor<T> get_return_object() noexcept {
            return actor<T>(actor_continuation<T>::from_promise(*this));
        }

        void unhandled_exception() noexcept {
            if (!continuation) {
                std::terminate();
            }
            this->result.set_exception(std::current_exception());
        }

        static auto initial_suspend() noexcept { return std::suspend_always{}; }

        struct TFinalSuspend {
            static bool await_ready() noexcept { return false; }
            static void await_resume() noexcept {}

            __attribute__((__noinline__))
            static std::coroutine_handle<> await_suspend(actor_continuation<T> h) noexcept {
                auto& p = h.promise();
                auto next = std::exchange(p.continuation, {});
                if (!next) {
                    // actor was detached, find more work from our context
                    if (p.context) {
                        next = p.context.pop();
                    }
                    if (!next) {
                        next = std::noop_coroutine();
                    }
                    h.destroy();
                    return next;
                }
                auto to = std::move(p.continuation_context);
                return switch_context(std::move(p.context), to, next);
            }
        };

        static auto final_suspend() noexcept { return TFinalSuspend{}; }

        void set_context(const actor_context& c) noexcept {
            context = c;
        }

        void set_continuation(std::coroutine_handle<> cont) noexcept {
            continuation = cont;
        }

        void set_continuation(std::coroutine_handle<> cont, const actor_context& c) noexcept {
            continuation = cont;
            continuation_context = c;
            if (!context) {
                // inherit context
                context = c;
            }
        }

        // Actors can use symmetric transfer of context between coroutines
        template<class U>
        actor<U>&& await_transform(actor<U>&& other) noexcept {
            return (actor<U>&&)other;
        }

#if 0
        // TODO: need a release/reacquire wrapper here
        template<class TAwaitable>
        TAwaitable&& await_transform(TAwaitable&& awaitable) noexcept
            requires (!std::convertible_to<TAwaitable&&, const actor_context&>)
        {
            return (TAwaitable&&)awaitable;
        }
#endif

        struct TSwitchContextAwaiter {
            const actor_context& to;
            const bool ready;

            bool await_ready() noexcept {
                return ready;
            }

            __attribute__((__noinline__))
            std::coroutine_handle<> await_suspend(actor_continuation<T> c) noexcept {
                assert(!ready);
                auto& self = c.promise();
                auto from = std::move(self.context);
                self.context = to;
                return switch_context(std::move(from), self.context, c);
            }

            void await_resume() noexcept {
                // nothing
            }
        };

        TSwitchContextAwaiter await_transform(const actor_context& to) noexcept {
            if (context == to) {
                return TSwitchContextAwaiter{ to, true };
            }
            if (!to) {
                // switching to an empty context, release without suspending
                auto from = std::move(context);
                context = to;
                assert(from);
                if (auto next = from.pop()) {
                    from.scheduler().schedule(next);
                }
                return TSwitchContextAwaiter{ to, true };
            }
            // We need to suspend and resume on the new context
            return TSwitchContextAwaiter{ to, false };
        }

        struct TRescheduleAwaiter {
            bool await_ready() { return false; }

            __attribute__((__noinline__))
            void await_suspend(actor_continuation<T> c) noexcept {
                auto& self = c.promise();
                assert(self.context && "Cannot reschedule without a context");
                auto& scheduler = self.context.scheduler();
                // note: current context is locked
                auto next = self.context.push(c);
                assert(!next && "Unexpected continuation from a locked context");
                next = self.context.pop();
                if (next) {
                    scheduler.schedule(next);
                }
            }

            void await_resume() noexcept {
                // nothing
            }
        };

        auto await_transform(actor_context::reschedule_t) noexcept {
            return TRescheduleAwaiter{};
        }

        struct TRescheduleLockedAwaiter {
            bool await_ready() { return false; }

            __attribute__((__noinline__))
            void await_suspend(actor_continuation<T> c) noexcept {
                auto& self = c.promise();
                assert(self.context && "Cannot reschedule without a context");
                auto& scheduler = self.context.scheduler();
                scheduler.schedule(c);
            }

            void await_resume() noexcept {
                // nothing
            }
        };

        auto await_transform(actor_context::reschedule_locked_t) noexcept {
            return TRescheduleLockedAwaiter{};
        }

    public:
        actor_context context;

    private:
        std::coroutine_handle<> continuation;
        actor_context continuation_context;
    };

    template<class T>
    class actor_awaiter {
    public:
        explicit actor_awaiter(actor_continuation<T> h) noexcept
            : handle(h)
        {}

        actor_awaiter(actor_awaiter&& rhs)
            : handle(std::exchange(rhs.handle, {}))
        {}

        actor_awaiter& operator=(const actor_awaiter&) = delete;
        actor_awaiter& operator=(actor_awaiter&&) = delete;

        ~actor_awaiter() noexcept {
            if (handle) {
                handle.destroy();
            }
        }

        bool await_ready() noexcept { return false; }

        template<class U>
        __attribute__((__noinline__))
        std::coroutine_handle<> await_suspend(actor_continuation<U> c) noexcept {
            auto& p = handle.promise();
            p.set_continuation(c, c.promise().context);
            return handle;
        }

        __attribute__((__noinline__))
        std::coroutine_handle<> await_suspend(std::coroutine_handle<> c) noexcept {
            auto& p = handle.promise();
            p.set_continuation(c);
            return handle;
        }

        detail::rvalue<T> await_resume() {
            return std::move(handle.promise().result).take();
        }

    private:
        actor_continuation<T> handle;
    };

} // namespace coroactors::detail

namespace coroactors {

    /**
     * Used as the return type of actor coroutines
     *
     * Inherits caller context when awaited, unless context is set explicitly.
     */
    template<class T>
    class actor {
        friend class detail::actor_promise<T>;

    private:
        explicit actor(detail::actor_continuation<T> handle) noexcept
            : handle(handle)
        {}

    public:
        using promise_type = detail::actor_promise<T>;

    public:
        actor() noexcept = default;

        actor(actor&& rhs) noexcept
            : handle(std::exchange(rhs.handle, {}))
        {}

        ~actor() noexcept {
            if (handle) {
                handle.destroy();
            }
        }

        actor& operator=(actor&& rhs) noexcept {
            if (this != &rhs) {
                auto prev = std::exchange(handle, {});
                handle = std::exchange(rhs.handle, {});
                if (prev) {
                    prev.destroy();
                }
            }
            return *this;
        }

        explicit operator bool() const noexcept {
            return bool(handle);
        }

        actor&& with(const actor_context& context) && noexcept {
            handle.promise().set_context(context);
            return std::move(*this);
        }

        auto operator co_await() && noexcept {
            return detail::actor_awaiter(std::exchange(handle, {}));
        }

        void detach() && noexcept {
            std::exchange(handle, {}).resume();
        }

    private:
        detail::actor_continuation<T> handle;
    };

} // namespace coroactors
