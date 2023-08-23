#pragma once
#include <coroactors/detail/with_continuation.h>

namespace coroactors {

    /**
     * A type-safe resumable continuation of an arbitrary coroutine
     */
    template<class T = void>
    class continuation {
        template<class U, class Callback>
        friend class detail::with_continuation_awaiter;

        continuation(const std::shared_ptr<detail::continuation_state<T>>& state)
            : state(std::move(state))
        {}

    public:
        continuation() noexcept = default;

        explicit operator bool() const noexcept {
            return bool(state);
        }

        void reset() noexcept {
            state.reset();
        }

        /**
         * Returns a coroutine handle that may be resumed manually with the
         * result constructed from args
         */
        template<class... TArgs>
        std::coroutine_handle<> release_with_result(TArgs&&... args)
            noexcept(std::is_void_v<T>)
        {
            state->set_result(std::forward<TArgs>(args)...);
            if (auto c = state->finish()) {
                return c;
            } else {
                return std::noop_coroutine();
            }
        }

        /**
         * Returns a coroutine handle that may be resumed manually by throwing
         * an exception
         */
        std::coroutine_handle<> release_with_exception(std::exception_ptr&& e) noexcept {
            state->set_exception(std::move(e));
            if (auto c = state->finish()) {
                return c;
            } else {
                return std::noop_coroutine();
            }
        }

        /**
         * Resumes continuation with the result constructed from args
         *
         * Returns true if suspended continuation was resumed
         * Returns false if continuation will continue without suspending
         */
        template<class... TArgs>
        bool resume(TArgs&&... args)
            noexcept(std::is_void_v<T>)
        {
            state->set_result(std::forward<TArgs>(args)...);
            if (auto c = state->finish()) {
                c.resume();
                return true;
            } else {
                return false;
            }
        }

        /**
         * Resumes continuation by throwing an exception
         *
         * Returns true if suspended continuation was resumed
         * Returns false if continuation will continue without suspending
         */
        bool resume_with_exception(std::exception_ptr&& e) noexcept {
            state->set_exception(std::move(e));
            if (auto c = state->finish()) {
                c.resume();
                return true;
            } else {
                return false;
            }
        }

    private:
        // Note: awaiter only keeps a weak reference to this state
        std::shared_ptr<detail::continuation_state<T>> state;
    };

    /**
     * Provides arbitrary suspension points for coroutines
     *
     * When the result is co_awaited (which must happen in the same expression
     * as the call to with_continuation) callback will be called with some
     * std::coroutine_handle<> that will resume the awaiting coroutine. The
     * awaiting coroutine might not suspend if the provided handle is resumed
     * before the callback returns.
     */
    template<class T, class Callback>
    detail::with_continuation_awaiter<T, Callback>
    with_continuation(Callback&& callback) noexcept
        requires (detail::is_with_continuation_callback<Callback, T>)
    {
        return detail::with_continuation_awaiter<T, Callback>(callback);
    }

    template<class Callback>
    detail::with_continuation_awaiter<void, Callback>
    with_continuation(Callback&& callback) noexcept
        requires (detail::is_with_continuation_callback<Callback, void>)
    {
        return detail::with_continuation_awaiter<void, Callback>(callback);
    }

} // namespace coroactors
