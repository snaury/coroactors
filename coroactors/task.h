#pragma once
#include <coroactors/detail/task.h>
#include <coroactors/result.h>
#include <utility>

namespace coroactors {

    /**
     * A simple lazily started coroutine task
     *
     * The advantage of this class over an actor without a context is a heap
     * allocation elision for some simple cases. The disadvantage is lack of
     * support for stop token propagation or actor related functions.
     */
    template<class T>
    class task {
        friend detail::task_promise<T>;

    private:
        explicit task(detail::task_continuation<T> handle)
            : handle(handle)
        {}

    public:
        using promise_type = detail::task_promise<T>;
        using value_type = T;

    public:
        task() noexcept = default;

        task(task&& rhs) noexcept
            : handle(std::exchange(rhs.handle, {}))
        {}

        ~task() noexcept {
            if (handle) {
                handle.destroy();
            }
        }

        task& operator=(task&& rhs) noexcept {
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

        std::coroutine_handle<> await_suspend(std::coroutine_handle<> h) {
            handle.promise().set_continuation(h);
            return handle;
        }

        T await_resume() {
            return handle.promise().take_result();
        }

    private:
        detail::task_continuation<T> handle;
    };

} // namespace coroactors
