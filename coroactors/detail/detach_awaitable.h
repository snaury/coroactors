#pragma once
#include <coroactors/detail/awaiters.h>
#include <coroutine>
#include <exception>

namespace coroactors::detail {

    template<class T>
    class detach_awaitable_ignore_result_handler {
    public:
        detach_awaitable_ignore_result_handler() noexcept = default;

        template<class Value>
        void return_value(Value&&) noexcept {
            // ignore
        }
    };

    template<>
    class detach_awaitable_ignore_result_handler<void> {
    public:
        detach_awaitable_ignore_result_handler() noexcept = default;

        void return_void() noexcept {
            // ignore
        }
    };

    template<class Awaitable>
    struct detach_awaitable_ignore_coroutine;

    template<class Awaitable>
    class detach_awaitable_ignore_promise
        : public detach_awaitable_ignore_result_handler<await_result_t<Awaitable>>
    {
        using handle_t = std::coroutine_handle<detach_awaitable_ignore_promise<Awaitable>>;

    public:
        detach_awaitable_ignore_promise() noexcept = default;

        detach_awaitable_ignore_coroutine<Awaitable> get_return_object() noexcept {
            return {};
        }

        static auto initial_suspend() noexcept { return std::suspend_never{}; }
        static auto final_suspend() noexcept { return std::suspend_never{}; }

        void unhandled_exception() noexcept {
            std::terminate();
        }
    };

    template<class Awaitable>
    struct detach_awaitable_ignore_coroutine {
        using promise_type = detach_awaitable_ignore_promise<Awaitable>;
    };

    template<class Callback, class T>
    class detach_awaitable_callback_result_handler {
    public:
        detach_awaitable_callback_result_handler(Callback& callback) noexcept
            : callback_(callback)
        {}

        template<class Value>
        void return_value(Value&& value) {
            callback_(std::forward<Value>(value));
        }

    private:
        Callback& callback_;
    };

    template<class Callback>
    class detach_awaitable_callback_result_handler<Callback, void> {
    public:
        detach_awaitable_callback_result_handler(Callback& callback) noexcept
            : callback_(callback)
        {}

        void return_void() {
            callback_();
        }

    private:
        Callback& callback_;
    };

    template<class Awaitable, class Callback>
    struct detach_awaitable_callback_coroutine;

    template<class Awaitable, class Callback>
    class detach_awaitable_callback_promise
        : public detach_awaitable_callback_result_handler<Callback, await_result_t<Awaitable>>
    {
        using handle_t = std::coroutine_handle<detach_awaitable_callback_promise<Awaitable, Callback>>;
        using result_handler_t = detach_awaitable_callback_result_handler<Callback, await_result_t<Awaitable>>;

    public:
        detach_awaitable_callback_promise(Awaitable&, Callback& callback) noexcept
            : result_handler_t(callback)
        {}

        detach_awaitable_callback_coroutine<Awaitable, Callback> get_return_object() noexcept {
            return {};
        }

        static auto initial_suspend() noexcept { return std::suspend_never{}; }
        static auto final_suspend() noexcept { return std::suspend_never{}; }

        void unhandled_exception() noexcept {
            std::terminate();
        }
    };

    template<class Awaitable, class Callback>
    struct detach_awaitable_callback_coroutine {
        using promise_type = detach_awaitable_callback_promise<Awaitable, Callback>;
    };

} // namespace coroactors::detail
