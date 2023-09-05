#pragma once
#include <coroactors/detail/config.h>
#include <cassert>
#include <cstddef>
#include <coroutine>

namespace coroactors::detail {

#if COROACTORS_USE_SYMMETRIC_TRANSFER
    class symmetric {
    public:
        using result_t = std::coroutine_handle<>;

        static void resume(std::coroutine_handle<> h) {
            h.resume();
        }

        static result_t noop() noexcept {
            return std::noop_coroutine();
        }

        static result_t self(std::coroutine_handle<> h) noexcept {
            return h;
        }

        static result_t transfer(std::coroutine_handle<> h) noexcept {
            return h;
        }

        static std::coroutine_handle<> intercept(bool suspended) noexcept {
            if (suspended) {
                return std::noop_coroutine();
            } else {
                return nullptr;
            }
        }

        static std::coroutine_handle<> intercept(std::coroutine_handle<> h) noexcept {
            return h;
        }
    };
#else
    class symmetric {
    public:
        /**
         * The return type from await_suspend to support symmetric transfer
         */
        using result_t = bool;

        /**
         * Use instead of h.resume() to avoid recursive resume on transfers
         */
        static void resume(std::coroutine_handle<> h) {
            running_guard guard;
            for (;;) {
                h.resume();
                if (!continuation) {
                    break;
                }
                h = std::coroutine_handle<>::from_address(continuation);
                continuation = nullptr;
            }
        }

        /**
         * Return this instead of `return std::noop_coroutine()`
         *
         * Usually causes the coroutine stack to suspend and return to caller
         */
        static result_t noop() noexcept {
            assert(!continuation && "Transferring to a noop coroutine with another transfer in progress");
            return true;
        }

        /**
         * Return this instead of `return c`, where `c` is the initial argument
         *
         * Usually causes the suspended coroutine to resume without recursion
         */
        static result_t self(std::coroutine_handle<>) noexcept {
            assert(!continuation && "Transferring to a self coroutine with another transfer in progress");
            return false;
        }

        /**
         * Return this instead of `return c`, where `c` is a continuation
         */
        static result_t transfer(std::coroutine_handle<> h) noexcept {
            assert(h && "Transferring to a nullptr coroutine");
            if (running > 0) [[likely]] {
                assert(!continuation && "Transferring to a coroutine with another transfer in progress");
                continuation = h.address();
            } else {
                resume(h);
            }
            return true;
        }

        /**
         * Use `auto next = intercept(await_suspend(h))`, when you want to
         * intercept transfers from upstream awaiters. Returns nullptr when
         * the original coroutine `h` should resume, or a valid coroutine
         * handle of the transfer, which may include `noop_coroutine`.
         */
        static std::coroutine_handle<> intercept(bool suspended) noexcept {
            if (suspended) {
                if (continuation) {
                    std::coroutine_handle<> h = std::coroutine_handle<>::from_address(continuation);
                    continuation = nullptr;
                    return h;
                } else {
                    return std::noop_coroutine();
                }
            } else {
                return nullptr;
            }
        }

        /**
         * Use `auto next = intercept(await_suspend(h))`, when you want to
         * intercept transfers from upstream awaiters. Returns nullptr when
         * the original coroutine `h` should resume, or a valid coroutine
         * handle of the transfer, which may include `noop_coroutine`.
         */
        static std::coroutine_handle<> intercept(std::coroutine_handle<> h) noexcept {
            return h;
        }

    private:
        struct running_guard {
            void* saved_continuation;

            running_guard() noexcept {
                saved_continuation = continuation;
                continuation = nullptr;
                ++running;
            }

            ~running_guard() noexcept {
                --running;
                assert(!continuation && "Symmetric transfer did not resume a continuation");
                continuation = saved_continuation;
            }

            running_guard(const running_guard&) = delete;
            running_guard& operator=(const running_guard&) = delete;
        };

    private:
        static inline thread_local void* continuation{ nullptr };
        static inline thread_local size_t running{ 0 };
    };
#endif

} // namespace coroactors::detail
