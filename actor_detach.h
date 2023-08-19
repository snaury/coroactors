#pragma once
#include "detail/get_awaiter.h"
#include <coroutine>
#include <exception>

namespace coroactors::detail {

    template<class T>
    class detach_awaitable_ignore_result_handler {
    public:
        detach_awaitable_ignore_result_handler() = default;

        template<class TArg>
        void return_value(TArg&&) noexcept {
            // ignore
        }
    };

    template<>
    class detach_awaitable_ignore_result_handler<void> {
    public:
        detach_awaitable_ignore_result_handler() = default;

        void return_void() noexcept {
            // ignore
        }
    };

    template<class TAwaitable>
    struct detach_awaitable_ignore_coroutine;

    template<class TAwaitable>
    class detach_awaitable_ignore_promise
        : public detach_awaitable_ignore_result_handler<await_result_t<TAwaitable>>
    {
        using handle_t = std::coroutine_handle<detach_awaitable_ignore_promise<TAwaitable>>;

    public:
        detach_awaitable_ignore_promise() = default;

        detach_awaitable_ignore_coroutine<TAwaitable> get_return_object() noexcept {
            return {};
        }

        static auto initial_suspend() noexcept { return std::suspend_never{}; }
        static auto final_suspend() noexcept { return std::suspend_never{}; }

        void unhandled_exception() noexcept {
            std::terminate();
        }
    };

    template<class TAwaitable>
    struct detach_awaitable_ignore_coroutine {
        using promise_type = detach_awaitable_ignore_promise<TAwaitable>;
    };

    template<class TCallback, class T>
    class detach_awaitable_callback_result_handler {
    public:
        detach_awaitable_callback_result_handler(TCallback& callback)
            : callback_(callback)
        {}

        template<class TArg>
        void return_value(TArg&& arg) noexcept {
            callback_(std::forward<TArg>(arg));
        }

    private:
        TCallback& callback_;
    };

    template<class TCallback>
    class detach_awaitable_callback_result_handler<TCallback, void> {
    public:
        detach_awaitable_callback_result_handler(TCallback& callback)
            : callback_(callback)
        {}

        void return_void() noexcept {
            callback_();
        }

    private:
        TCallback& callback_;
    };

    template<class TAwaitable, class TCallback>
    struct detach_awaitable_callback_coroutine;

    template<class TAwaitable, class TCallback>
    class detach_awaitable_callback_promise
        : public detach_awaitable_callback_result_handler<TCallback, await_result_t<TAwaitable>>
    {
        using handle_t = std::coroutine_handle<detach_awaitable_callback_promise<TAwaitable, TCallback>>;
        using result_handler_t = detach_awaitable_callback_result_handler<TCallback, await_result_t<TAwaitable>>;

    public:
        detach_awaitable_callback_promise(TAwaitable&, TCallback& callback)
            : result_handler_t(callback)
        {}

        detach_awaitable_callback_coroutine<TAwaitable, TCallback> get_return_object() noexcept {
            return {};
        }

        static auto initial_suspend() noexcept { return std::suspend_never{}; }
        static auto final_suspend() noexcept { return std::suspend_never{}; }

        void unhandled_exception() noexcept {
            std::terminate();
        }
    };

    template<class TAwaitable, class TCallback>
    struct detach_awaitable_callback_coroutine {
        using promise_type = detach_awaitable_callback_promise<TAwaitable, TCallback>;
    };

} // namespace coroactors::detail

namespace coroactors {

    template<class TAwaitable>
    detail::detach_awaitable_ignore_coroutine<TAwaitable>
    detach_awaitable(TAwaitable awaitable) {
        co_return co_await std::move(awaitable);
    }

    template<class TAwaitable, class TCallback>
    detail::detach_awaitable_callback_coroutine<TAwaitable, TCallback>
    detach_awaitable(TAwaitable awaitable, TCallback) {
        // Note: underlying promise takes callback argument address and calls it when we return
        co_return co_await std::move(awaitable);
    }

} // namespace coroactors
