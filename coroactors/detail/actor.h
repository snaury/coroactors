#pragma once
#include <coroactors/actor_context.h>
#include <coroactors/actor_error.h>
#include <coroactors/detail/awaiters.h>
#include <coroactors/result.h>
#include <coroactors/stop_token.h>
#include <coroactors/with_resume_callback.h>
#include <cassert>
#include <stdexcept>
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
    class actor_awaiter;

    template<class T>
    class actor_result_awaiter;

    template<class T>
    using actor_continuation = std::coroutine_handle<actor_promise<T>>;

    template<class Awaitable, class T>
    concept actor_passthru_awaitable =
        awaitable<Awaitable, actor_promise<T>> &&
        requires {
            typename awaitable_unwrap_awaiter_type<Awaitable, actor_promise<T>>::is_actor_passthru_awaiter;
        };

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

    /**
     * Runs the specified callback on exceptions
     */
    template<class Callback>
    class exceptions_guard {
    public:
        template<class... Args>
        explicit exceptions_guard(Args&&... args)
            : callback(std::forward<Args>(args)...)
        {}

        exceptions_guard(const exceptions_guard&) = delete;
        exceptions_guard& operator=(const exceptions_guard&) = delete;

        ~exceptions_guard() {
            if (count != std::uncaught_exceptions()) [[unlikely]] {
                callback();
            }
        }

    private:
        Callback callback;
        int count = std::uncaught_exceptions();
    };

    template<class Callback>
    exceptions_guard(Callback&&) -> exceptions_guard<std::decay_t<Callback>>;

    /**
     * A callback type that restores actor context before resuming a coroutine
     */
    class actor_restore_context_callback {
    public:
        actor_restore_context_callback(const actor_context& context,
                std::coroutine_handle<> c, std::coroutine_handle<>& wrapped) noexcept
            : context(context)
            , c(c)
            , wrapped(wrapped)
        {}

        actor_restore_context_callback(const actor_restore_context_callback&) = delete;
        actor_restore_context_callback& operator=(const actor_restore_context_callback&) = delete;

        actor_restore_context_callback(actor_restore_context_callback&& rhs) noexcept
            : context(rhs.context)
            , c(std::exchange(rhs.c, {}))
            , wrapped(rhs.wrapped)
        {}

        ~actor_restore_context_callback() noexcept {
            // Note: must short circuit on `c` here. The `wrapped`
            // reference is likely pointing to an already destroyed value
            // after the coroutine is resumed.
            if (c && wrapped) {
                // Callback was not invoked, but we are supposed to resume
                // our continuation. All we can do now is destroy it and
                // hope it unwinds its stack correctly.
                wrapped = {};
                c.destroy();
            }
        }

        std::coroutine_handle<> operator()() noexcept {
            wrapped = {};
            return context.manager().restore(std::exchange(c, {}));
        }

        /**
         * Wraps a given actor coroutine handle `c` to restore the given actor
         * context before resuming. The handle also propagates destroy calls
         * unless the `wrapped` variable is unset at that time.
         */
        static std::coroutine_handle<> wrap(const actor_context& context,
                std::coroutine_handle<> c, std::coroutine_handle<>& wrapped) noexcept
        {
            return with_resume_callback(actor_restore_context_callback{ context, c, wrapped });
        }

    private:
        const actor_context& context;
        std::coroutine_handle<> c;
        std::coroutine_handle<>& wrapped;
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
            static void await_resume() noexcept { /* never called */ }

            __attribute__((__noinline__))
            static std::coroutine_handle<> await_suspend(actor_continuation<T> h) noexcept {
                auto& p = h.promise();
                p.finished = true;

                // Check if we have been co_await'ed
                auto next = std::exchange(p.continuation, {});
                if (!next) {
                    if (!p.context_initialized) {
                        // We did not reach a co_await context point
                        // Suspend until actor is co_awaited
                        return std::noop_coroutine();
                    }

                    // Actor finished after a call to detach, see if there's
                    // some other continuation available in the same context,
                    // this would also correctly leave the context.
                    next = p.context.manager().next();

                    // We have to destroy ourselves, since nobody is waiting
                    h.destroy();

                    return next;
                }

                if (p.continuation_context) {
                    return p.continuation_context->manager().switch_from(
                        p.context.manager(), next, /* returning */ true);
                }

                // We are returning to a non-actor coroutine
                p.context.manager().leave();
                return next;
            }
        };

        static auto final_suspend() noexcept { return final_suspend_t{}; }

        bool ready() const {
            return finished;
        }

        void set_stop_token(const stop_token& t) noexcept {
            token = t;
        }

        const stop_token& get_stop_token() const noexcept {
            return token;
        }

        const actor_context& get_context() const {
            return context;
        }

        const actor_context* get_context_ptr() const {
            return &context;
        }

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
            continuation_context = c.promise().get_context_ptr();
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

        std::coroutine_handle<> start_await() noexcept {
            std::coroutine_handle<> h = actor_continuation<T>::from_promise(*this);

            if (!continuation_context) {
                // We are awaited by a non-actor coroutine, enter the context
                return context.manager().enter_for_await(h);
            }

            // Transfer directly if awaiter context is the same
            if (context == *continuation_context) {
                // We are awaited by an actor coroutine with the same context
                return h;
            }

            // Switch to our context
            return context.manager().switch_from(
                continuation_context->manager(), h, /* returning */ false);
        }

        void start_detached(bool use_scheduler = true) noexcept {
            std::coroutine_handle<> h = actor_continuation<T>::from_promise(*this);

            // Destroy an already finished coroutine
            if (finished) {
                h.destroy();
                return;
            }

            // Resume coroutine recursively when allowed
            if (auto c = context.manager().enter_for_resume(h, use_scheduler)) {
                context.manager().resume(c);
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
                // The first time we suspend we return to caller
                if (type == ESwitchContext::Initial) {
                    return std::noop_coroutine();
                }

                // We are changing to a different context
                assert(type != ESwitchContext::Ready);
                auto& self = c.promise();
                auto from = std::move(self.context);
                self.context = to;
                // Note: we set "returning" to true here to match a possible
                // change in behavior when returning from actors without a
                // context. On the one hand user could put code in a separate
                // function, and the starting wait would be elided, but with
                // this flag set it would have to defer when switching from
                // empty to non-empty context. However the much more important
                // difference is when this actor returns. With a separate
                // function it would first switch to empty context and then
                // returning to non-empty context would defer, but when we
                // allow context switching without defer here, it would
                // potentially run for a long time inside some unknown handler
                // that initially resumed the context-less actor.
                return self.context.manager().switch_from(
                    from.manager(), c, /* returning */ true);
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
                // Switching to an empty context, avoid suspending
                auto from = std::move(context);
                context = bound.context;
                context.manager().switch_from(
                    from.manager(), nullptr, /* returning */ false);
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

            // Reuse await_transform above for binding to a continuation context
            return await_transform(actor_context::bind_context_t{
                continuation_context ? *continuation_context : no_actor_context
            });
        }

        void check_context_initialized() const {
            if (!context_initialized) [[unlikely]] {
                throw actor_error("actor must co_await context first");
            }
        }

        struct return_context_awaiter_t {
            const actor_context& context;

            bool await_ready() noexcept { return true; }
            bool await_suspend(std::coroutine_handle<>) noexcept { return false; }
            [[nodiscard]] const actor_context& await_resume() noexcept { return context; }
        };

        auto await_transform(actor_context::caller_context_t) {
            check_context_initialized();

            return return_context_awaiter_t{ continuation_context ? *continuation_context : no_actor_context };
        }

        auto await_transform(actor_context::current_context_t) {
            check_context_initialized();

            return return_context_awaiter_t{ context };
        }

        struct yield_context_awaiter_t {
            bool await_ready() noexcept { return false; }

            __attribute__((__noinline__))
            std::coroutine_handle<> await_suspend(actor_continuation<T> c) noexcept {
                auto& self = c.promise();
                return self.context.manager().yield(c);
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
                return self.context.manager().preempt(c);
            }

            void await_resume() noexcept {}
        };

        auto await_transform(actor_context::preempt_t) {
            check_context_initialized();

            return preempt_context_awaiter_t{};
        }

        struct current_stop_token_awaiter_t {
            const stop_token& token;

            bool await_ready() noexcept { return true; }
            bool await_suspend(actor_continuation<T>) noexcept { return false; }
            [[nodiscard]] const stop_token& await_resume() noexcept { return token; }
        };

        auto await_transform(actor_context::current_stop_token_t) {
            check_context_initialized();

            return current_stop_token_awaiter_t{ token };
        }

        template<awaitable Awaitable>
        class same_context_wrapped_awaiter {
            // Note: Awaiter may be a reference type
            using Awaiter = awaiter_type_t<Awaitable>;
            // Note: Result may be a reference type
            using Result = awaiter_result_t<Awaiter>;

        public:
            // Note: if operator co_await returns a value we will construct it
            // in place without moves. If it returns a reference we will bind
            // to that. This should be safe because it's all part of the same
            // co_await expression and we have the same lifetime as the
            // awaitable.
            same_context_wrapped_awaiter(Awaitable&& awaitable, actor_promise& self)
                : awaiter(get_awaiter(std::forward<Awaitable>(awaitable)))
                , self(self)
            {}

            same_context_wrapped_awaiter(const same_context_wrapped_awaiter&) = delete;
            same_context_wrapped_awaiter& operator=(const same_context_wrapped_awaiter&) = delete;

            ~same_context_wrapped_awaiter() noexcept {
                // Note: there are fours ways this awaiter is destroyed:
                // - it never suspended, then wrapped will be empty
                // - it was resumed normally, and it unsets wrapped variable
                //   before resuming us, so we will not cause double free
                // - it was destroyed, and it unsets wrapped variable before
                //   destroying us, so we will not cause double free
                // - our parent frame was destroyed (bottom-up), or we resumed
                //   directly without a wrapper, we have to destroy it to avoid
                //   a memory leak.
                if (wrapped) {
                    std::exchange(wrapped, {}).destroy();
                }
            }

            bool await_ready()
                noexcept(has_await_ready_stop_token<Awaiter>
                    ? has_noexcept_await_ready_stop_token<Awaiter>
                    : has_noexcept_await_ready<Awaiter>)
            {
                if constexpr (has_await_ready_stop_token<Awaiter>) {
                    return awaiter.await_ready(self.get_stop_token());
                } else {
                    return awaiter.await_ready();
                }
            }

            __attribute__((__noinline__))
            std::coroutine_handle<> await_suspend(actor_continuation<T> c)
                // Note: we allocate a wrapper in this method, so not noexcept
                // noexcept(has_noexcept_await_suspend<Awaiter>)
            {
                // The wrapped handle restores context before resuming
                std::coroutine_handle<> k = wrapped =
                    actor_restore_context_callback::wrap(self.context, c, wrapped);

                // This will make sure wrapper does not leak on exceptions
                exceptions_guard guard([this]{
                    if (wrapped) {
                        std::exchange(wrapped, {}).destroy();
                    }
                });

                // Note: we still have context locked, but after the call to
                // Awaiter's await_suspend our frame may be destroyed and we
                // cannot access self or any members. Make a copy of context.
                auto context = self.context;
                if constexpr (has_await_suspend_void<Awaiter>) {
                    // Awaiter always suspends
                    awaiter.await_suspend(std::move(k));
                } else if constexpr (has_await_suspend_bool<Awaiter>) {
                    if (!awaiter.await_suspend(std::move(k))) {
                        // Awaiter did not suspend, unwrap and resume
                        std::exchange(wrapped, {}).destroy();
                        return c;
                    }
                } else {
                    static_assert(has_await_suspend_handle<Awaiter>);
                    k = awaiter.await_suspend(std::move(k));
                    if (k != std::noop_coroutine()) {
                        // Awaiter is asking us to resume some coroutine, but
                        // we cannot know if it is the same coroutine, even
                        // if it has the same address (it may have been
                        // destroyed and reallocated again).
                        context.manager().leave();
                        return k;
                    }
                }
                // Awaiter suspended without a continuation
                // Take the next continuation from our context
                return context.manager().next();
            }

            Result await_resume()
                noexcept(has_noexcept_await_resume<Awaiter>)
            {
                return awaiter.await_resume();
            }

        private:
            Awaiter awaiter;
            actor_promise& self;
            std::coroutine_handle<> wrapped;
        };

        // Note: it's awaitable and not awaitable<actor_promise<T>>
        //       we always supply wrapped awaiter with a type erased handle
        template<awaitable Awaitable>
        auto await_transform(Awaitable&& awaitable)
            requires (!actor_passthru_awaitable<Awaitable, T>)
        {
            // Protect against metaprogramming mistakes
            static_assert(!std::is_same_v<std::decay_t<Awaitable>, actor<T>>);
            static_assert(!std::is_same_v<std::decay_t<Awaitable>, actor_awaiter<T>>);
            static_assert(!std::is_same_v<std::decay_t<Awaitable>, actor_result_awaiter<T>>);

            check_context_initialized();

            return same_context_wrapped_awaiter<Awaitable>(std::forward<Awaitable>(awaitable), *this);
        }

        template<awaitable Awaitable>
        class change_context_wrapped_awaiter {
            // Note: Awaiter may be a reference type
            using Awaiter = awaiter_type_t<Awaitable>;
            // Note: Result may be a reference type
            using Result = awaiter_result_t<Awaiter>;

        public:
            // Note: if operator co_await returns a value we will construct it
            // in place without moves. If it returns a reference we will bind
            // to that. This should be safe because it's all part of the same
            // co_await expression and we have the same lifetime as the
            // awaitable.
            change_context_wrapped_awaiter(Awaitable&& awaitable, const actor_context& new_context, actor_promise& self)
                : awaiter(get_awaiter(std::forward<Awaitable>(awaitable)))
                , new_context(new_context)
                , self(self)
            {}

            change_context_wrapped_awaiter(const change_context_wrapped_awaiter&) = delete;
            change_context_wrapped_awaiter& operator=(const change_context_wrapped_awaiter&) = delete;

            ~change_context_wrapped_awaiter() noexcept {
                // Note: there are fours ways this awaiter is destroyed:
                // - it never suspended, then wrapped will be empty
                // - it was resumed normally, and it unsets wrapped variable
                //   before resuming us, so we will not cause double free
                // - it was destroyed, and it unsets wrapped variable before
                //   destroying us, so we will not cause double free
                // - our parent frame was destroyed (bottom-up), or we resumed
                //   directly without a wrapper, we have to destroy it to avoid
                //   a memory leak.
                if (wrapped) {
                    std::exchange(wrapped, {}).destroy();
                }
            }

            bool await_ready()
                noexcept(has_await_ready_stop_token<Awaiter>
                    ? has_noexcept_await_ready_stop_token<Awaiter>
                    : has_noexcept_await_ready<Awaiter>)
            {
                if constexpr (has_await_ready_stop_token<Awaiter>) {
                    ready = awaiter.await_ready(self.get_stop_token());
                } else {
                    ready = awaiter.await_ready();
                }
                return ready && new_context == self.context;
            }

            __attribute__((__noinline__))
            std::coroutine_handle<> await_suspend(actor_continuation<T> c)
                // Note: we allocate a wrapper in this method, so not noexcept
                // noexcept(has_noexcept_await_suspend<Awaiter>)
            {
                if (ready) {
                    // Awaiter's await_ready returned true, so we perform a
                    // context switch here, as if returning from the awaiter.
                    // This also handles return path for a context-less actor
                    // that wraps a trivial awaitable in a context. It would
                    // behavior as if switching to a new context, see a large
                    // comment in switch_context_awaiter as to why.
                    assert(self.context != new_context);
                    auto context = std::move(self.context);
                    self.context = new_context;
                    return self.context.manager().switch_from(
                        context.manager(), c, /* returning */ true);
                }

                // The wrapped handle restores context before resuming
                std::coroutine_handle<> k = wrapped =
                    actor_restore_context_callback::wrap(new_context, c, wrapped);

                // This will make sure wrapper does not leak on exceptions
                exceptions_guard guard([this]{
                    if (wrapped) {
                        std::exchange(wrapped, {}).destroy();
                    }
                });

                // Note: we still have context locked, but after the call to
                // Awaiter's await_suspend our frame may be destroyed and we
                // cannot access self or any members. Make a copy of context.
                auto context = std::move(self.context);
                // Change promise context to new context
                self.context = new_context;
                if constexpr (has_await_suspend_void<Awaiter>) {
                    // Awaiter always suspends
                    awaiter.await_suspend(std::move(k));
                } else if constexpr (has_await_suspend_bool<Awaiter>) {
                    if (!awaiter.await_suspend(std::move(k))) {
                        // Awaiter did not suspend, unwrap and resume
                        std::exchange(wrapped, {}).destroy();
                        return c;
                    }
                } else {
                    static_assert(has_await_suspend_handle<Awaiter>);
                    k = awaiter.await_suspend(std::move(k));
                    if (k != std::noop_coroutine()) {
                        // Awaiter is asking us to resume some coroutine, but
                        // we cannot know if it is the same coroutine, even
                        // if it has the same address (it may have been
                        // destroyed and reallocated again)
                        context.manager().leave();
                        return k;
                    }
                }
                // Awaiter suspended without a continuation
                // Take the next continuation from our context
                return context.manager().next();
            }

            Result await_resume()
                noexcept(has_noexcept_await_resume<Awaiter>)
            {
                return awaiter.await_resume();
            }

        private:
            Awaiter awaiter;
            const actor_context& new_context;
            actor_promise& self;
            std::coroutine_handle<> wrapped;
            bool ready = false;
        };

        template<awaitable Awaitable>
        auto await_transform(actor_context::bind_awaitable_t<Awaitable> bound) {
            check_context_initialized();

            return change_context_wrapped_awaiter<Awaitable>(
                std::forward<Awaitable>(bound.awaitable),
                bound.context,
                *this);
        }

        template<awaitable Awaitable>
        auto await_transform(actor_context::caller_context_t::bind_awaitable_t<Awaitable> bound) {
            check_context_initialized();

            return change_context_wrapped_awaiter<Awaitable>(
                std::forward<Awaitable>(bound.awaitable),
                continuation_context ? *continuation_context : no_actor_context,
                *this);
        }

        template<class Awaitable>
        class actor_passthru_awaiter {
            using Awaiter = awaiter_type_t<Awaitable>;
            using Result = awaiter_result_t<Awaiter>;

        public:
            actor_passthru_awaiter(Awaitable&& awaitable, actor_promise& self)
                : awaiter(get_awaiter(std::forward<Awaitable>(awaitable)))
                , self(self)
            {}

            bool await_ready()
                noexcept(has_await_ready_stop_token<Awaiter>
                    ? has_noexcept_await_ready_stop_token<Awaiter>
                    : has_noexcept_await_ready<Awaiter>)
            {
                if constexpr (has_await_ready_stop_token<Awaiter>) {
                    return awaiter.await_ready(self.get_stop_token());
                } else {
                    return awaiter.await_ready();
                }
            }

            template<class Promise>
            __attribute__((__noinline__))
            decltype(auto) await_suspend(std::coroutine_handle<Promise> c)
                noexcept(has_noexcept_await_suspend<Awaiter, Promise>)
                requires has_await_suspend<Awaiter, Promise>
            {
                return awaiter.await_suspend(c);
            }

            Result await_resume()
                noexcept(has_noexcept_await_resume<Awaiter>)
            {
                return awaiter.await_resume();
            }

        private:
            Awaiter awaiter;
            actor_promise& self;
        };

        // Awaitables that have awaiters marked with is_actor_passthru_awaiter
        // claim to support context switches directly, this includes wrappers.
        template<actor_passthru_awaitable<T> Awaitable>
        auto await_transform(Awaitable&& awaitable) {
            check_context_initialized();

            return actor_passthru_awaiter<Awaitable>(std::forward<Awaitable>(awaitable), *this);
        }

    private:
        stop_token token;
        actor_context context;
        std::coroutine_handle<> continuation;
        const actor_context* continuation_context{ nullptr };
        bool context_initialized = false;
        bool context_inherited = false;
        bool finished = false;
    };

    template<class T>
    class [[nodiscard]] actor_awaiter {
    public:
        using is_actor_passthru_awaiter = void;

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

        bool await_ready(const stop_token& token) noexcept {
            handle.promise().set_stop_token(token);
            return handle.promise().ready();
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
            return p.start_await();
        }

        T await_resume() {
            suspended = false;
            return handle.promise().take_result().take_value();
        }

    private:
        actor_continuation<T> handle;
        bool suspended = false;
    };

    template<class T>
    class [[nodiscard]] actor_result_awaiter {
    public:
        using is_actor_passthru_awaiter = void;

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

        bool await_ready(const stop_token& token) noexcept {
            handle.promise().set_stop_token(token);
            return handle.promise().ready();
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
            return p.start_await();
        }

        result<T> await_resume() {
            suspended = false;
            return handle.promise().take_result();
        }

    private:
        actor_continuation<T> handle;
        bool suspended = false;
    };

} // namespace coroactors::detail
