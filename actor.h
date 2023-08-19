#pragma once
#include "actor_context.h"
#include "detail/awaiters.h"
#include "detail/result.h"
#include "with_resume_callback.h"
#include <cassert>
#include <functional>
#include <type_traits>
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
            // We must be careful to never switch directly, because a very
            // common case might be starting an actor coroutine in some task
            // group (initial context is empty) which then makes a call and
            // awaits some other context. This might all happen recursively
            // from another actor coroutine which will be blocked until all
            // synchronous parts complete, which might take a very long time.
            // We cannot detect this recursion right now, so it's safer to
            // always reschedule continuations.
            c = to.push(c);
            if (c) {
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
        result<T> result_;
    };

    template<class T>
    class actor_result_handler : public actor_result_handler_base<T> {
    public:
        template<class TArg>
        void return_value(TArg&& arg) {
            this->result_.set_value(std::forward<TArg>(arg));
        }
    };

    template<>
    class actor_result_handler<void> : public actor_result_handler_base<void> {
    public:
        void return_void() noexcept {
            this->result_.set_value();
        }
    };

    template<class T>
    class actor_promise final : public actor_result_handler<T> {
        template<class U>
        friend class actor_promise;

    public:
        [[nodiscard]] actor<T> get_return_object() noexcept {
            return actor<T>(actor_continuation<T>::from_promise(*this));
        }

        void unhandled_exception() noexcept {
            if (!continuation) {
                std::terminate();
            }
            this->result_.set_exception(std::current_exception());
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
            explicit_context = true;
        }

        template<class U>
        void set_continuation(actor_continuation<U> c) noexcept {
            continuation = c;
            continuation_context = c.promise().context;
            continuation_is_actor = true;
            if (!explicit_context) {
                context = continuation_context;
            }
        }

        void set_continuation(std::coroutine_handle<> c) noexcept {
            continuation = c;
        }

        std::coroutine_handle<> start(std::coroutine_handle<> c) noexcept {
            // Context is normally inherited (and matches continuation_context),
            // or we are starting a detached coroutine (and both are empty), but
            // when using with_context the context may be different.
            if (continuation_context == context) {
                // Transfer directly without context changes
                return c;
            }

            // We are switching from continuation context to our context
            auto saved = continuation_context;
            return switch_context(std::move(saved), context, c);
        }

        void detach(std::coroutine_handle<> c) noexcept {
            if (context) {
                // Detached with explicit context
                c = context.push(c);
                if (c) {
                    context.scheduler().schedule(c);
                }
            } else {
                c.resume();
            }
        }

        // Actors can use symmetric transfer of context between coroutines
        template<class U>
        actor<U>&& await_transform(actor<U>&& other) noexcept {
            return (actor<U>&&)other;
        }

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

        std::coroutine_handle<> wrap_restore_context(std::coroutine_handle<> c) {
            if (!context) {
                // There is nothing to restore
                return c;
            }

            // Generate a wrapped continuation that will restore context on resume
            return with_resume_callback([this, c]() noexcept -> std::coroutine_handle<> {
                if (auto next = context.push(c)) {
                    if (!context.scheduler().preempt()) {
                        // Run directly unless preempted by scheduler
                        return next;
                    }
                    context.scheduler().schedule(next);
                }
                return std::noop_coroutine();
            });
        }

        void release_context() {
            if (context) {
                if (auto next = context.pop()) {
                    context.scheduler().schedule(next);
                }
            }
        }

        std::coroutine_handle<> next_from_context() {
            if (context) {
                if (auto next = context.pop()) {
                    return next;
                }
            }
            return std::noop_coroutine();
        }

        template<detail::awaiter TAwaiter>
        struct TContextReleaseRestoreAwaiter {
            using TResult = decltype(std::declval<TAwaiter&>().await_resume());

            TAwaiter awaiter;

            bool await_ready()
                noexcept(has_noexcept_await_ready<TAwaiter>)
            {
                return awaiter.await_ready();
            }

            __attribute__((__noinline__))
            std::coroutine_handle<> await_suspend(actor_continuation<T> c)
                noexcept(detail::has_noexcept_await_suspend<TAwaiter>)
                requires (detail::has_await_suspend_void<TAwaiter>)
            {
                auto& self = c.promise();
                auto k = self.wrap_restore_context(c);
                awaiter.await_suspend(std::move(k));
                // We still have context locked, move to the next task
                return self.next_from_context();
            }

            __attribute__((__noinline__))
            std::coroutine_handle<> await_suspend(actor_continuation<T> c)
                noexcept(detail::has_noexcept_await_suspend<TAwaiter>)
                requires (detail::has_await_suspend_bool<TAwaiter>)
            {
                auto& self = c.promise();
                auto k = self.wrap_restore_context(c);
                if (!awaiter.await_suspend(std::move(k))) {
                    // Awaiter did not suspend, transfer back directly
                    self.release_context();
                    return k;
                }
                // We still have context locked, move to the next task
                return self.next_from_context();
            }

            __attribute__((__noinline__))
            std::coroutine_handle<> await_suspend(actor_continuation<T> c)
                noexcept(detail::has_noexcept_await_suspend<TAwaiter>)
                requires (detail::has_await_suspend_handle<TAwaiter>)
            {
                auto& self = c.promise();
                auto k = awaiter.await_suspend(self.wrap_restore_context(c));
                if (k != std::noop_coroutine()) {
                    // Transfer directly to the task from awaiter
                    self.release_context();
                    return k;
                }
                // We still have context locked, move to the next task
                return self.next_from_context();
            }

            TResult await_resume()
                noexcept(detail::has_noexcept_await_resume<TAwaiter>)
            {
                return awaiter.await_resume();
            }
        };

        template<detail::awaitable TAwaitable>
        auto await_transform(TAwaitable&& awaitable) noexcept {
            using TAwaiter = std::remove_reference_t<decltype(detail::get_awaiter((TAwaitable&&) awaitable))>;
            return TContextReleaseRestoreAwaiter<TAwaiter>{
                detail::get_awaiter((TAwaitable&&) awaitable)
            };
        }

    private:
        actor_context context;
        std::coroutine_handle<> continuation;
        actor_context continuation_context;
        bool continuation_is_actor = false;
        bool explicit_context = false;
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

        template<class TPromise>
        __attribute__((__noinline__))
        std::coroutine_handle<> await_suspend(std::coroutine_handle<TPromise> c) noexcept {
            auto& p = handle.promise();
            p.set_continuation(c);
            return p.start(handle);
        }

        std::add_rvalue_reference_t<T> await_resume() {
            return std::move(handle.promise().result_).take();
        }

    private:
        actor_continuation<T> handle;
    };

    template<class T>
    class actor_result_awaiter {
    public:
        explicit actor_result_awaiter(actor_continuation<T> h) noexcept
            : handle(h)
        {}

        actor_result_awaiter(actor_result_awaiter&& rhs)
            : handle(std::exchange(rhs.handle, {}))
        {}

        actor_result_awaiter& operator=(const actor_result_awaiter&) = delete;
        actor_result_awaiter& operator=(actor_result_awaiter&&) = delete;

        ~actor_result_awaiter() noexcept {
            if (handle) {
                handle.destroy();
            }
        }

        bool await_ready() noexcept { return false; }

        template<class TPromise>
        __attribute__((__noinline__))
        std::coroutine_handle<> await_suspend(std::coroutine_handle<TPromise> c) noexcept {
            auto& p = handle.promise();
            p.set_continuation(c);
            return p.start(handle);
        }

        result<T>&& await_resume() {
            return std::move(handle.promise().result_);
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
        using result_type = detail::result<T>;
        using value_type = T;

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

        actor&& with_context(const actor_context& context) && noexcept {
            handle.promise().set_context(context);
            return std::move(*this);
        }

        actor&& without_context() && noexcept {
            handle.promise().set_context(actor_context());
            return std::move(*this);
        }

        auto operator co_await() && noexcept {
            return detail::actor_awaiter(std::exchange(handle, {}));
        }

        auto result() && noexcept {
            return detail::actor_result_awaiter(std::exchange(handle, {}));
        }

        void detach() && noexcept {
            auto& p = handle.promise();
            return p.detach(std::exchange(handle, {}));
        }

    private:
        detail::actor_continuation<T> handle;
    };

} // namespace coroactors
