#pragma once
#include <coroactors/detail/with_continuation.h>
#include <coroactors/with_continuation_error.h>

namespace coroactors {

    /**
     * A type-safe resumable continuation over an arbitrary coroutine
     */
    template<class T = void>
    class continuation {
        template<class U>
        friend class detail::with_continuation_awaiter;

        explicit continuation(const intrusive_ptr<detail::continuation_state<T>>& state) noexcept
            : state(state)
        {
            assert(state);
        }

    public:
        continuation() noexcept
            : state()
        {}

        continuation(continuation&& rhs) noexcept
            : state(std::move(rhs.state))
        {
            assert(!rhs.state);
        }

        ~continuation() noexcept {
            if (state) {
                state->destroy();
            }
        }

        continuation& operator=(continuation&& rhs) noexcept {
            if (this != &rhs) [[likely]] {
                auto prev = std::move(state);
                state = std::move(rhs.state);
                assert(!rhs.state);
                if (prev) {
                    prev->destroy();
                }
            }
            return *this;
        }

        explicit operator bool() const noexcept {
            return bool(state);
        }

        friend bool operator<(const continuation& a, const continuation& b) noexcept {
            return a.state < b.state;
        }

        friend bool operator==(const continuation& a, const continuation& b) noexcept {
            return a.state == b.state;
        }

        /**
         * Destroy a continuation without resuming
         *
         * Note: the awaiting coroutine frame will not be destroyed however
         * and a `with_continuation_error` exception will be thrown instead.
         */
        void destroy() {
            if (state) {
                state->destroy_explicit();
            }
        }

        /**
         * Returns a coroutine handle that may be resumed manually
         *
         * The result is constructed from args.
         */
        template<class... TArgs>
        std::coroutine_handle<> release_with_result(TArgs&&... args) {
            state->set_value(std::forward<TArgs>(args)...);
            if (auto c = state->finish()) {
                return c;
            } else {
                return std::noop_coroutine();
            }
        }

        /**
         * Returns a coroutine handle that may be resumed manually
         *
         * Throws the provided exception or std::exception_ptr when resumed.
         */
        template<class E>
        std::coroutine_handle<> release_with_exception(E&& e) {
            state->set_exception(std::forward<E>(e));
            if (auto c = state->finish()) {
                return c;
            } else {
                return std::noop_coroutine();
            }
        }

        /**
         * Resumes a continuation with the result constructed from args
         *
         * Returns true when a suspended continuation is resumed
         * Returns false when a continuation is not going to suspend
         */
        template<class... TArgs>
        bool resume(TArgs&&... args) {
            state->set_value(std::forward<TArgs>(args)...);
            if (auto c = state->finish()) {
                c.resume();
                return true;
            } else {
                return false;
            }
        }

        /**
         * Resumes a continuation by throwing the provided exception
         *
         * Returns true when a suspended continuation is resumed
         * Returns false when a continuation is not going to suspend
         */
        template<class E>
        bool resume_with_exception(E&& e) {
            state->set_exception(std::forward<E>(e));
            if (auto c = state->finish()) {
                c.resume();
                return true;
            } else {
                return false;
            }
        }

    private:
        intrusive_ptr<detail::continuation_state<T>> state;
    };

    /**
     * Provides arbitrary suspension points for coroutines
     *
     * The provided callback is called with a continuation<T> object that can
     * be used to resume the awaiting coroutine. The coroutine will not suspend
     * when the provided object is resumed before the callback returns.
     */
    template<class T, class Callback>
    detail::with_continuation_awaiter<T>
    with_continuation(Callback&& callback)
        requires (detail::is_with_continuation_callback<Callback, T>)
    {
        return detail::with_continuation_awaiter<T>(std::forward<Callback>(callback));
    }

    /**
     * Provides arbitrary suspension points for coroutines
     *
     * The provided callback is called with a continuation<void> object that can
     * be used to resume the awaiting coroutine. The coroutine will not suspend
     * when the provided object is resumed before the callback returns.
     */
    template<class Callback>
    detail::with_continuation_awaiter<void>
    with_continuation(Callback&& callback)
        requires (detail::is_with_continuation_callback<Callback, void>)
    {
        return detail::with_continuation_awaiter<void>(std::forward<Callback>(callback));
    }

} // namespace coroactors
