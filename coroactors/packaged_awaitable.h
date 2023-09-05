#pragma once
#include <coroactors/detail/packaged_awaitable.h>
#include <coroactors/detail/symmetric_transfer.h>
#include <utility>

namespace coroactors {

    /**
     * This class starts an internal coroutine with `co_await awaitable` on
     * construction, and destroys that coroutine on destruction unless it has
     * completed already, performing bottom-up destruction of the coroutine
     * stack. This class also holds the result of the awaitable, which is
     * avaiable when awaitable either returns with a value or an exception.
     *
     * This class is not thread-safe and cannot be safely used with awaitables
     * that may complete concurrently in another thread. The primary use case
     * is for testing and running coroutines in a controlled way.
     */
    template<class T>
    class packaged_awaitable : public result<T> {
    public:
        using result_type = result<T>;
        using value_type = T;

        template<detail::awaitable Awaitable>
        explicit packaged_awaitable(Awaitable&& awaitable)
            : handle(detail::make_packaged_awaitable<T>(std::forward<Awaitable>(awaitable)).handle)
        {
            handle.promise().bind(this, &handle);
            detail::symmetric::resume(handle);
        }

        packaged_awaitable(const packaged_awaitable&) = delete;
        packaged_awaitable& operator=(const packaged_awaitable&) = delete;

        packaged_awaitable(packaged_awaitable&& rhs) noexcept
            : result_type(static_cast<result_type&&>(rhs))
            , handle(std::exchange(rhs.handle, {}))
        {
            if (handle) {
                handle.promise().bind(this, &handle);
            }
        }

        ~packaged_awaitable() noexcept {
            if (handle) {
                handle.destroy();
                assert(!handle);
            }
        }

        /**
         * Returns true when the awaitable is still running
         *
         * It is possible for both the result and running() to be false, either
         * because the packaged awaitable was moved from, or because coroutine
         * was destroyed without finishing.
         */
        bool running() const noexcept {
            return bool(handle);
        }

        /**
         * Returns true when the awaitable finished normally and without
         * throwing an exception. This is an alias to `has_value` for
         * convenience when writing tests.
         */
        bool success() const noexcept {
            return this->has_value();
        }

        /**
         * Releases a currently running coroutine handle
         */
        std::coroutine_handle<> release() noexcept {
            if (handle) {
                handle.promise().unbind();
            }
            return std::exchange(handle, {});
        }

        /**
         * Destroys a currently running coroutine
         */
        void destroy() noexcept {
            if (handle) {
                handle.destroy();
                assert(!handle);
            }
        }

        std::add_lvalue_reference_t<T> operator*() { return this->get_value(); }
        std::add_lvalue_reference_t<const T> operator*() const { return this->get_value(); }

        T* operator->() requires (!std::is_void_v<T>) { return &this->get_value(); }
        const T* operator->() const requires (!std::is_void_v<T>) { return &this->get_value(); }

    private:
        template<class... Args>
        void set_value(Args&&...) = delete;

        template<class... Args>
        void emplace_value(Args&&...) = delete;

        template<class... Args>
        void set_exception(Args&&...) = delete;

    private:
        detail::packaged_awaitable_handle<T> handle;
    };

    template<class Awaitable>
    packaged_awaitable(Awaitable&&) -> packaged_awaitable<std::decay_t<detail::await_result_t<Awaitable>>>;

} // namespace coroactors
