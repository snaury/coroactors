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

        explicit continuation(detail::continuation_state<T>* state) noexcept
            : state(state)
        {
            state->init_strong();
        }

    public:
        continuation() noexcept
            : state(nullptr)
        {}

        continuation(const continuation& rhs) noexcept
            : state(rhs.state)
        {
            if (state) {
                state->add_strong_ref();
            }
        }

        continuation(continuation&& rhs) noexcept
            : state(std::exchange(rhs.state, nullptr))
        {}

        ~continuation() noexcept {
            if (state) {
                state->release_strong_ref();
            }
        }

        continuation& operator=(const continuation& rhs) noexcept {
            auto* prev = state;
            state = rhs.state;
            if (state) {
                state->add_strong_ref();
            }
            if (prev) {
                prev->release_strong_ref();
            }
            return *this;
        }

        continuation& operator=(continuation&& rhs) noexcept {
            if (this != &rhs) [[likely]] {
                auto* prev = state;
                state = rhs.state;
                rhs.state = nullptr;
                if (prev) {
                    prev->release_strong_ref();
                }
            }
            return *this;
        }

        explicit operator bool() const noexcept {
            return bool(state);
        }

        friend bool operator==(const continuation& a, const continuation& b) noexcept {
            return a.state == b.state;
        }

        void reset() noexcept {
            if (state) {
                state->release_strong_ref();
                state = nullptr;
            }
        }

        /**
         * Returns a coroutine handle that may be resumed manually with the
         * result constructed from args
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
         * Returns a coroutine handle that may be resumed manually and will
         * throw the provided exception
         */
        std::coroutine_handle<> release_with_exception(std::exception_ptr&& e) {
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
         * Resumes continuation by throwing an exception
         *
         * Returns true if suspended continuation was resumed
         * Returns false if continuation will continue without suspending
         */
        bool resume_with_exception(std::exception_ptr&& e) {
            state->set_exception(std::move(e));
            if (auto c = state->finish()) {
                c.resume();
                return true;
            } else {
                return false;
            }
        }

        /**
         * Returns the associated stop token
         */
        const stop_token& get_stop_token() const noexcept {
            return state->get_stop_token();
        }

    private:
        detail::continuation_state<T>* state;
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
