#pragma once
#include <coroactors/actor_context.h>
#include <coroactors/detail/awaiters.h>
#include <coroactors/detail/result.h>
#include <coroactors/with_resume_callback.h>
#include <cassert>
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

    template<class Awaitable, class T>
    concept actor_passthru_awaitable = requires {
        typename Awaitable::is_actor_passthru_awaitable;
    } && awaitable<Awaitable, actor_promise<T>>;

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
        result<T>&& take_result() noexcept {
            return std::move(result_);
        }

    protected:
        result<T> result_;
    };

    template<class T>
    class actor_result_handler : public actor_result_handler_base<T> {
    public:
        template<class Value>
        void return_value(Value&& value)
            requires (std::is_convertible_v<Value&&, T>)
        {
            this->result_.set_value(std::forward<Value>(value));
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
    public:
        ~actor_promise() noexcept {
            if (continuation) {
                // We have a continuation, but promise is destroyed before
                // coroutine has finished. This means it was destroyed while
                // suspended and all we can do is destroy continuation as well.
                std::exchange(continuation, {}).destroy();
            }
        }

        actor<T> get_return_object() noexcept {
            return actor<T>(actor_continuation<T>::from_promise(*this));
        }

        void unhandled_exception() noexcept {
            this->result_.set_exception(std::current_exception());
        }

        static auto initial_suspend() noexcept { return std::suspend_never{}; }

        struct final_suspend_t {
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

        static auto final_suspend() noexcept { return final_suspend_t{}; }

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

        bool unset_continuation() noexcept {
            if (continuation) {
                continuation = nullptr;
                return true;
            }
            return false;
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

        struct switch_context_awaiter {
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

        switch_context_awaiter await_transform(actor_context::bind_context_t bound) {
            if (!context_initialized) {
                // This is the first time we suspend
                assert(!context_inherited);
                context = bound.context;
                context_initialized = true;
                return switch_context_awaiter{ bound.context, ESwitchContext::Initial };
            }

            if (context == bound.context) {
                // We are not changing context, no op
                return switch_context_awaiter{ bound.context, ESwitchContext::Ready };
            }

            if (!bound.context) {
                // Switching to an empty context, release without suspending
                auto from = std::move(context);
                context = bound.context;
                assert(from);
                if (auto next = from.pop()) {
                    from.scheduler().schedule(next);
                }
                return switch_context_awaiter{ bound.context, ESwitchContext::Ready };
            }

            // We need to suspend and resume on the new context
            return switch_context_awaiter{ bound.context, ESwitchContext::Switch };
        }

        auto await_transform(actor_context::caller_context_t::bind_context_t) {
            if (!context_initialized) {
                // This is the first time we suspend
                assert(!context_inherited);
                context_inherited = true;
                return switch_context_awaiter{ no_actor_context, ESwitchContext::Initial };
            }

            // Reuse await_transform above for binding to continuation context
            return await_transform(actor_context::bind_context_t{ continuation_context });
        }

        void check_context_initialized() const {
            if (!context_initialized) {
                [[unlikely]]
                throw std::logic_error("actor must co_await context first");
            }
        }

        struct return_context_awaiter_t {
            const actor_context& context;

            bool await_ready() noexcept { return true; }
            bool await_suspend(std::coroutine_handle<>) noexcept { return false; }
            const actor_context& await_resume() noexcept { return context; }
        };

        auto await_transform(actor_context::caller_context_t) {
            check_context_initialized();

            return return_context_awaiter_t{ context };
        }

        auto await_transform(actor_context::current_context_t) {
            check_context_initialized();

            return return_context_awaiter_t{ continuation_context };
        }

        struct yield_context_awaiter_t {
            bool await_ready() noexcept { return false; }

            __attribute__((__noinline__))
            std::coroutine_handle<> await_suspend(actor_continuation<T> c) noexcept {
                auto& self = c.promise();
                if (!self.context) {
                    return c;
                }
                // We push current coroutine to the end of the queue
                auto next = self.context.push(c);
                if (next) {
                    [[unlikely]]
                    throw std::logic_error("Expected no continuation when pushing to a locked context");
                }
                // Check the front of the queue, there's at least one continuation now
                next = self.context.pop();
                if (!next) {
                    [[unlikely]]
                    throw std::logic_error("Expected at least one continuation after pushing to a locked context");
                }
                // Switch to the next coroutine in our context, unless preempted
                auto& scheduler = self.context.scheduler();
                if (scheduler.preempt()) {
                    scheduler.schedule(next);
                    return std::noop_coroutine();
                } else {
                    return next;
                }
            }

            void await_resume() noexcept {}
        };

        auto await_transform(actor_context::yield_t) {
            check_context_initialized();

            return yield_context_awaiter_t{};
        }

        struct preempt_context_awaiter_t {
            bool await_ready() noexcept { return false; }

            __attribute__((__noinline__))
            std::coroutine_handle<> await_suspend(actor_continuation<T> c) noexcept {
                auto& self = c.promise();
                if (!self.context) {
                    return c;
                }
                self.context.scheduler().schedule(c);
                return std::noop_coroutine();
            }

            void await_resume() noexcept {}
        };

        auto await_transform(actor_context::preempt_t) {
            check_context_initialized();

            return preempt_context_awaiter_t{};
        }

        struct restore_context_callback {
            const actor_context& context;
            std::coroutine_handle<> c;

            restore_context_callback(const actor_context& context, std::coroutine_handle<> c) noexcept
                : context(context)
                , c(c)
            {}

            restore_context_callback(const restore_context_callback&) = delete;
            restore_context_callback& operator=(const restore_context_callback&) = delete;

            restore_context_callback(restore_context_callback&& rhs) noexcept
                : context(rhs.context)
                , c(std::exchange(rhs.c, {}))
            {}

            ~restore_context_callback() noexcept {
                if (c) {
                    // Callback was not invoked, but we are supposed to resume
                    // our continuation. All we can do now is destroy it and
                    // hope it unwinds its stack correctly.
                    c.destroy();
                }
            }

            std::coroutine_handle<> operator()() noexcept {
                if (auto next = context.push(std::exchange(c, {}))) {
                    // Note: caller may have called resume recursively, from
                    // a different actor even, and won't be expecting us to
                    // keep going indefinitely. So we always reschedule.
                    context.scheduler().schedule(next);
                }
                return std::noop_coroutine();
            }
        };

        static std::coroutine_handle<> wrap_restore_context(const actor_context& context, std::coroutine_handle<> c) {
            if (!context) {
                return c;
            }

            // Generate a wrapped continuation that will restore context on resume
            return with_resume_callback(restore_context_callback{ context, c });
        }

        std::coroutine_handle<> wrap_restore_context(std::coroutine_handle<> c) {
            return wrap_restore_context(context, c);
        }

        static void release_context(const actor_context& context) {
            if (context) {
                if (auto next = context.pop()) {
                    context.scheduler().schedule(next);
                }
            }
        }

        static std::coroutine_handle<> next_from_context(const actor_context& context) {
            if (context) {
                if (auto next = context.pop()) {
                    auto& scheduler = context.scheduler();
                    if (!scheduler.preempt()) {
                        return next;
                    }
                    scheduler.schedule(next);
                }
            }
            return std::noop_coroutine();
        }

        template<awaitable Awaitable>
        class same_context_wrapped_awaiter {
            // Note: Awaiter may be a reference type
            using Awaiter = decltype(get_awaiter(std::declval<Awaitable&&>()));
            // Note: Result may be a reference type
            using Result = decltype(std::declval<Awaiter&>().await_resume());

        public:
            // Note: if operator co_await returns a value we will construct it
            // in place without moves. If it returns a reference we will bind
            // to that. This should be safe because it's all part of the same
            // co_await expression and we have the same lifetime as the
            // awaitable.
            same_context_wrapped_awaiter(Awaitable&& awaitable)
                : awaiter(get_awaiter(std::forward<Awaitable>(awaitable)))
            {}

            same_context_wrapped_awaiter(const same_context_wrapped_awaiter&) = delete;
            same_context_wrapped_awaiter& operator=(const same_context_wrapped_awaiter&) = delete;

            bool await_ready()
                noexcept(has_noexcept_await_ready<Awaiter>)
            {
                return awaiter.await_ready();
            }

            __attribute__((__noinline__))
            std::coroutine_handle<> await_suspend(actor_continuation<T> c)
                noexcept(has_noexcept_await_suspend<Awaiter>)
            {
                auto& self = c.promise();
                std::coroutine_handle<> k = wrap_restore_context(self.context, c);
                // Note: we still have context locked, but after the call to
                // Awaiter's await_suspend our frame may be destroyed and we
                // cannot access self or any members. Make a copy of context.
                auto context = self.context;
                if constexpr (has_await_suspend_void<Awaiter>) {
                    // Awaiter always suspends
                    awaiter.await_suspend(std::move(k));
                } else if constexpr (has_await_suspend_bool<Awaiter>) {
                    if (!awaiter.await_suspend(std::move(k))) {
                        // Awaiter did not suspend, release context and resume
                        release_context(context);
                        return k;
                    }
                } else {
                    static_assert(has_await_suspend_handle<Awaiter>);
                    k = awaiter.await_suspend(std::move(k));
                    if (k != std::noop_coroutine()) {
                        // Awaiter is asking us to resume some coroutine, but
                        // we cannot know if it is the same coroutine, even
                        // if it has the same address (it may have been
                        // destroyed and reallocated again)
                        release_context(context);
                        return k;
                    }
                }
                // Awaiter suspended without a continuation
                // Take the next continuation from our context
                return next_from_context(context);
            }

            Result await_resume()
                noexcept(has_noexcept_await_resume<Awaiter>)
            {
                return awaiter.await_resume();
            }

        private:
            Awaiter awaiter;
        };

        // Note: it's awaitable and not awaitable<actor_promise<T>>
        //       we always supply wrapped awaiter with a type erased handle
        template<awaitable Awaitable>
        auto await_transform(Awaitable&& awaitable)
            requires (!actor_passthru_awaitable<Awaitable, T>)
        {
            check_context_initialized();

            return same_context_wrapped_awaiter<Awaitable>(std::forward<Awaitable>(awaitable));
        }

        template<awaitable Awaitable>
        class change_context_wrapped_awaiter {
            // Note: Awaiter may be a reference type
            using Awaiter = decltype(get_awaiter(std::declval<Awaitable&&>()));
            // Note: Result may be a reference type
            using Result = decltype(std::declval<Awaiter&>().await_resume());

        public:
            // Note: if operator co_await returns a value we will construct it
            // in place without moves. If it returns a reference we will bind
            // to that. This should be safe because it's all part of the same
            // co_await expression and we have the same lifetime as the
            // awaitable.
            change_context_wrapped_awaiter(Awaitable&& awaitable, const actor_context& new_context, bool same_context)
                : awaiter(get_awaiter(std::forward<Awaitable>(awaitable)))
                , new_context(new_context)
                , same_context(same_context)
            {}

            change_context_wrapped_awaiter(const change_context_wrapped_awaiter&) = delete;
            change_context_wrapped_awaiter& operator=(const change_context_wrapped_awaiter&) = delete;

            bool await_ready()
                noexcept(has_noexcept_await_ready<Awaiter>)
            {
                return (ready = awaiter.await_ready()) && same_context;
            }

            __attribute__((__noinline__))
            std::coroutine_handle<> await_suspend(actor_continuation<T> c)
                noexcept(has_noexcept_await_suspend<Awaiter>)
            {
                auto& self = c.promise();
                std::coroutine_handle<> k = wrap_restore_context(new_context, c);
                if (ready) {
                    // Awaiter's await_ready returned true, we just need to change context
                    assert(self.context != new_context);
                    release_context(self.context);
                    self.context = new_context;
                    return k;
                }
                // Note: we still have context locked, but after the call to
                // Awaiter's await_suspend our frame may be destroyed and we
                // cannot access self or any members. Make a copy of context.
                auto context = self.context;
                // Change promise context to new context
                self.context = new_context;
                if constexpr (has_await_suspend_void<Awaiter>) {
                    // Awaiter always suspends
                    awaiter.await_suspend(std::move(k));
                } else if constexpr (has_await_suspend_bool<Awaiter>) {
                    if (!awaiter.await_suspend(std::move(k))) {
                        // Awaiter did not suspend, release context and resume
                        release_context(context);
                        return k;
                    }
                } else {
                    static_assert(has_await_suspend_handle<Awaiter>);
                    k = awaiter.await_suspend(std::move(k));
                    if (k != std::noop_coroutine()) {
                        // Awaiter is asking us to resume some coroutine, but
                        // we cannot know if it is the same coroutine, even
                        // if it has the same address (it may have been
                        // destroyed and reallocated again)
                        release_context(context);
                        return k;
                    }
                }
                // Awaiter suspended without a continuation
                // Take the next continuation from our context
                return next_from_context(context);
            }

            Result await_resume()
                noexcept(has_noexcept_await_resume<Awaiter>)
            {
                return awaiter.await_resume();
            }

        private:
            Awaiter awaiter;
            const actor_context& new_context;
            const bool same_context;
            bool ready = false;
        };

        template<awaitable Awaitable>
        auto await_transform(actor_context::bind_awaitable_t<Awaitable> bound) {
            check_context_initialized();

            return change_context_wrapped_awaiter<Awaitable>(
                std::forward<Awaitable>(bound.awaitable),
                bound.context,
                bound.context == context);
        }

        template<awaitable Awaitable>
        auto await_transform(actor_context::caller_context_t::bind_awaitable_t<Awaitable> bound) {
            check_context_initialized();

            return change_context_wrapped_awaiter<Awaitable>(
                std::forward<Awaitable>(bound.awaitable),
                continuation_context,
                continuation_context == context);
        }

        // Awaitables marked with is_actor_passthru_awaitable claim to support
        // context switches directly, and those are not transformed.
        template<actor_passthru_awaitable<T> Awaitable>
        Awaitable&& await_transform(Awaitable&& awaitable) {
            check_context_initialized();

            // This awaitable is marked to support context switches directly
            return (Awaitable&&) awaitable;
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
        bool context_initialized = false;
        bool context_inherited = false;
        bool finished = false;
    };

    template<class T>
    class [[nodiscard]] actor_awaiter {
    public:
        using is_actor_passthru_awaitable = void;

        explicit actor_awaiter(actor_continuation<T> h) noexcept
            : handle(h)
        {}

        actor_awaiter(const actor_awaiter&) = delete;
        actor_awaiter& operator=(const actor_awaiter&) = delete;

        actor_awaiter(actor_awaiter&& rhs)
            : handle(std::exchange(rhs.handle, {}))
        {}

        ~actor_awaiter() noexcept {
            if (handle) {
                if (suspended) {
                    if (handle.promise().unset_continuation()) {
                        // Continue bottom-up frame cleanup
                        handle.destroy();
                    }
                } else {
                    handle.destroy();
                }
            }
        }

        bool await_ready() noexcept {
            return handle.promise().ready();
        }

        template<class Promise>
        __attribute__((__noinline__))
        std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> c) noexcept {
            auto& p = handle.promise();
            p.set_continuation(c);
            suspended = true;
            return p.start();
        }

        std::add_rvalue_reference_t<T> await_resume() {
            suspended = false;
            return handle.promise().take_result().take();
        }

    private:
        actor_continuation<T> handle;
        bool suspended = false;
    };

    template<class T>
    class [[nodiscard]] actor_result_awaiter {
    public:
        using is_actor_passthru_awaitable = void;

        explicit actor_result_awaiter(actor_continuation<T> h) noexcept
            : handle(h)
        {}

        actor_result_awaiter(const actor_result_awaiter&) = delete;
        actor_result_awaiter& operator=(const actor_result_awaiter&) = delete;

        actor_result_awaiter(actor_result_awaiter&& rhs)
            : handle(std::exchange(rhs.handle, {}))
        {}

        ~actor_result_awaiter() noexcept {
            if (handle) {
                if (suspended) {
                    if (handle.promise().unset_continuation()) {
                        // Continue bottom-up frame cleanup
                        handle.destroy();
                    }
                } else {
                    handle.destroy();
                }
            }
        }

        bool await_ready() noexcept {
            return handle.promise().ready();
        }

        template<class Promise>
        __attribute__((__noinline__))
        std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> c) noexcept {
            auto& p = handle.promise();
            p.set_continuation(c);
            suspended = true;
            return p.start();
        }

        result<T>&& await_resume() {
            suspended = false;
            return handle.promise().take_result();
        }

    private:
        actor_continuation<T> handle;
        bool suspended = false;
    };

} // namespace coroactors::detail
