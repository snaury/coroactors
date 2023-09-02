#pragma once
#include <coroactors/result.h>
#include <atomic>
#include <cassert>
#include <coroutine>

namespace coroactors {

    template<class T>
    class task;

} // namespace coroactors

namespace coroactors::detail {

    template<class T>
    class task_promise;

    template<class T>
    using task_continuation = std::coroutine_handle<task_promise<T>>;

    template<class T>
    class task_result_handler {
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
    class task_result_handler<void> {
    public:
        void return_void() {
            this->result_.set_value();
        }

    protected:
        result<void> result_;
    };

    template<class T>
    class task_promise final : public task_result_handler<T> {
        static constexpr uintptr_t MarkerFinished = 1;

    public:
        auto get_return_object() noexcept {
            return task<T>(task_continuation<T>::from_promise(*this));
        }

        void unhandled_exception() noexcept {
            this->result_.set_exception(std::current_exception());
        }

        static auto initial_suspend() noexcept { return std::suspend_always{}; }

        struct final_suspend_t {
            static bool await_ready() noexcept { return false; }
            static void await_resume() noexcept { /* never called */ }

            static std::coroutine_handle<> await_suspend(task_continuation<T> h) noexcept {
                auto& self = h.promise();
                return self.continuation;
            }
        };

        static auto final_suspend() noexcept { return final_suspend_t{}; }

        bool ready() const noexcept {
            return false;
        }

        void set_continuation(std::coroutine_handle<> h) {
            if (continuation) [[unlikely]] {
                throw std::logic_error("task cannot be awaited more than once");
            }
            continuation = h;
        }

        T take_result() {
            return this->result_.take_value();
        }

    private:
        std::coroutine_handle<> continuation;
    };

} // namespace coroactors::detail
