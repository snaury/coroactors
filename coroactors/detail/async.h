#pragma once
#include <coroactors/detail/async_context.h>
#include <coroactors/detail/async_task.h>
#include <coroactors/detail/awaiters.h>
#include <coroactors/detail/config.h>
#include <coroactors/detail/scope_guard.h>
#include <coroactors/detail/symmetric_transfer.h>
#include <coroactors/detail/tag_invoke.h>
#include <coroactors/result.h>
#include <coroactors/with_resume_callback.h>
#include <cassert>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>

namespace coroactors {

    template<class T>
    class async;

} // namespace coroactors

namespace coroactors::detail {

    template<class T>
    class async_promise;

    template<class T>
    using async_handle = std::coroutine_handle<async_promise<T>>;

    template<class T>
    class async_result_handler_base {
    public:
        result<T>&& take_result() noexcept {
            return std::move(result_);
        }

    protected:
        result<T> result_;
    };

    template<class T>
    class async_result_handler : public async_result_handler_base<T> {
    public:
        template<class Value>
        void return_value(Value&& value)
            requires (std::is_convertible_v<Value&&, T>)
        {
            this->result_.set_value(std::forward<Value>(value));
        }
    };

    template<>
    class async_result_handler<void> : public async_result_handler_base<void> {
    public:
        void return_void() noexcept {
            this->result_.set_value();
        }
    };

    /**
     * A simple runnable that resumes continuation after a context switch
     */
    struct async_context_switch_runnable : public actor_scheduler_runnable {
        async_task* task;
        std::coroutine_handle<> continuation;

        void run() noexcept override {
            assert(task);
            task->enter();
            task->scheduled = true;
            // Note: `this` will be destroyed inside this call
            symmetric::resume(continuation);
        }
    };

    /**
     * A callback that restores task context before resuming a continuation
     */
    class async_restore_task_callback : private actor_scheduler_runnable {
    public:
        /**
         * Wraps a given task and continuation, which is resumed in its current context
         */
        static std::coroutine_handle<> wrap(async_task* task, std::coroutine_handle<> c) noexcept {
            return with_resume_callback(async_restore_task_callback{ task, c });
        }

        async_restore_task_callback(const async_restore_task_callback&) = delete;
        async_restore_task_callback& operator=(const async_restore_task_callback&) = delete;

        async_restore_task_callback(async_restore_task_callback&& rhs) noexcept
            : task(rhs.task)
            , continuation(rhs.continuation)
        {}

        std::coroutine_handle<> operator()() noexcept {
            if (async_context_manager::enter(task->context, *this)) {
                task->enter();
                return continuation;
            } else {
                return std::noop_coroutine();
            }
        }

    private:
        async_restore_task_callback(async_task* task, std::coroutine_handle<> c) noexcept
            : task(task)
            , continuation(c)
        {}

        void run() noexcept override {
            assert(task);
            task->enter();
            task->scheduled = true;
            // Note: `this` will be destroyed inside this call
            symmetric::resume(continuation);
        }

    private:
        async_task* task;
        std::coroutine_handle<> continuation;
    };

