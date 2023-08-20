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

    template<class TAwaitable, class T>
    concept actor_passthru_awaitable = requires {
        typename TAwaitable::is_actor_passthru_awaitable;
    } && awaitable<TAwaitable, actor_promise<T>>;

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
        result<T> result;
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
        ~actor_promise() noexcept {
            if (continuation) {
                // We have a continuation, but promise is destroyed before
                // coroutine has finished. This means it was destroyed while
                // suspended and all we can do is destroy continuation as well.
                continuation.destroy();
            }
        }

        [[nodiscard]] actor<T> get_return_object() noexcept {
            return actor<T>(actor_continuation<T>::from_promise(*this));
        }

        void unhandled_exception() noexcept {
            this->result.set_exception(std::current_exception());
        }

        static auto initial_suspend() noexcept { return std::suspend_never{}; }

        struct TFinalSuspend {
            static bool await_ready() noexcept { return false; }
            static void await_resume() noexcept {}

            __attribute__((__noinline__))
            static std::coroutine_handle<> await_suspend(actor_continuation<T> h) noexcept {
                auto& p = h.promise();
                p.finished = true;

                auto next = std::exchange(p.continuation, {});
                if (!next) {
                    // We have no continuation, it may be because it finished
                    // without any co_awaits, or maybe it was detached.
                    if (!p.context_initialized) {
                        // We did not reach a co_await context point
                        // Suspend until actor is co_awaited
                        return std::noop_coroutine();
                    }

                    // Actor finished after a call to detach, see if there's
                    // some other continuation available in the same context
                    if (p.context) {
                        next = p.context.pop();
                        if (next && p.context.scheduler().preempt()) {
                            p.context.scheduler().schedule(next);
                            next = nullptr;
                        }
                    }
                    if (!next) {
                        next = std::noop_coroutine();
                    }
                    // We have to destroy ourselves, since nobody is waiting
                    h.destroy();
                    return next;
                }

                auto to = std::move(p.continuation_context);
                return switch_context(std::move(p.context), to, next);
            }
        };

        static auto final_suspend() noexcept { return TFinalSuspend{}; }

        template<class U>
        void set_continuation(actor_continuation<U> c) noexcept {
            if (context_inherited) {
                // We will start with the same context
                context = c.promise().get_context();
                context_initialized = true;
            }
            assert(context_initialized);
            assert(!finished);
            continuation = c;
            continuation_context = c.promise().get_context();
            continuation_is_actor = true;
        }

        void set_continuation(std::coroutine_handle<> c) noexcept {
            if (context_inherited) {
                // We will start with an empty context
                context_initialized = true;
            }
            assert(context_initialized);
            assert(!finished);
            continuation = c;
        }

        void unset_continuation() noexcept {
            continuation = nullptr;
        }

        std::coroutine_handle<> start() noexcept {
            std::coroutine_handle<> c = actor_continuation<T>::from_promise(*this);

            // Transfer directly if awaiter context is the same
            if (continuation_context == context) {
                return c;
            }

            // We are switching from continuation context to our context
            auto saved = continuation_context;
            return switch_context(std::move(saved), context, c);
        }

        void detach() noexcept {
            std::coroutine_handle<> c = actor_continuation<T>::from_promise(*this);

            if (context) {
                // Detached with a non-empty context, always reschedule
                c = context.push(c);
                if (c) {
                    context.scheduler().schedule(c);
                }
            } else {
                c.resume();
            }
        }

        enum class ESwitchContext {
            Initial,
            Ready,
            Switch,
        };

        struct TSwitchContextAwaiter {
            const actor_context& to;
            const ESwitchContext type;

            bool await_ready() noexcept {
                return type == ESwitchContext::Ready;
            }

            __attribute__((__noinline__))
            std::coroutine_handle<> await_suspend(actor_continuation<T> c) noexcept {
                if (type == ESwitchContext::Initial) {
                    // This is the first time we suspend
                    return std::noop_coroutine();
                }
                assert(type != ESwitchContext::Ready);
                auto& self = c.promise();
                auto from = std::move(self.context);
                self.context = to;
                return switch_context(std::move(from), self.context, c);
            }

            void await_resume() noexcept {
                // nothing
            }
        };

        TSwitchContextAwaiter await_transform(const actor_context& to) {
            if (!context_initialized) {
                // This is the first time we suspend
                context = to;
                context_initialized = true;
                return TSwitchContextAwaiter{ to, ESwitchContext::Initial };
            }

            if (context == to) {
                // We are not changing context, no op
                return TSwitchContextAwaiter{ to, ESwitchContext::Ready };
            }

            if (!to) {
                // Switching to an empty context, release without suspending
                auto from = std::move(context);
                context = to;
                assert(from);
                if (auto next = from.pop()) {
                    from.scheduler().schedule(next);
                }
                return TSwitchContextAwaiter{ to, ESwitchContext::Ready };
            }

            // We need to suspend and resume on the new context
            return TSwitchContextAwaiter{ to, ESwitchContext::Switch };
        }

        auto await_transform(actor_context::inherit_t) {
            if (context_initialized || context_inherited) {
                [[unlikely]]
                throw std::logic_error("cannot inherit context (already initialized)");
            }

            context_inherited = true;
            return TSwitchContextAwaiter{ no_actor_context, ESwitchContext::Initial };
        }

        void check_context_initialized() const {
            if (!context_initialized) {
                [[unlikely]]
                throw std::logic_error("actor must co_await context first");
            }
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

        auto await_transform(actor_context::reschedule_t) {
            check_context_initialized();
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

        auto await_transform(actor_context::reschedule_locked_t) {
            check_context_initialized();
            return TRescheduleLockedAwaiter{};
        }

        struct TRestoreContextCallback {
            actor_promise<T>& self;
            std::coroutine_handle<> c;

            TRestoreContextCallback(actor_promise<T>& self, std::coroutine_handle<> c) noexcept
                : self(self)
                , c(c)
            {}

            TRestoreContextCallback(TRestoreContextCallback&& rhs) noexcept
                : self(rhs.self)
                , c(std::exchange(rhs.c, {}))
            {}

            ~TRestoreContextCallback() noexcept {
                if (c) {
                    // Callback was not invoked, but we are supposed to resume
                    // our continuation. All we can do now is destroy it and
                    // hope it unwinds its stack correctly.
                    c.destroy();
                }
            }

            TRestoreContextCallback& operator=(const TRestoreContextCallback&) = delete;

            std::coroutine_handle<> operator()() noexcept {
                if (auto next = self.context.push(std::exchange(c, {}))) {
                    if (!self.context.scheduler().preempt()) {
                        // Run directly unless preempted by scheduler
                        return next;
                    }
                    self.context.scheduler().schedule(next);
                }
                return std::noop_coroutine();
            }
        };

        std::coroutine_handle<> wrap_restore_context(std::coroutine_handle<> c) {
            if (!context) {
                // There is nothing to restore
                return c;
            }

            // Generate a wrapped continuation that will restore context on resume
            return with_resume_callback(TRestoreContextCallback{ *this, c });
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

        template<awaiter TAwaiter>
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
                noexcept(has_noexcept_await_suspend<TAwaiter>)
                requires (has_await_suspend_void<TAwaiter>)
            {
                auto& self = c.promise();
                auto k = self.wrap_restore_context(c);
                awaiter.await_suspend(std::move(k));
                // We still have context locked, move to the next task
                return self.next_from_context();
            }

            __attribute__((__noinline__))
            std::coroutine_handle<> await_suspend(actor_continuation<T> c)
                noexcept(has_noexcept_await_suspend<TAwaiter>)
                requires (has_await_suspend_bool<TAwaiter>)
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
                noexcept(has_noexcept_await_suspend<TAwaiter>)
                requires (has_await_suspend_handle<TAwaiter>)
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
                noexcept(has_noexcept_await_resume<TAwaiter>)
            {
                return awaiter.await_resume();
            }
        };

        // N.B.: it's awaitable and not awaitable<actor_promise<T>>
        //       we always supply wrapped awaiter with a type erased handle
        template<awaitable TAwaitable>
        auto await_transform(TAwaitable&& awaitable)
            requires (!actor_passthru_awaitable<TAwaitable, T>)
        {
            check_context_initialized();

            using TAwaiter = std::remove_reference_t<decltype(get_awaiter((TAwaitable&&) awaitable))>;
            return TContextReleaseRestoreAwaiter<TAwaiter>{
                get_awaiter((TAwaitable&&) awaitable)
            };
        }

        // Awaitables marked with is_actor_passthru_awaitable claim to support
        // context switches directly, and those are not transformed.
        template<actor_passthru_awaitable<T> TAwaitable>
        TAwaitable&& await_transform(TAwaitable&& awaitable) {
            check_context_initialized();

            // This awaitable is marked to support context switches directly
            return (TAwaitable&&) awaitable;
        }

        const actor_context& get_context() const {
            return context;
        }

        bool ready() const {
            return finished;
        }

    private:
        actor_context context;
        std::coroutine_handle<> continuation;
        actor_context continuation_context;
        bool continuation_is_actor = false;
        bool context_initialized = false;
        bool context_inherited = false;
        bool finished = false;
    };

    template<class T>
    class actor_awaiter {
    public:
        using is_actor_passthru_awaitable = void;

        explicit actor_awaiter(actor_continuation<T> h) noexcept
            : handle(h)
        {}

        actor_awaiter(actor_awaiter&& rhs)
            : handle(std::exchange(rhs.handle, {}))
        {}

        actor_awaiter& operator=(const actor_awaiter&) = delete;

        ~actor_awaiter() noexcept {
            if (handle) {
                if (active) {
                    handle.promise().unset_continuation();
                } else {
                    handle.destroy();
                }
            }
        }

        bool await_ready() noexcept {
            active = true;
            return handle.promise().ready();
        }

        template<class TPromise>
        __attribute__((__noinline__))
        std::coroutine_handle<> await_suspend(std::coroutine_handle<TPromise> c) noexcept {
            auto& p = handle.promise();
            p.set_continuation(c);
            return p.start();
        }

        std::add_rvalue_reference_t<T> await_resume() {
            active = false;
            return std::move(handle.promise().result).take();
        }

    private:
        actor_continuation<T> handle;
        bool active = false;
    };

    template<class T>
    class actor_result_awaiter {
    public:
        using is_actor_passthru_awaitable = void;

        explicit actor_result_awaiter(actor_continuation<T> h) noexcept
            : handle(h)
        {}

        actor_result_awaiter(actor_result_awaiter&& rhs)
            : handle(std::exchange(rhs.handle, {}))
        {}

        actor_result_awaiter& operator=(const actor_result_awaiter&) = delete;

        ~actor_result_awaiter() noexcept {
            if (handle) {
                if (active) {
                    handle.promise().unset_continuation();
                } else {
                    handle.destroy();
                }
            }
        }

        bool await_ready() noexcept {
            active = true;
            return handle.promise().ready();
        }

        template<class TPromise>
        __attribute__((__noinline__))
        std::coroutine_handle<> await_suspend(std::coroutine_handle<TPromise> c) noexcept {
            auto& p = handle.promise();
            p.set_continuation(c);
            return p.start();
        }

        result<T>&& await_resume() {
            active = false;
            return std::move(handle.promise().result);
        }

    private:
        actor_continuation<T> handle;
        bool active = false;
    };

} // namespace coroactors::detail

namespace coroactors {

    /**
     * Used as the return type of actor coroutines
     *
     * Actor coroutines are eagerly started, but must either return the result,
     * or co_await context first. When bound to a context they will continue
     * execution on that context, automatically releasing it on co_awaits and
     * reacquiring it before they return.
     */
    template<class T>
    class actor {
        friend class detail::actor_promise<T>;

    private:
        explicit actor(detail::actor_continuation<T> handle) noexcept
            : handle(handle)
        {}

    public:
        using is_actor_passthru_awaitable = void;
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

        [[nodiscard]] auto operator co_await() && noexcept {
            return detail::actor_awaiter(std::exchange(handle, {}));
        }

        [[nodiscard]] auto result() && noexcept {
            return detail::actor_result_awaiter(std::exchange(handle, {}));
        }

        void detach() && noexcept {
            auto& p = handle.promise();
            handle = {};
            return p.detach();
        }

    private:
        detail::actor_continuation<T> handle;
    };

} // namespace coroactors
