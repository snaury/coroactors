#pragma once
#include <coroutine>
#include <concepts>

namespace coroactors::detail {

    template<class Callback>
    concept is_resume_callback_void = requires(Callback& callback) {
        { callback() } noexcept -> std::same_as<void>;
    };

    template<class Callback>
    concept is_resume_callback_handle = requires(Callback& callback) {
        { callback() } noexcept -> std::convertible_to<std::coroutine_handle<>>;
    };

    template<class Callback>
    class with_resume_callback_promise;

    template<class Callback>
    using with_resume_callback_handle = std::coroutine_handle<with_resume_callback_promise<Callback>>;

    template<class Callback>
    struct with_resume_callback_coroutine {
        using promise_type = with_resume_callback_promise<Callback>;

        std::coroutine_handle<> handle;
    };

    template<class Callback>
    class with_resume_callback_promise {
    public:
        with_resume_callback_promise(Callback& callback)
            : callback(callback)
        {}

        with_resume_callback_coroutine<Callback> get_return_object() noexcept {
            return with_resume_callback_coroutine<Callback>{
                with_resume_callback_handle<Callback>::from_promise(*this)
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
            static void await_suspend(with_resume_callback_handle<Callback> h) noexcept
                requires (is_resume_callback_void<Callback>)
            {
                auto& self = h.promise();
                self.callback();
            }

            __attribute__((__noinline__))
            static std::coroutine_handle<> await_suspend(with_resume_callback_handle<Callback> h) noexcept
                requires (is_resume_callback_handle<Callback>)
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
        Callback& callback;
    };

    template<class Callback>
    with_resume_callback_coroutine<Callback> make_resume_callback_coroutine(Callback) {
        co_return;
    }

} // namespace coroactors::detail

namespace coroactors {

    /**
     * Returns a coroutine handle that, when resumed, calls the specified callback.
     *
     * Callback may optionally return a coroutine handle that will run after callback returns.
     */
    template<class Callback>
    std::coroutine_handle<> with_resume_callback(Callback&& callback)
        requires (
            detail::is_resume_callback_void<Callback> ||
            detail::is_resume_callback_handle<Callback>)
    {
        // Note: if callback is an rvalue then Callback will not be a reference
        // and we will move the object to coroutine frame. However in cases
        // where callback is an lvalue reference Callback will be a reference
        // and we will store that reference in the frame, not an object copy.
        return detail::make_resume_callback_coroutine<Callback>(std::forward<Callback>(callback)).handle;
    }

} // namespace coroactors
