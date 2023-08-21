#pragma once
#include <atomic>
#include <cassert>
#include <coroutine>
#include <memory>
#include <utility>
#include <variant>

namespace coroactors {

    template<class T>
    class continuation;

} // namespace coroactors

namespace coroactors::detail {

    template<class TCallback, class T>
    concept is_with_continuation_callback = requires(TCallback&& callback, continuation<T> c) {
        { ((TCallback&&) callback)(std::move(c)) } -> std::same_as<void>;
    };

    template<class T>
    class continuation_result {
    public:
        continuation_result() = default;

        template<class... TArgs>
        void set_result(TArgs&&... args) {
            result_.template emplace<1>(std::forward<TArgs>(args)...);
            initialized_.store(true, std::memory_order_release);
        }

        void set_exception(std::exception_ptr&& e) noexcept {
            result_.template emplace<2>(std::move(e));
            initialized_.store(true, std::memory_order_release);
        }

        bool has_result() const noexcept {
            return initialized_.load(std::memory_order_acquire) == true;
        }

        std::add_rvalue_reference_t<T> take_result() {
            // This synchronizes with release in set_result/set_exception
            if (initialized_.load(std::memory_order_acquire)) {
                switch (result_.index()) {
                case 1:
                    if constexpr (!std::is_void_v<T>) {
                        return std::get<1>(std::move(result_));
                    } else {
                        return;
                    }
                case 2:
                    std::rethrow_exception(std::get<2>(std::move(result_)));
                }
            }
            assert(false && "Unexpected state in take_result");
            throw std::logic_error("unexpected state in take_result");
        }

    private:
        struct TVoid {};
        using TValue = std::conditional_t<std::is_void_v<T>, TVoid, T>;
        std::variant<std::monostate, TValue, std::exception_ptr> result_;
        std::atomic<bool> initialized_{ false };
    };

    template<class T>
    class continuation_state {
    public:
        continuation_state(continuation_result<T>* result) noexcept
            : result(result)
        {}

        ~continuation_state() noexcept {
            // This synchronizes with release in set_continuation
            void* addr = continuation.exchange(
                reinterpret_cast<void*>(MarkerDestroyed),
                std::memory_order_acquire);
            assert(addr != reinterpret_cast<void*>(MarkerDestroyed));
            if (addr && addr != reinterpret_cast<void*>(MarkerFinished)) {
                // Continuation was not resumed and this is the last reference
                // Propagate this as an awaiting coroutine frame destruction
                std::coroutine_handle<>::from_address(addr).destroy();
            }
        }

        continuation_state(const continuation_state&) = delete;
        continuation_state& operator=(const continuation_state&) = delete;

        bool set_continuation(std::coroutine_handle<> c) noexcept {
            void* addr = nullptr;
            // Note: this synchronizes with later resume or destroy
            // This does not synchronize with the result of resume in any way
            return continuation.compare_exchange_strong(addr, c.address(), std::memory_order_release);
        }

        template<class... TArgs>
        void set_result(TArgs&&... args) {
            result->set_result(std::forward<TArgs>(args)...);
        }

        void set_exception(std::exception_ptr&& e) noexcept {
            result->set_exception(std::move(e));
        }

        std::coroutine_handle<> finish() noexcept {
            // This synchronizes with release in set_continuation
            void* addr = continuation.exchange(
                reinterpret_cast<void*>(MarkerFinished),
                std::memory_order_acquire);
            assert(addr != reinterpret_cast<void*>(MarkerFinished));
            assert(addr != reinterpret_cast<void*>(MarkerDestroyed));
            if (addr) {
                return std::coroutine_handle<>::from_address(addr);
            } else {
                return {};
            }
        }

        void cancel() noexcept {
            // Called by awaiter when it is destroyed without resuming
            // Tries to make sure it will not be resumed or destroyed later,
            // but it's up to the user to make sure there is no race with both
            // ends trying to resume/destroy the same coroutine frame.
            continuation.store(nullptr, std::memory_order_release);
        }

    private:
        static constexpr uintptr_t MarkerFinished = 1;
        static constexpr uintptr_t MarkerDestroyed = 2;

    private:
        continuation_result<T>* const result;
        std::atomic<void*> continuation{ nullptr };
    };

    template<class T, class TCallback>
    class with_continuation_awaiter
        : private continuation_result<T>
    {
    public:
        with_continuation_awaiter(TCallback& callback) noexcept
            : callback(callback)
        {}

        // Awaiter should only ever be moved before await starts
        // So all we have to move is reference to the callback
        with_continuation_awaiter(with_continuation_awaiter&& rhs) noexcept
            : callback(rhs.callback)
        {}

        with_continuation_awaiter& operator=(const with_continuation_awaiter&) = delete;

        ~with_continuation_awaiter() noexcept {
            if (auto state = state_.lock()) {
                state->cancel();
            }
        }

        bool await_ready() noexcept {
            auto state = std::make_shared<continuation_state<T>>(
                static_cast<continuation_result<T>*>(this));
            state_ = state;
            std::forward<TCallback>(callback)(continuation<T>(state));
            // Avoid suspending when continuation has the result already
            return this->has_result();
        }

        __attribute__((__noinline__))
        bool await_suspend(std::coroutine_handle<> c) noexcept {
            if (auto state = state_.lock()) {
                if (!state->set_continuation(c)) {
                    // Continuation resumed concurrently in another thread
                    // Note: there is no result synchronization yet
                    return false;
                }

                // Continuation is set and this coroutine will be resumed or
                // destroyed eventually, possibly concurrently with us in
                // another thread. There's also an edge case where the last
                // reference is dropped while we are holding on to the state,
                // but state destructor will then destroy our own coroutine
                // frame, which should be ok since we are suspended.
                return true;
            }

            // Continuation object is already destroyed, which could happen
            // just after it was resumed with some result. We should resume.
            if (this->has_result()) {
                return false;
            }

            // The last reference was dropped without resuming, so we need to
            // destroy our own coroutine frame.
            c.destroy();
            return true;
        }

        T await_resume() {
            state_.reset();
            return this->take_result();
        }

    private:
        std::weak_ptr<continuation_state<T>> state_;
        TCallback& callback;
    };

} // namespace coroactors::detail

namespace coroactors {

    /**
     * A type-safe resumable continuation of an arbitrary coroutine
     */
    template<class T = void>
    class continuation {
        template<class TResult, class TCallback>
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
    template<class T, class TCallback>
    detail::with_continuation_awaiter<T, TCallback>
    with_continuation(TCallback&& callback) noexcept
        requires (detail::is_with_continuation_callback<TCallback, T>)
    {
        return detail::with_continuation_awaiter<T, TCallback>(callback);
    }

    template<class TCallback>
    detail::with_continuation_awaiter<void, TCallback>
    with_continuation(TCallback&& callback) noexcept
        requires (detail::is_with_continuation_callback<TCallback, void>)
    {
        return detail::with_continuation_awaiter<void, TCallback>(callback);
    }

} // namespace coroactors
