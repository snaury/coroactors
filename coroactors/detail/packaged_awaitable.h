#pragma once
#include <coroactors/detail/awaiters.h>
#include <coroactors/detail/result.h>

namespace coroactors::detail {

    template<class T>
    struct packaged_awaitable_coroutine;

    template<class T>
    class packaged_awaitable_promise;

    template<class T>
    using packaged_awaitable_handle = std::coroutine_handle<packaged_awaitable_promise<T>>;

    template<class T>
    class packaged_awaitable_result_handler {
    public:
        template<class Value>
        void return_value(Value&& value)
            requires std::is_convertible_v<Value&&, T>
        {
            if (result_ptr) {
                result_ptr->set_value(std::forward<Value>(value));
            }
        }

    protected:
        result<T>* result_ptr{ nullptr };
    };

    template<>
    class packaged_awaitable_result_handler<void> {
    public:
        void return_void() {
            if (result_ptr) {
                result_ptr->set_value();
            }
        }

    protected:
        result<void>* result_ptr{ nullptr };
    };

    template<class T>
    class packaged_awaitable_promise
        : public packaged_awaitable_result_handler<T>
    {
    public:
        ~packaged_awaitable_promise() {
            if (handle_ptr) {
                *handle_ptr = nullptr;
            }
        }

        packaged_awaitable_coroutine<T> get_return_object() noexcept {
            return packaged_awaitable_coroutine<T>{ packaged_awaitable_handle<T>::from_promise(*this) };
        }

        auto initial_suspend() noexcept { return std::suspend_always{}; }
        auto final_suspend() noexcept { return std::suspend_never{}; }

        void unhandled_exception() noexcept {
            if (this->result_ptr) {
                this->result_ptr->set_exception(std::current_exception());
            }
        }

        void bind(result<T>* result_ptr, packaged_awaitable_handle<T>* handle_ptr) {
            this->result_ptr = result_ptr;
            this->handle_ptr = handle_ptr;
        }

        void unbind() {
            this->result_ptr = nullptr;
            this->handle_ptr = nullptr;
        }

    private:
        packaged_awaitable_handle<T>* handle_ptr{ nullptr };
    };

    template<class T>
    struct packaged_awaitable_coroutine {
        using promise_type = packaged_awaitable_promise<T>;

        packaged_awaitable_handle<T> handle;
    };

    template<class T, detail::awaitable Awaitable>
    packaged_awaitable_coroutine<T> make_packaged_awaitable(Awaitable awaitable) {
        co_return co_await std::move(awaitable);
    }

} // namespace coroactors::detail
