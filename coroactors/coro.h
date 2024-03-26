#pragma once
#include <coroactors/detail/coro.h>
#include <coroactors/result.h>
#include <utility>

namespace coroactors {

    /**
     * An extremely bare-bones coroutine class
     *
     * This coroutine is intentially very simple and not integrated into the
     * rest of the library, making it easier to test interaction with non-async
     * coroutines.
     */
    template<class T>
    class [[nodiscard]] coro {
        friend detail::coro_promise<T>;

    private:
        explicit coro(detail::coro_handle<T> handle)
            : handle(handle)
        {}

    public:
        using promise_type = detail::coro_promise<T>;
        using value_type = T;

    public:
        coro() noexcept = default;

        coro(coro&& rhs) noexcept
            : handle(std::exchange(rhs.handle, {}))
        {}

        ~coro() noexcept {
            if (handle) {
                handle.destroy();
            }
        }

        coro& operator=(coro&& rhs) noexcept {
            if (this != &rhs) {
                auto prev = handle;
                handle = rhs.handle;
                rhs.handle = {};
                if (prev) {
                    prev.destroy();
                }
            }
            return *this;
        }

        explicit operator bool() const noexcept {
            return bool(handle);
        }

        bool await_ready() noexcept {
            return handle.promise().ready();
        }

        std::coroutine_handle<> await_suspend(std::coroutine_handle<> c) {
            handle.promise().set_continuation(c);
            return handle;
        }

        T await_resume() & {
            return handle.promise().get_result();
        }

        T await_resume() && {
            return handle.promise().take_result();
        }

    private:
        detail::coro_handle<T> handle;
    };

} // namespace coroactors