    template<class T>
    class async_promise final
        : public async_result_handler<T>
    {
    public:
        async_promise() noexcept = default;

        async<T> get_return_object() noexcept {
            return async<T>(async_handle<T>::from_promise(*this));
        }

        void unhandled_exception() noexcept {
            this->result_.set_exception(std::current_exception());
        }

        static auto initial_suspend() noexcept { return std::suspend_always{}; }

        struct final_suspend_t {
            static bool await_ready() noexcept { return false; }
            static void await_resume() noexcept { /* promise destroyed */ }

            COROACTORS_AWAIT_SUSPEND
            symmetric::result_t await_suspend(async_handle<T> h) noexcept {
                auto& p = h.promise();

                async_task* task = async_task::current;
                assert(task);

                switch (p.state.index()) {
                    case 1: { // inherited task, same context, we just return
                        break;
                    }
                    case 2: { // inherited task, maybe different context
                        actor_context from_context = std::move(task->context);
                        task->context = std::get<2>(std::move(p.state));
                        // Prepare to possibly resume in another thread
                        task->leave();
                        r.task = task;
                        r.continuation = p.continuation;
                        // Try to change context, when true we resume in the current thread
                        if (!async_context_manager::transfer(from_context, task->context, r, /* returning */ true)) {
                            return symmetric::noop();
                        }
                        // We continue in the same thread, restore current task
                        task->enter();
                        break;
                    }
                    case 3: { // our private task, leave and resume
                        assert(task == &std::get<3>(p.state));
                        task->leave();
                        actor_context from_context = std::move(task->context);
                        async_context_manager::leave(from_context);
                        p.state.template emplace<0>();
                        break;
                    }
                    default: {
                        std::terminate();
                    }
                }

                if (!p.continuation) {
                    // Async function finished after a detach
                    // We have to destroy ourselves, since nobody is waiting
                    h.destroy();
                    return symmetric::noop();
                }

                // We return to the continuation
                return symmetric::transfer(p.continuation);
            }

        private:
            async_context_switch_runnable r;
        };

        static auto final_suspend() noexcept { return final_suspend_t{}; }

        /**
         * Prepare to run in an inherited or possibly a private task
         *
         * Eventually returns to c (nullptr when detached)
         */
        void prepare(std::coroutine_handle<> c) noexcept {
            if (state.index() == 0) {
                state.template emplace<async_task>().enter();
            }
            continuation = c;
        }

        const actor_context& get_caller_context() const noexcept {
            switch (state.index()) {
                case 1: {
                    assert(async_task::current);
                    return async_task::current->context;
                }
                case 2: {
                    return std::get<2>(state);
                }
                case 3: {
                    return no_actor_context;
                }
                default: {
                    std::terminate();
                }
            }
        }

        bool preserve_caller_context() noexcept {
            if (state.index() == 1) {
                assert(async_task::current);
                state.template emplace<actor_context>(async_task::current->context);
                return true;
            } else {
                return false;
            }
        }

        enum class ESwitchContext {
            Ready,
            Enter,
            Return,
        };

        class switch_context_awaiter {
        public:
            switch_context_awaiter(const actor_context& to, ESwitchContext type)
                : to(to)
                , type(type)
            {}

            bool await_ready() noexcept {
                return type == ESwitchContext::Ready;
            }

            COROACTORS_AWAIT_SUSPEND
            symmetric::result_t await_suspend(async_handle<T> c) noexcept {
                async_task* task = async_task::current;
                assert(task);

                assert(type != ESwitchContext::Ready);

                auto context = std::move(task->context);
                task->context = to;

                // Prepare to possibly resume in another thread
                task->leave();
                r.task = task;
                r.continuation = c;

                if (!context) {
                    if (!async_context_manager::enter(task->context, r, !task->scheduled)) {
                        return symmetric::noop();
                    }
                } else {
                    // We are returning from co_await most of the time
                    // But the first co_await context() is special
                    bool returning = type == ESwitchContext::Return;
                    if (!async_context_manager::transfer(context, task->context, r, returning)) {
                        return symmetric::noop();
                    }
                }

                // We continue in the same thread
                task->enter();
                return symmetric::self(c);
            }

            void await_resume() noexcept {
                // nothing
            }

        private:
            const actor_context& to;
            const ESwitchContext type;
            async_context_switch_runnable r;
        };

        auto await_transform(actor_context::bind_context_t bound) noexcept {
            async_task* task = async_task::current;
            assert(task);

            bool initial = preserve_caller_context();

            if (task->context == bound.context) {
                return switch_context_awaiter{ bound.context, ESwitchContext::Ready };
            }

            if (!bound.context) {
                // Switching to an empty context, avoid suspending
                async_context_manager::leave(task->context);
                task->context = bound.context;
                return switch_context_awaiter{ bound.context, ESwitchContext::Ready };
            }

            // We consider the first co_await of a different context to be special
            auto type = initial ? ESwitchContext::Enter : ESwitchContext::Return;

            return switch_context_awaiter{ bound.context, type };
        }

        auto await_transform(actor_context::caller_context_t::bind_context_t) {
            // Reuse await_transform above for binding to a continuation context
            return await_transform(actor_context::bind_context_t{
                get_caller_context()
            });
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
            same_context_wrapped_awaiter(Awaitable&& awaitable)
                : awaiter(get_awaiter(std::forward<Awaitable>(awaitable)))
            {}

            same_context_wrapped_awaiter(const same_context_wrapped_awaiter&) = delete;
            same_context_wrapped_awaiter& operator=(const same_context_wrapped_awaiter&) = delete;

            ~same_context_wrapped_awaiter() noexcept {
                if (wrapped) {
                    wrapped.destroy();
                }
            }

            bool await_ready()
                noexcept(has_noexcept_await_ready<Awaiter>)
            {
                return awaiter.await_ready();
            }

            // Note: we allocate a wrapper in this method, so not noexcept
            COROACTORS_AWAIT_SUSPEND
            symmetric::result_t await_suspend(async_handle<T> c) {
                async_task* task = async_task::current;
                assert(task);

                // The wrapped handle restores context when resumed
                std::coroutine_handle<> k = wrapped =
                    async_restore_task_callback::wrap(task, c);

                // Prepare to resume in another thread
                task->leave();

                // Note: we still have context locked, but after the call to
                // awaiter's await_suspend our frame and task may be destroyed
                // and we cannot access any members. Make a copy of current
                // context.
                auto context = task->context;

                // Restores task on exceptions
                scope_guard guard([task]{
                    task->enter();
                });

                if constexpr (has_await_suspend_void<Awaiter>) {
                    // Awaiter always suspends
                    awaiter.await_suspend(std::move(k));
                    guard.cancel();
                } else {
                    auto next = symmetric::intercept(
                        awaiter.await_suspend(std::move(k)));
                    guard.cancel();

                    if (!next) {
                        // Awaiter did not suspend, abort and resume
                        task->enter();
                        return symmetric::self(c);
                    }

                    if (next != std::noop_coroutine()) {
                        // Awaiter is transferring to some valid coroutine
                        // handle. Note: this may be a different coroutine,
                        // even when its address is the same. Treat the
                        // original as destroyed.
                        async_context_manager::leave(context);
                        return symmetric::transfer(next);
                    }
                }

                // Awaiter suspended without a continuation
                async_context_manager::leave(context);
                return symmetric::noop();
            }

            Result await_resume()
                noexcept(has_noexcept_await_resume<Awaiter>)
            {
                return awaiter.await_resume();
            }

        private:
            Awaiter awaiter;
            std::coroutine_handle<> wrapped;
        };

        // Note: it's awaitable and not awaitable<async_promise<T>>, because
        // we always supply a wrapped awaiter which has a type erased handle
        template<awaitable Awaitable>
        auto await_transform(Awaitable&& awaitable)
            requires (!inherit_async_task_invocable<Awaitable>)
        {
            return same_context_wrapped_awaiter<Awaitable>(
                std::forward<Awaitable>(awaitable));
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
            change_context_wrapped_awaiter(Awaitable&& awaitable, const actor_context& new_context)
                : awaiter(get_awaiter(std::forward<Awaitable>(awaitable)))
                , new_context(new_context)
            {}

            change_context_wrapped_awaiter(const change_context_wrapped_awaiter&) = delete;
            change_context_wrapped_awaiter& operator=(const change_context_wrapped_awaiter&) = delete;

            ~change_context_wrapped_awaiter() noexcept {
                if (wrapped) {
                    wrapped.destroy();
                }
            }

            bool await_ready()
                noexcept(has_noexcept_await_ready<Awaiter>)
            {
                async_task* task = async_task::current;
                assert(task);

                ready = awaiter.await_ready();

                return ready && task->context == new_context;
            }

            // Note: we allocate a wrapper in this method, so not noexcept
            COROACTORS_AWAIT_SUSPEND
            symmetric::result_t await_suspend(async_handle<T> c) {
                async_task* task = async_task::current;
                assert(task);

                if (ready) {
                    // Awaiter's await_ready returned true, so we perform a
                    // context switch here, as if returning from the awaiter.
                    assert(task->context != new_context);

                    return transfer(task, c);
                }

                // The wrapped handle restores context when resumed
                std::coroutine_handle<> k = wrapped =
                    async_restore_task_callback::wrap(task, c);

                // Prepare to resume in another thread
                task->leave();

                // Note: we still have context locked, but after the call to
                // awaiter's await_suspend our frame and task may be destroyed
                // and we cannot access any members. Make a copy of current
                // context and switch to a new context.
                auto old_context = std::move(task->context);
                task->context = new_context;

                // Restores task and context on exceptions
                scope_guard guard([task, &old_context]{
                    task->context = std::move(old_context);
                    task->enter();
                });

                if constexpr (has_await_suspend_void<Awaiter>) {
                    // Awaiter always suspends
                    awaiter.await_suspend(std::move(k));
                    guard.cancel();
                } else {
                    auto next = symmetric::intercept(
                        awaiter.await_suspend(std::move(k)));
                    guard.cancel();

                    if (!next) {
                        // Awaiter did not suspend, abort and resume, but also
                        // perform a context switch to the new context
                        return finish_transfer(task, old_context, c);
                    }

                    if (next != std::noop_coroutine()) {
                        // Awaiter is transferring to some valid coroutine
                        // handle. Note: this may be a different coroutine,
                        // even when its address is the same. Treat the
                        // original as destroyed.
                        async_context_manager::leave(old_context);
                        return symmetric::transfer(next);
                    }
                }

                // Awaiter suspended without a continuation
                async_context_manager::leave(old_context);
                return symmetric::noop();
            }

            Result await_resume()
                noexcept(has_noexcept_await_resume<Awaiter>)
            {
                return awaiter.await_resume();
            }

        private:
            symmetric::result_t transfer(async_task* task, std::coroutine_handle<> c) noexcept {
                auto old_context = std::move(task->context);
                task->context = new_context;

                task->leave();

                return finish_transfer(task, old_context, c);
            }

            symmetric::result_t finish_transfer(async_task* task,
                    const actor_context& old_context, std::coroutine_handle<> c) noexcept
            {
                r.task = task;
                r.continuation = c;

                if (!async_context_manager::transfer(old_context, task->context, r, /* returning */ true)) {
                    return symmetric::noop();
                }

                // We continue in the same thread
                task->enter();
                return symmetric::self(c);
            }

        private:
            Awaiter awaiter;
            const actor_context& new_context;
            std::coroutine_handle<> wrapped;
            async_context_switch_runnable r;
            bool ready = false;
        };

        // Note: it's awaitable and not awaitable<async_promise<T>>, because
        // we always supply a wrapped awaiter which has a type erased handle
        template<awaitable Awaitable>
        auto await_transform(actor_context::bind_awaitable_t<Awaitable> bound)
            requires (!inherit_async_task_invocable<Awaitable>)
        {
            preserve_caller_context();
            return change_context_wrapped_awaiter<Awaitable>(
                std::forward<Awaitable>(bound.awaitable),
                bound.context);
        }

        template<awaitable Awaitable>
        auto await_transform(actor_context::caller_context_t::bind_awaitable_t<Awaitable> bound)
            requires (!inherit_async_task_invocable<Awaitable>)
        {
            preserve_caller_context();
            return change_context_wrapped_awaiter<Awaitable>(
                std::forward<Awaitable>(bound.awaitable),
                get_caller_context());
        }

        struct yield_context_awaiter_t {
            bool await_ready() noexcept { return false; }
            void await_resume() noexcept {}

            COROACTORS_AWAIT_SUSPEND
            symmetric::result_t await_suspend(async_handle<T> c) noexcept {
                async_task* task = async_task::current;
                assert(task);

                // Prepare to resume in another thread
                task->leave();
                r.task = task;
                r.continuation = c;
                if (!async_context_manager::yield(task->context, r)) {
                    return symmetric::noop();
                }

                // We resume in the same thread
                task->enter();
                return symmetric::self(c);
            }

        private:
            async_context_switch_runnable r;
        };

        auto await_transform(actor_context::yield_t) noexcept {
            return yield_context_awaiter_t{};
        }

        struct preempt_context_awaiter_t {
            bool await_ready() noexcept { return false; }
            void await_resume() noexcept {}

            COROACTORS_AWAIT_SUSPEND
            symmetric::result_t await_suspend(async_handle<T> c) noexcept {
                async_task* task = async_task::current;
                assert(task);

                // Prepare to resume in another thread
                task->leave();
                r.task = task;
                r.continuation = c;
                if (!async_context_manager::preempt(task->context, r)) {
                    return symmetric::noop();
                }

                // We resume in the same thread
                task->enter();
                return symmetric::self(c);
            }

        private:
            async_context_switch_runnable r;
        };

        auto await_transform(actor_context::preempt_t) noexcept {
            return preempt_context_awaiter_t{};
        }

        friend void tag_invoke(inherit_async_task_fn, async_promise<T>& self) noexcept {
            assert(self.state.index() == 0);
            self.state.template emplace<async_task_inherited>();
        }

        template<awaitable<async_promise<T>> Awaitable>
            requires inherit_async_task_invocable<Awaitable>
        Awaitable&& await_transform(Awaitable&& awaitable) noexcept {
            inherit_async_task(awaitable);
            return (Awaitable&&) awaitable;
        }

        friend void tag_invoke(inherit_async_task_fn, async_promise<T>& self, const actor_context& context) noexcept {
            assert(self.state.index() == 0);
            self.state.template emplace<actor_context>(context);
        }

        template<awaitable<async_promise<T>> Awaitable>
            requires inherit_async_task_with_context_invocable<Awaitable>
        Awaitable&& await_transform(actor_context::bind_awaitable_t<Awaitable> bound) noexcept {
            preserve_caller_context();
            inherit_async_task(bound.awaitable, bound.context);
            return std::move(bound.awaitable);
        }

        template<awaitable<async_promise<T>> Awaitable>
            requires inherit_async_task_with_context_invocable<Awaitable>
        Awaitable&& await_transform(actor_context::caller_context_t::bind_awaitable_t<Awaitable> bound) noexcept {
            inherit_async_task(bound.awaitable, get_caller_context());
            return std::move(bound.awaitable);
        }

    private:
        struct async_task_inherited {};

    private:
        std::coroutine_handle<> continuation;
        std::variant<std::monostate, async_task_inherited, actor_context, async_task> state;
    };

