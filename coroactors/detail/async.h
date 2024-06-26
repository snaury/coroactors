#pragma once
#include <coroactors/detail/async_context.h>
#include <coroactors/detail/async_task.h>
#include <coroactors/detail/awaiters.h>
#include <coroactors/detail/config.h>
#include <coroactors/detail/scope_guard.h>
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
        async_task* task = nullptr;
        std::coroutine_handle<> continuation;

        async_context_switch_runnable() noexcept = default;

        async_context_switch_runnable(async_task* task, std::coroutine_handle<> continuation)
            : task(task)
            , continuation(continuation)
        {}

        void run() noexcept override {
            assert(task);
            task->scheduled = true;
            // Note: `this` will be destroyed inside this call
            continuation.resume();
        }
    };

    /**
     * A callback that restores task context before resuming a continuation
     */
    class async_restore_task_callback : private async_context_switch_runnable {
        using async_context_switch_runnable::async_context_switch_runnable;

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
            : async_context_switch_runnable(rhs.task, rhs.continuation)
        {}

        std::coroutine_handle<> operator()() noexcept {
            if (async_context_manager::enter(task->context, *this)) {
                // We don't know which code resumed us and where
                // Assume the worst: we are running recursively on top of
                // something else.
                task->scheduled = false;
                return continuation;
            } else {
                return std::noop_coroutine();
            }
        }
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

        struct initial_suspend_t {
            async_promise<T>& p;

            static bool await_ready() noexcept { return false; }
            static void await_suspend(async_handle<T>) {}

            void await_resume() noexcept {
                p.get_task()->enter();
            }
        };

        auto initial_suspend() noexcept { return initial_suspend_t{ *this }; }

        struct final_suspend_t {
            static bool await_ready() noexcept { return false; }
            static void await_resume() noexcept { /* promise destroyed */ }

            COROACTORS_AWAIT_SUSPEND
            std::coroutine_handle<> await_suspend(async_handle<T> h) noexcept {
                auto& p = h.promise();

                switch (p.state.index()) {
                    case 1: {
                        // Inherited task
                        inherited_t& s = std::get<1>(p.state);
                        async_task* task = s.task;
                        assert(task == async_task::current());
                        task->leave();
                        if (s.caller_context) {
                            // Maybe different context, try to restore
                            actor_context from_context = std::move(task->context);
                            task->context = std::move(*s.caller_context);
                            // Prepare to possibly resume in another thread
                            r.task = task;
                            r.continuation = p.continuation;
                            // Try to change context, when false we will resume somewhere else
                            if (!async_context_manager::transfer(from_context, task->context, r, /* returning */ true)) {
                                return std::noop_coroutine();
                            }
                        }
                        // We continue in the same thread, context is correct
                        break;
                    }
                    case 2: {
                        // Our private task
                        async_task* task = &std::get<2>(p.state);
                        assert(task == async_task::current());
                        task->leave();
                        actor_context from_context = std::move(task->context);
                        async_context_manager::leave(from_context);
                        // We continue in the same thread, no context
                        break;
                    }
                    default: {
                        fail("Unexpected async promise state");
                    }
                }

                if (!p.continuation) {
                    // Async function finished after a detach
                    // We have to destroy ourselves, since nobody is waiting
                    h.destroy();
                    return std::noop_coroutine();
                }

                // We return to the continuation
                return p.continuation;
            }

        private:
            async_context_switch_runnable r;
        };

        static auto final_suspend() noexcept { return final_suspend_t{}; }

        /**
         * Prepare to run in an inherited or a private task and eventually
         * return to c (nullptr when detached).
         *
         * Returns a task that would need to be restored by the awaiter.
         */
        async_task* prepare(std::coroutine_handle<> c) noexcept {
            continuation = c;
            switch (state.index()) {
                case 0: {
                    state.template emplace<async_task>();
                    return nullptr;
                }
                case 1: {
                    inherited_t& s = std::get<1>(state);
                    return s.task;
                }
                default: {
                    fail("Unexpected async promise state");
                }
            }
        }

        async_task* get_task() noexcept {
            switch (state.index()) {
                case 1: {
                    inherited_t& s = std::get<1>(state);
                    return s.task;
                }
                case 2: {
                    async_task& t = std::get<2>(state);
                    return &t;
                }
                default: {
                    fail("Unexpected async promise state");
                }
            }
        }

        const actor_context& get_caller_context() const noexcept {
            switch (state.index()) {
                case 1: {
                    const inherited_t& s = std::get<1>(state);
                    if (s.caller_context) {
                        return *s.caller_context;
                    } else {
                        return s.task->context;
                    }
                }
                case 2: {
                    return no_actor_context;
                }
                default: {
                    fail("Unexpected async promise state");
                }
            }
        }

        bool preserve_caller_context() noexcept {
            switch (state.index()) {
                case 1: {
                    inherited_t& s = std::get<1>(state);
                    if (!s.caller_context) {
                        s.caller_context.emplace(s.task->context);
                        return true;
                    }
                    return false;
                }
                case 2: {
                    return false;
                }
                default: {
                    fail("Unexpected async promise state");
                }
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
            bool await_suspend(async_handle<T> c) noexcept {
                assert(type != ESwitchContext::Ready);

                async_task* task = c.promise().get_task();
                assert(task == async_task::current());

                auto context = std::move(task->context);
                task->context = to;

                // Prepare to possibly resume in another thread
                task->leave();
                r.task = task;
                r.continuation = c;

                if (!context) {
                    if (!async_context_manager::enter(task->context, r, !task->scheduled)) {
                        return true;
                    }
                } else {
                    // We are returning from co_await most of the time
                    // But the first co_await context() is special
                    bool returning = type == ESwitchContext::Return;
                    if (!async_context_manager::transfer(context, task->context, r, returning)) {
                        return true;
                    }
                }

                // We continue in the same thread
                return false;
            }

            void await_resume() noexcept {
                if (r.task) {
                    r.task->enter();
                }
            }

        private:
            const actor_context& to;
            const ESwitchContext type;
            async_context_switch_runnable r;
        };

        auto await_transform(actor_context::bind_context_t bound) noexcept {
            async_task* task = get_task();

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
            auto await_suspend(async_handle<T> c) {
                async_task* task = c.promise().get_task();
                assert(task == async_task::current());

                // The wrapped handle restores context when resumed
                std::coroutine_handle<> k = wrapped =
                    async_restore_task_callback::wrap(task, c);

                // Prepare to resume in another thread
                task->leave();
                restore_task = task;

                // Note: we still have context locked, but after the call to
                // awaiter's await_suspend our frame and task may be destroyed
                // and we cannot access any members. Make a copy of current
                // context.
                auto context = task->context;

                // Restores task on exceptions
                scope_guard guard([this, task]{
                    task->enter();
                    restore_task = nullptr;
                });

                return this->suspend(guard, context, k);
            }

            Result await_resume()
                noexcept(has_noexcept_await_resume<Awaiter>)
            {
                scope_guard guard([task = restore_task]{
                    if (task) {
                        task->enter();
                    }
                });
                return awaiter.await_resume();
            }

        private:
            std::coroutine_handle<> suspend(
                    auto& guard, const actor_context& context,
                    std::coroutine_handle<> k)
                noexcept(has_noexcept_await_suspend<Awaiter>)
                requires(has_await_suspend_handle<Awaiter>)
            {
                // Awaiter always returns some valid coroutine handle.
                // Note: this may be a different coroutine, even when its
                // address is the same (it may finish in another thread,
                // with the same address reused and added to some queue).
                // For us it's identical to awaiter always suspending.
                auto next = awaiter.await_suspend(k);
                guard.cancel();
                async_context_manager::leave(context);
                return next;
            }

            void suspend(
                    auto& guard, const actor_context& context,
                    std::coroutine_handle<> k)
                noexcept(has_noexcept_await_suspend<Awaiter>)
                requires(has_await_suspend_void<Awaiter>)
            {
                // Awaiter always suspends
                awaiter.await_suspend(k);
                guard.cancel();
                async_context_manager::leave(context);
            }

            bool suspend(
                    auto& guard, const actor_context& context,
                    std::coroutine_handle<> k)
                noexcept(has_noexcept_await_suspend<Awaiter>)
                requires(has_await_suspend_bool<Awaiter>)
            {
                // Awaiter may choose not to suspend
                bool suspended = awaiter.await_suspend(k);
                guard.cancel();
                if (!suspended) {
                    // Awaiter did not suspend, keep current context
                    return false;
                }
                async_context_manager::leave(context);
                return true;
            }

        private:
            Awaiter awaiter;
            std::coroutine_handle<> wrapped;
            async_task* restore_task = nullptr;
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
                async_task* task = async_task::current();
                assert(task);

                ready = awaiter.await_ready();

                return ready && task->context == new_context;
            }

            // Note: we allocate a wrapper in this method, so not noexcept
            COROACTORS_AWAIT_SUSPEND
            auto await_suspend(async_handle<T> c) {
                async_task* task = c.promise().get_task();
                assert(task == async_task::current());

                // The wrapped handle restores context when resumed
                std::coroutine_handle<> k;
                if (!ready) {
                    // Give this a chance to throw before we setup guards,
                    // in case everything else below is actually noexcept.
                    k = wrapped = async_restore_task_callback::wrap(task, c);
                }

                // Prepare to resume in another thread
                task->leave();
                r.task = task;
                r.continuation = c;

                // Note: we still have context locked, but after the call to
                // awaiter's await_suspend our frame and task may be destroyed
                // and we cannot access any members. Make a copy of current
                // context and switch to a new context.
                auto old_context = std::move(task->context);
                task->context = new_context;

                // Restores task and context on exceptions
                scope_guard guard([this, task, &old_context]{
                    task->context = std::move(old_context);
                    task->enter();
                    r.task = nullptr;
                });

                if (ready) {
                    // Awaiter's await_ready returned true, so we perform a
                    // context switch here, as if returning from the awaiter.
                    return this->transfer_to(old_context);
                }

                return this->suspend(guard, old_context, k);
            }

            Result await_resume()
                noexcept(has_noexcept_await_resume<Awaiter>)
            {
                scope_guard guard([this]{
                    if (r.task) {
                        r.task->enter();
                    }
                });
                return awaiter.await_resume();
            }

        private:
            std::coroutine_handle<> suspend(
                    auto& guard, const actor_context& old_context,
                    std::coroutine_handle<> k)
                noexcept(has_noexcept_await_suspend<Awaiter>)
                requires(has_await_suspend_handle<Awaiter>)
            {
                auto next = awaiter.await_suspend(k);
                guard.cancel();
                // Awaiter always returns some valid coroutine handle.
                // Note: this may be a different coroutine, even when its
                // address is the same (it may finish in another thread,
                // with the same address reused and added to some queue).
                // For us it's identical to awaiter always suspending.
                async_context_manager::leave(old_context);
                return next;
            }

            bool suspend(
                    auto& guard, const actor_context& old_context,
                    std::coroutine_handle<> k)
                noexcept(has_noexcept_await_suspend<Awaiter>)
                requires(has_await_suspend_void<Awaiter>)
            {
                awaiter.await_suspend(k);
                guard.cancel();
                // Awaiter always suspends
                async_context_manager::leave(old_context);
                return true;
            }

            bool suspend(
                    auto& guard, const actor_context& old_context,
                    std::coroutine_handle<> k)
                noexcept(has_noexcept_await_suspend<Awaiter>)
                requires(has_await_suspend_bool<Awaiter>)
            {
                auto suspended = awaiter.await_suspend(k);
                guard.cancel();
                if (!suspended) {
                    // Awaiter did not suspend, abort and try to resume, but
                    // also perform a context switch to the new context
                    return this->transfer_to(old_context);
                }
                // Awaiter suspended
                async_context_manager::leave(old_context);
                return true;
            }

        private:
            std::coroutine_handle<> transfer_to(const actor_context& old_context) noexcept
                requires(has_await_suspend_handle<Awaiter>)
            {
                if (!async_context_manager::transfer(old_context, r.task->context, r, /* returning */ true)) {
                    return std::noop_coroutine();
                }

                // We continue in the same thread
                return r.continuation;
            }

            bool transfer_to(const actor_context& old_context) noexcept
                requires(has_await_suspend_void<Awaiter> || has_await_suspend_bool<Awaiter>)
            {
                if (!async_context_manager::transfer(old_context, r.task->context, r, /* returning */ true)) {
                    return true;
                }

                // We continue in the same thread
                return false;
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

            COROACTORS_AWAIT_SUSPEND
            bool await_suspend(async_handle<T> c) noexcept {
                async_task* task = c.promise().get_task();
                assert(task == async_task::current());

                // Prepare to resume in another thread
                task->leave();
                r.task = task;
                r.continuation = c;

                if (!async_context_manager::yield(task->context, r)) {
                    return true;
                }

                // We resume in the same thread
                return false;
            }

            void await_resume() noexcept {
                r.task->enter();
            }

        private:
            async_context_switch_runnable r;
        };

        auto await_transform(actor_context::yield_t) noexcept {
            return yield_context_awaiter_t{};
        }

        struct preempt_context_awaiter_t {
            bool await_ready() noexcept { return false; }

            COROACTORS_AWAIT_SUSPEND
            bool await_suspend(async_handle<T> c) noexcept {
                async_task* task = c.promise().get_task();
                assert(task == async_task::current());

                // Prepare to resume in another thread
                task->leave();
                r.task = task;
                r.continuation = c;

                if (!async_context_manager::preempt(task->context, r)) {
                    return true;
                }

                // We resume in the same thread
                return false;
            }

            void await_resume() noexcept {
                r.task->enter();
            }

        private:
            async_context_switch_runnable r;
        };

        auto await_transform(actor_context::preempt_t) noexcept {
            return preempt_context_awaiter_t{};
        }

        friend void tag_invoke(inherit_async_task_fn, async_promise<T>& self) noexcept {
            assert(self.state.index() == 0);
            self.state.template emplace<inherited_t>();
        }

        template<awaitable<async_promise<T>> Awaitable>
            requires inherit_async_task_invocable<Awaitable>
        Awaitable&& await_transform(Awaitable&& awaitable) noexcept {
            inherit_async_task(awaitable);
            return (Awaitable&&) awaitable;
        }

        friend void tag_invoke(inherit_async_task_fn, async_promise<T>& self, const actor_context& context) noexcept {
            assert(self.state.index() == 0);
            self.state.template emplace<inherited_t>(context);
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
        /**
         * When used in a noexcept method causes std::terminate with a message
         */
        [[noreturn]] static void fail(const char* message) {
            throw std::logic_error(message);
        }

    private:
        struct inherited_t {
            async_task* task;
            std::optional<actor_context> caller_context;

            explicit inherited_t() noexcept
                : task(async_task::current())
            {
                assert(task);
            }

            explicit inherited_t(const actor_context& context) noexcept
                : task(async_task::current())
                , caller_context(context)
            {
                assert(task);
            }
        };

    private:
        std::coroutine_handle<> continuation;
        std::variant<std::monostate, inherited_t, async_task> state;
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
        std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> c) noexcept {
            restore_task = handle.promise().prepare(c);
            if (restore_task) {
                restore_task->leave();
            }
            return handle;
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
        void resumed() noexcept {
            if (restore_task) {
                restore_task->enter();
            }
        }

        result<T>&& take_result() noexcept {
            return handle.promise().take_result();
        }

    private:
        async_handle<T> handle;
        async_task* restore_task = nullptr;
    };

    template<class T>
    class [[nodiscard]] async_awaiter : public async_awaiter_base<T> {
        using base = async_awaiter_base<T>;

    public:
        using base::base;

        T await_resume() {
            base::resumed();
            return base::take_result().take_value();
        }
    };

    template<class T>
    class [[nodiscard]] async_result_awaiter : public async_awaiter_base<T> {
        using base = async_awaiter_base<T>;

    public:
        using base::base;

        result<T> await_resume() noexcept(std::is_nothrow_move_constructible_v<result<T>>) {
            base::resumed();
            return base::take_result();
        }
    };

} // namespace coroactors::detail
