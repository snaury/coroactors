#pragma once
#include <coroactors/actor_context.h>
#include <coroactors/actor_error.h>
#include <coroactors/detail/actor_context_frame.h>
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
            if (!cancelled && count != std::uncaught_exceptions()) [[unlikely]] {
                callback();
            }
        }

        void cancel() {
            cancelled = true;
        }

    private:
        Callback callback;
        int count = std::uncaught_exceptions();
        bool cancelled = false;
    };

    template<class Callback>
    exceptions_guard(Callback&&) -> exceptions_guard<std::decay_t<Callback>>;

    /**
     * A callback type that restores the frame context
     */
    class actor_restore_context_callback {
    public:
        actor_restore_context_callback(actor_context_frame* frame) noexcept
            : frame(frame)
        {}

        actor_restore_context_callback(const actor_restore_context_callback&) = delete;
        actor_restore_context_callback& operator=(const actor_restore_context_callback&) = delete;

        actor_restore_context_callback(actor_restore_context_callback&& rhs) noexcept
            : frame(rhs.frame)
        {}

        std::coroutine_handle<> operator()() noexcept {
            return frame->context.manager().restore(std::exchange(frame, nullptr));
        }

        /**
         * Wraps a given frame, which is resumed in its current context
         */
        static std::coroutine_handle<> wrap(actor_context_frame* frame) noexcept {
            return with_resume_callback(actor_restore_context_callback{ frame });
        }

    private:
        actor_context_frame* frame;
    };

    template<class T>
    class actor_promise final
        : public actor_context_frame
        , public actor_result_handler<T>
    {
    public:
        actor_promise() noexcept
            : actor_context_frame(actor_continuation<T>::from_promise(*this))
        {}

        actor<T> get_return_object() noexcept {
            return actor<T>(actor_continuation<T>::from_promise(*this));
        }

        void unhandled_exception() noexcept {
            this->result_.set_exception(std::current_exception());
        }

        static auto initial_suspend() noexcept { return std::suspend_always{}; }

        struct final_suspend_t {
            static bool await_ready() noexcept { return false; }
            static void await_resume() noexcept { /* never called */ }

            __attribute__((__noinline__))
            static std::coroutine_handle<> await_suspend(actor_continuation<T> h) noexcept {
                auto& p = h.promise();

                // Check if we have been co_await'ed
                auto next = std::exchange(p.continuation, {});
                if (!next) {
                    // Actor finished after a call to detach, see if there's
                    // some other continuation available in the same context,
                    // this will also correctly leave the frame.
                    next = p.context.manager().finish(&p);

                    // We have to destroy ourselves, since nobody is waiting
                    h.destroy();

                    return next;
                }

                if (p.continuation_frame) {
                    // We are returning to an actor, handle context switches
                    return p.continuation_frame->context.manager().switch_frame(
                        /* from */ &p, /* to */ p.continuation_frame, /* returning */ true);
                } else {
                    // We are returning to a non-actor coroutine
                    p.context.manager().leave(&p);
                    return next;
                }
            }
        };

        static auto final_suspend() noexcept { return final_suspend_t{}; }

        const actor_context& get_context() const {
            return context;
        }

        const actor_context* get_context_ptr() const {
            return &context;
        }

        template<class U>
        void set_continuation(actor_continuation<U> c) noexcept {
            continuation = c;
            continuation_frame = &c.promise();
            context = continuation_frame->context;
        }

        void set_continuation(std::coroutine_handle<> c) noexcept {
            continuation = c;
        }

        std::coroutine_handle<> start_await() noexcept {
            if (!continuation_frame) {
                // We are awaited by a non-actor coroutine, enter the context
                return context.manager().enter(this);
            }

            return context.manager().switch_frame(
                continuation_frame, this, /* returning */ false);
        }

        void start_detached() noexcept {
            context.manager().start(this);
        }

        enum class ESwitchContext {
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
                // We are changing to a different context
                assert(type != ESwitchContext::Ready);
                auto& self = c.promise();
                // Note: we are returning from a co_await here
                return to.manager().switch_context(&self, /* returning */ true);
            }

            void await_resume() noexcept {
                // nothing
            }
        };

        switch_context_awaiter await_transform(actor_context::bind_context_t bound) {
            if (context == bound.context) {
                // We are not changing contexts, no op
                return switch_context_awaiter{ bound.context, ESwitchContext::Ready };
            }

            if (!bound.context) {
                // Switching to an empty context, avoid suspending
                context.manager().switch_to_empty(this);
                return switch_context_awaiter{ bound.context, ESwitchContext::Ready };
            }

            // We need to suspend and resume in the new context
            return switch_context_awaiter{ bound.context, ESwitchContext::Switch };
        }

        auto await_transform(actor_context::caller_context_t::bind_context_t) {
            // Reuse await_transform above for binding to a continuation context
            return await_transform(actor_context::bind_context_t{
                continuation_frame ? continuation_frame->context : no_actor_context
            });
        }

        struct return_context_awaiter_t {
            const actor_context& context;

            bool await_ready() noexcept { return true; }
            bool await_suspend(std::coroutine_handle<>) noexcept { return false; }
            [[nodiscard]] const actor_context& await_resume() noexcept { return context; }
        };

        auto await_transform(actor_context::caller_context_t) {
            return return_context_awaiter_t{ continuation_frame ? continuation_frame->context : no_actor_context };
        }

        auto await_transform(actor_context::current_context_t) {
            return return_context_awaiter_t{ context };
        }

        struct yield_context_awaiter_t {
            bool await_ready() noexcept { return false; }

            __attribute__((__noinline__))
            std::coroutine_handle<> await_suspend(actor_continuation<T> c) noexcept {
                auto& self = c.promise();
                return self.context.manager().yield(&self);
            }

            void await_resume() noexcept {}
        };

        auto await_transform(actor_context::yield_t) {
            return yield_context_awaiter_t{};
        }

        struct preempt_context_awaiter_t {
            bool await_ready() noexcept { return false; }

            __attribute__((__noinline__))
            std::coroutine_handle<> await_suspend(actor_continuation<T> c) noexcept {
                auto& self = c.promise();
                return self.context.manager().yield(&self);
            }

            void await_resume() noexcept {}
        };

        auto await_transform(actor_context::preempt_t) {
            return preempt_context_awaiter_t{};
        }

        struct current_stop_token_awaiter_t {
            const stop_token& token;

            bool await_ready() noexcept { return true; }
            bool await_suspend(actor_continuation<T>) noexcept { return false; }
            [[nodiscard]] const stop_token& await_resume() noexcept { return token; }
        };

        auto await_transform(actor_context::current_stop_token_t) {
            return current_stop_token_awaiter_t{ get_stop_token() };
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
                if (wrapped) {
                    wrapped.destroy();
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
                // The wrapped handle restores context when resumed
                std::coroutine_handle<> k = wrapped =
                    actor_restore_context_callback::wrap(&self);

                // Prepare to resume in another thread
                self.context.manager().async_start(&self);

                // Note: we still have context locked, but after the call to
                // awaiter's await_suspend our frame may be destroyed and we
                // cannot access self or any members. Make a copy of frame's
                // context.
                auto context = self.context;

                // Aborts async_start on exceptions
                exceptions_guard guard([this]{
                    self.context.manager().async_abort(&self);
                });

                if constexpr (has_await_suspend_void<Awaiter>) {
                    // Awaiter always suspends
                    awaiter.await_suspend(std::move(k));
                } else if constexpr (has_await_suspend_bool<Awaiter>) {
                    if (!awaiter.await_suspend(std::move(k))) {
                        // Awaiter did not suspend, abort and resume
                        guard.cancel();
                        self.context.manager().async_abort(&self);
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
                        context.manager().async_leave();
                        return k;
                    }
                }

                // Awaiter suspended without a continuation
                // Take the next continuation from our context
                return context.manager().async_next();
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
                if (wrapped) {
                    wrapped.destroy();
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
                    assert(self.context != new_context);
                    return new_context.manager().switch_context(
                        &self, /* returning */ true);
                }

                // The wrapped handle restores context when resumed
                std::coroutine_handle<> k = wrapped =
                    actor_restore_context_callback::wrap(&self);

                // Prepare to resume in another thread
                self.context.manager().async_start(&self);

                // Note: we still have context locked, but after the call to
                // awaiter's await_suspend our frame may be destroyed and we
                // cannot access self or any members. Make a copy of frame's
                // context and switch to a new context.
                auto context = std::move(self.context);
                self.context = new_context;

                // Aborts async_start and context change on exceptions
                exceptions_guard guard([this, &context]{
                    self.context = std::move(context);
                    self.context.manager().async_abort(&self);
                });

                if constexpr (has_await_suspend_void<Awaiter>) {
                    // Awaiter always suspends
                    awaiter.await_suspend(std::move(k));
                } else if constexpr (has_await_suspend_bool<Awaiter>) {
                    if (!awaiter.await_suspend(std::move(k))) {
                        // Awaiter did not suspend, abort and resume, but also
                        // propely switch to the new context.
                        guard.cancel();
                        self.context = std::move(context);
                        self.context.manager().async_abort(&self);
                        return new_context.manager().switch_context(
                            &self, /* returning */ true);
                    }
                } else {
                    static_assert(has_await_suspend_handle<Awaiter>);
                    k = awaiter.await_suspend(std::move(k));
                    if (k != std::noop_coroutine()) {
                        // Awaiter is asking us to resume some coroutine, but
                        // we cannot know if it is the same coroutine, even
                        // if it has the same address (it may have been
                        // destroyed and reallocated again)
                        context.manager().async_leave();
                        return k;
                    }
                }

                // Awaiter suspended without a continuation
                // Take the next continuation from our context
                return context.manager().async_next();
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
            return change_context_wrapped_awaiter<Awaitable>(
                std::forward<Awaitable>(bound.awaitable),
                bound.context,
                *this);
        }

        template<awaitable Awaitable>
        auto await_transform(actor_context::caller_context_t::bind_awaitable_t<Awaitable> bound) {
            return change_context_wrapped_awaiter<Awaitable>(
                std::forward<Awaitable>(bound.awaitable),
                continuation_frame ? continuation_frame->context : no_actor_context,
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
            return actor_passthru_awaiter<Awaitable>(std::forward<Awaitable>(awaitable), *this);
        }

    private:
        std::coroutine_handle<> continuation;
        actor_context_frame* continuation_frame{ nullptr };
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
                handle.destroy();
            }
        }

        bool await_ready(stop_token token) noexcept {
            handle.promise().set_stop_token(std::move(token));
            return false;
        }

        bool await_ready() noexcept {
            return false;
        }

        template<class Promise>
        __attribute__((__noinline__))
        std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> c) noexcept {
            auto& p = handle.promise();
            p.set_continuation(c);
            return p.start_await();
        }

        T await_resume() {
            return handle.promise().take_result().take_value();
        }

    private:
        actor_continuation<T> handle;
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
                handle.destroy();
            }
        }

        bool await_ready(stop_token token) noexcept {
            handle.promise().set_stop_token(std::move(token));
            return false;
        }

        bool await_ready() noexcept {
            return false;
        }

        template<class Promise>
        __attribute__((__noinline__))
        std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> c) noexcept {
            auto& p = handle.promise();
            p.set_continuation(c);
            return p.start_await();
        }

        result<T> await_resume() {
            return handle.promise().take_result();
        }

    private:
        actor_continuation<T> handle;
    };

} // namespace coroactors::detail
