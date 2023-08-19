#pragma once
#include <coroutine>
#include <concepts>

namespace coroactors::detail {

    template<class TCallback>
    concept is_resume_callback_void = requires(TCallback& callback) {
        { callback() } noexcept -> std::same_as<void>;
    };

    template<class TCallback>
    concept is_resume_callback_handle = requires(TCallback& callback) {
        { callback() } noexcept -> std::convertible_to<std::coroutine_handle<>>;
    };

    template<class TCallback>
    class with_resume_callback_promise;

    template<class TCallback>
    using with_resume_callback_handle = std::coroutine_handle<with_resume_callback_promise<TCallback>>;

    template<class TCallback>
    struct with_resume_callback_coroutine {
        using promise_type = with_resume_callback_promise<TCallback>;

        std::coroutine_handle<> handle;
    };

    template<class TCallback>
    class with_resume_callback_promise {
    public:
        with_resume_callback_promise(TCallback& callback)
            : callback(callback)
        {}

        with_resume_callback_coroutine<TCallback> get_return_object() noexcept {
            return with_resume_callback_coroutine<TCallback>{
                with_resume_callback_handle<TCallback>::from_promise(*this)
            };
        }

        void unhandled_exception() noexcept {
            // cannot happen
        }

        void return_void() noexcept {
            // coroutine body resumed
        }

        auto initial_suspend() noexcept { return std::suspend_always{}; }

        struct TFinalSuspend {
            static bool await_ready() noexcept { return false; }

            __attribute__((__noinline__))
            static void await_suspend(with_resume_callback_handle<TCallback> h) noexcept
                requires (is_resume_callback_void<TCallback>)
            {
                auto& self = h.promise();
                self.callback();
            }

            __attribute__((__noinline__))
            static std::coroutine_handle<> await_suspend(with_resume_callback_handle<TCallback> h) noexcept
                requires (is_resume_callback_handle<TCallback>)
            {
                auto& self = h.promise();
                std::coroutine_handle<> c = self.callback();
                h.destroy();
                return c;
            }

            static void await_resume() noexcept {}
        };

        auto final_suspend() noexcept { return TFinalSuspend{}; }

    private:
        TCallback& callback;
    };

    template<class TCallback>
    with_resume_callback_coroutine<TCallback> make_resume_callback_coroutine(TCallback) {
        co_return;
    }

} // namespace coroactors::detail

namespace coroactors {

    /**
     * Returns a coroutine handle that, when resumed, calls the specified callback.
     *
     * Callback may optionally return a coroutine handle that will run after callback returns.
     */
    template<class TCallback>
    std::coroutine_handle<> with_resume_callback(TCallback&& callback)
        requires (
            detail::is_resume_callback_void<TCallback> ||
            detail::is_resume_callback_handle<TCallback>)
    {
        // Note: if callback is an rvalue then TCallback will not be a reference
        // and we will move the object to coroutine frame. However in cases
        // where callback is an lvalue reference TCallback will be a reference
        // and we will store that reference in the frame, not an object copy.
        return detail::make_resume_callback_coroutine<TCallback>(std::forward<TCallback>(callback)).handle;
    }

} // namespace coroactors