    template<class T>
    class async_awaiter_base {
    public:
        explicit async_awaiter_base(async_handle<T> h) noexcept
            : handle(h)
        {}

        async_awaiter_base(const async_awaiter_base&) = delete;
        async_awaiter_base& operator=(const async_awaiter_base&) = delete;

        async_awaiter_base(async_awaiter_base&& rhs) noexcept
            : handle(std::exchange(rhs.handle, {}))
        {}

        ~async_awaiter_base() noexcept {
            if (handle) {
                handle.destroy();
            }
        }

        static bool await_ready() noexcept { return false; }

        template<class Promise>
        COROACTORS_AWAIT_SUSPEND
        symmetric::result_t await_suspend(std::coroutine_handle<Promise> c) noexcept {
            handle.promise().prepare(c);
            return symmetric::transfer(handle);
        }

        template<class Tag, class... Args>
            requires detail::tag_invocable<Tag, detail::async_promise<T>&, Args...>
        friend auto tag_invoke(Tag&& tag, async_awaiter_base<T>& awaiter, Args&&... args)
            noexcept(detail::nothrow_tag_invocable<Tag, detail::async_promise<T>&, Args...>)
            -> detail::tag_invoke_result_t<Tag, detail::async_promise<T>&, Args...>
        {
            return tag_invoke((Tag&&) tag, awaiter.handle.promise(), (Args&&) args...);
        }

    protected:
        result<T>&& take_result() noexcept {
            return handle.promise().take_result();
        }

    private:
        async_handle<T> handle;
    };

    template<class T>
    class [[nodiscard]] async_awaiter : public async_awaiter_base<T> {
        using base = async_awaiter_base<T>;

    public:
        using base::base;

        T await_resume() {
            return base::take_result().take_value();
        }
    };

    template<class T>
    class [[nodiscard]] async_result_awaiter : public async_awaiter_base<T> {
        using base = async_awaiter_base<T>;

    public:
        using base::base;

        result<T> await_resume() noexcept(std::is_nothrow_move_constructible_v<result<T>>) {
            return base::take_result();
        }
    };

} // namespace coroactors::detail