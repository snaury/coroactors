#pragma once
#include <coroactors/detail/async.h>
#include <coroactors/stop_token.h>

namespace coroactors {

    /**
     * Used as the return type of async coroutines
     *
     * Each async coroutine has an associated task, that represents a logical
     * thread of execution, and which is inherited on `co_await` of other
     * async coroutines. Async coroutines may have an associated stop_token
     * for concellation, bound actor_context for isolation, and task local
     * values for transparent propagation of contextual information.
     *
     * Associated task is mounted to the current thread and considered running
     * as long as an async coroutine is running. Task is automatically unmounted
     * when coroutine suspends (e.g. when awaiting results of async operations),
     * and remounted (possibly in another thread) when coroutine resumes. While
     * the task is suspended its bound actor_context is also released, and
     * reacquired before the task resumes.
     */
    template<class T>
    class [[nodiscard]] async {
        template<class U>
        friend class detail::async_promise;

    private:
        explicit async(detail::async_handle<T> handle) noexcept
            : handle(handle)
        {}

    public:
        using promise_type = detail::async_promise<T>;
        using result_type = ::coroactors::result<T>;
        using value_type = T;

    public:
        async() noexcept = default;

        async(async&& rhs) noexcept
            : handle(std::exchange(rhs.handle, {}))
        {}

        ~async() noexcept {
            if (handle) {
                handle.destroy();
            }
        }

        async& operator=(async&& rhs) noexcept {
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

        auto operator co_await() && noexcept {
            return detail::async_awaiter<T>(std::exchange(handle, {}));
        }

        auto result() && noexcept {
            return detail::async_result_awaiter<T>(std::exchange(handle, {}));
        }

        void detach() && noexcept {
            auto h = std::exchange(handle, {});
            h.promise().prepare(nullptr);
            h.resume();
        }

        template<class Tag, class... Args>
            requires detail::tag_invocable<Tag, detail::async_promise<T>&, Args...>
        friend auto tag_invoke(Tag&& tag, async<T>& coro, Args&&... args)
            noexcept(detail::nothrow_tag_invocable<Tag, detail::async_promise<T>&, Args...>)
            -> detail::tag_invoke_result_t<Tag, detail::async_promise<T>&, Args...>
        {
            return tag_invoke((Tag&&) tag, coro.handle.promise(), (Args&&) args...);
        }

    private:
        detail::async_handle<T> handle;
    };

    using detail::current_stop_token;
    using detail::current_actor_context;

} // namespace coroactors
