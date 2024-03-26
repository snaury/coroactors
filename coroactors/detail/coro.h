#pragma once
#include <coroactors/result.h>
#include <cassert>
#include <coroutine>

namespace coroactors {

    template<class T>
    class coro;

} // namespace coroactors

namespace coroactors::detail {

    template<class T>
    class coro_promise;

    template<class T>
    using coro_handle = std::coroutine_handle<coro_promise<T>>;

    template<class T>
    class coro_result_handler {
    public:
        template<class Value>
        void return_value(Value&& value)
            requires (std::is_convertible_v<Value&&, T>)
        {
            this->result_.set_value(std::forward<Value>(value));
        }

    protected:
        result<T> result_;
    };

    template<>
    class coro_result_handler<void> {
    public:
        void return_void() {
            this->result_.set_value();
        }

    protected:
        result<void> result_;
    };

    template<class T>
    class coro_promise final : public coro_result_handler<T> {
    public:
        auto get_return_object() noexcept {
            return coro<T>(coro_handle<T>::from_promise(*this));
        }

        void unhandled_exception() noexcept {
            this->result_.set_exception(std::current_exception());
        }

        static auto initial_suspend() noexcept { return std::suspend_always{}; }

        struct final_suspend_t {
            static bool await_ready() noexcept { return false; }
            static void await_resume() noexcept { /* never called */ }

            static std::coroutine_handle<> await_suspend(coro_handle<T> h) noexcept {
                return h.promise().continuation;
            }
        };

        static auto final_suspend() noexcept { return final_suspend_t{}; }

        bool ready() const noexcept {
            return bool(this->result_);
        }

        void set_continuation(std::coroutine_handle<> h) noexcept {
            continuation = h;
        }

        T get_result() {
            return this->result_.get_value();
        }

        T take_result() {
            return this->result_.take_value();
        }

    private:
        std::coroutine_handle<> continuation;
    };

} // namespace coroactors::detail
