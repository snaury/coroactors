#pragma once
#include <coroactors/detail/config.h>
#include <coroutine>
#include <concepts>

namespace coroactors::detail {

    template<class Callback>
    concept is_resume_callback_void = requires(Callback& callback) {
        { std::move(callback)() } noexcept -> std::same_as<void>;
    };

    template<class Callback>
    concept is_resume_callback_handle = requires(Callback& callback) {
        { std::move(callback)() } noexcept -> std::convertible_to<std::coroutine_handle<>>;
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

        struct final_suspend_t {
            static bool await_ready() noexcept { return false; }

            COROACTORS_AWAIT_SUSPEND
            static void await_suspend(with_resume_callback_handle<Callback> h) noexcept
                requires (is_resume_callback_void<Callback>)
            {
                auto& self = h.promise();
                std::move(self.callback)();
            }

            COROACTORS_AWAIT_SUSPEND
            static std::coroutine_handle<> await_suspend(with_resume_callback_handle<Callback> h) noexcept
                requires (is_resume_callback_handle<Callback>)
            {
                auto& self = h.promise();
                return std::move(self.callback)();
            }

            static void await_resume() noexcept {}
        };

        auto final_suspend() noexcept { return final_suspend_t{}; }

    private:
        Callback& callback;
    };

    template<class Callback>
    with_resume_callback_coroutine<Callback> make_resume_callback_coroutine(Callback) {
        co_return;
    }

} // namespace coroactors::detail
