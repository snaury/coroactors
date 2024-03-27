#pragma once
#include <coroactors/detail/config.h>
#include <cassert>
#include <cstddef>
#include <coroutine>

namespace coroactors::detail {

    class symmetric_native {
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

    class symmetric_emulated {
    public:
        /**
         * The return type from await_suspend to support symmetric transfer
         */
        using result_t = bool;

        /**
         * Use instead of h.resume() to avoid recursive resume on transfers
         */
        static void resume(std::coroutine_handle<> h) {
            std::coroutine_handle<> continuation;
            running_guard guard(continuation);
            for (;;) {
                h.resume();
                if (!continuation) {
                    break;
                }
                h = continuation;
                continuation = nullptr;
            }
        }

        /**
         * Return this instead of `return std::noop_coroutine()`
         *
         * Usually causes the coroutine stack to suspend and return to caller
         */
        static result_t noop() noexcept {
            assert((!current_loop || !*current_loop) &&
                    "Transferring to a noop coroutine with another transfer in progress");
            return true;
        }

        /**
         * Return this instead of `return c`, where `c` is the initial argument
         *
         * Usually causes the suspended coroutine to resume without recursion
         */
        static result_t self(std::coroutine_handle<>) noexcept {
            assert((!current_loop || !*current_loop) &&
                    "Transferring to a self coroutine with another transfer in progress");
            return false;
        }

        /**
         * Return this instead of `return c`, where `c` is a continuation
         */
        static result_t transfer(std::coroutine_handle<> h) noexcept {
            assert(h && "Transferring to a nullptr coroutine");
            if (auto* loop = current_loop) [[likely]] {
                assert(!*loop && "Transferring to a coroutine with another transfer in progress");
                *loop = h;
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
                auto* loop = current_loop;
                if (loop && *loop) {
                    std::coroutine_handle<> h = *loop;
                    *loop = nullptr;
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
            std::coroutine_handle<>* const saved_loop;

            running_guard(std::coroutine_handle<>& handle) noexcept
                : saved_loop(current_loop)
            {
                current_loop = &handle;
            }

            ~running_guard() noexcept {
                assert(!*current_loop && "Symmetric transfer did not resume a continuation");
                current_loop = saved_loop;
            }

            running_guard(const running_guard&) = delete;
            running_guard& operator=(const running_guard&) = delete;
        };

    private:
        static inline thread_local std::coroutine_handle<>* current_loop{ nullptr };
    };

#if COROACTORS_USE_SYMMETRIC_TRANSFER
    using symmetric = symmetric_native;
#else
    using symmetric = symmetric_emulated;
#endif

} // namespace coroactors::detail
