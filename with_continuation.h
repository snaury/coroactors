#pragma once
#include <atomic>
#include <coroutine>
#include <utility>

namespace coroactors::detail {

    template<class TCallback>
    concept is_with_continuation_callback = requires(TCallback&& callback, std::coroutine_handle<> c) {
        { ((TCallback&&) callback)(std::move(c)) } -> std::same_as<void>;
    };

    template<class TCallback>
    class with_continuation_coroutine;

    template<class TCallback>
    class with_continuation_promise;

    template<class TCallback>
    using with_continuation_handle = std::coroutine_handle<with_continuation_promise<TCallback>>;

    template<class TCallback>
    class with_continuation_promise {
    public:
        with_continuation_promise(TCallback& callback)
            : callback(callback)
        {}

        [[nodiscard]] with_continuation_coroutine<TCallback> get_return_object() noexcept {
            return with_continuation_handle<TCallback>::from_promise(*this);
        }

        auto initial_suspend() noexcept { return std::suspend_always{}; }

        struct TFinalSuspend {
            bool await_ready() noexcept { return false; }

            __attribute__((__noinline__))
            std::coroutine_handle<> await_suspend(with_continuation_handle<TCallback> c) noexcept {
                auto& self = c.promise();
                void* addr = self.continuation.exchange(
                    reinterpret_cast<void*>(MarkerFinished), std::memory_order_acq_rel);
                if (addr) {
                    c.destroy();
                    return std::coroutine_handle<>::from_address(addr);
                }
                return std::noop_coroutine();
            }

            void await_resume() noexcept {}
        };

        auto final_suspend() noexcept { return TFinalSuspend{}; }

        bool run_callback(std::coroutine_handle<> c) {
            std::forward<TCallback>(callback)(std::move(c));
            return continuation.load(std::memory_order_acquire) == reinterpret_cast<void*>(MarkerFinished);
        }

        bool set_continuation(std::coroutine_handle<> c) {
            // Note: acquire/release synchronizes with concurrent resume in another thread
            void* expected = nullptr;
            return continuation.compare_exchange_strong(expected, c.address(), std::memory_order_acq_rel);
        }

        void unhandled_exception() noexcept {
            // our coroutine never throws exceptions
        }

        void return_void() noexcept {
            // nothing
        }

    private:
        static constexpr uintptr_t MarkerFinished = 1;

    private:
        TCallback& callback;
        std::atomic<void*> continuation{ nullptr };
    };

    template<class TCallback>
    class with_continuation_coroutine {
        friend class with_continuation_promise<TCallback>;

        with_continuation_coroutine(with_continuation_handle<TCallback> handle) noexcept
            : handle(handle)
        {}

    public:
        using promise_type = with_continuation_promise<TCallback>;

        with_continuation_coroutine(with_continuation_coroutine&& rhs) noexcept
            : handle(std::exchange(rhs.handle, {}))
            , suspended(rhs.suspended)
        {}

        ~with_continuation_coroutine() {
            if (handle && !suspended) {
                handle.destroy();
            }
        }

        with_continuation_coroutine& operator=(const with_continuation_coroutine&) = delete;

        bool await_ready() {
            if (handle && !suspended) {
                return handle.promise().run_callback(handle);
            } else {
                return true;
            }
        }

        __attribute__((__noinline__))
        bool await_suspend(std::coroutine_handle<> c) noexcept {
            suspended = true;
            if (handle.promise().set_continuation(c)) {
                return true;
            }
            suspended = false;
            return false;
        }

        void await_resume() noexcept {
            // caller resumed
        }

    private:
        with_continuation_handle<TCallback> handle;
        bool suspended = false;
    };

} // namespace coroactors::detail

namespace coroactors {

    /**
     * Provides arbitrary suspension points to coroutines
     *
     * When the result is co_awaited (which must happen in the same expression
     * as the call to with_continuation) callback will be called with some
     * std::coroutine_handle<> that will resume the awaiting coroutine. The
     * awaiting coroutine might not suspend if the provided handle is resumed
     * before the callback returns.
     */
    template<class TCallback>
    detail::with_continuation_coroutine<TCallback>
    with_continuation(TCallback&& callback)
        requires (detail::is_with_continuation_callback<TCallback>)
    {
        // Note: coroutine binds to the argument and calls callback when awaited
        (void)callback;
        co_return;
    }

} // namespace coroactors
