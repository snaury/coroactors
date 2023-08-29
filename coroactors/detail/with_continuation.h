#pragma once
#include <coroactors/stop_token.h>
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

    template<class Callback, class T>
    concept is_with_continuation_callback = requires(Callback& callback) {
        { std::forward<Callback>(callback)(std::declval<continuation<T>>()) } -> std::same_as<void>;
    };

    template<class T>
    class continuation_result {
    public:
        continuation_result() = default;

        template<class... TArgs>
        void set_value(TArgs&&... args) {
            result_.template emplace<1>(std::forward<TArgs>(args)...);
            initialized_.store(true, std::memory_order_release);
        }

        void set_exception(std::exception_ptr&& e) noexcept {
            result_.template emplace<2>(std::move(e));
            initialized_.store(true, std::memory_order_release);
        }

        bool has_result() const noexcept {
            // Note: this is relaxed because it is only an observer of whether
            // set_value/set_exception was called before the last shared_ptr
            // reference was dropped. It's important however that it's not
            // stale when weak_ptr::lock returns nullptr, otherwise we may
            // decide set_value/set_exception have never been called. Now to
            // guarantee safe destruction refcount decrements usually have
            // memory_order_acq_rel, so the store to initialized_ must have
            // happened before weak_ptr::lock returns nullptr, and thus no
            // stale value should ever be observed.
            return initialized_.load(std::memory_order_relaxed) == true;
        }

        T take_value() {
            // This synchronizes with release in set_value/set_exception
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
            throw std::logic_error("unexpected state in take_value");
        }

    private:
        struct Void {};
        using Result = std::conditional_t<std::is_void_v<T>, Void, T>;
        std::variant<std::monostate, Result, std::exception_ptr> result_;
        std::atomic<bool> initialized_{ false };
    };

    template<class T>
    class continuation_state {
    public:
        continuation_state(continuation_result<T>* result, stop_token&& token) noexcept
            : result(result)
            , token(std::move(token))
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
        void set_value(TArgs&&... args) {
            result->set_value(std::forward<TArgs>(args)...);
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

        const stop_token& get_stop_token() const noexcept {
            return token;
        }

    private:
        static constexpr uintptr_t MarkerFinished = 1;
        static constexpr uintptr_t MarkerDestroyed = 2;

    private:
        continuation_result<T>* const result;
        std::atomic<void*> continuation{ nullptr };
        stop_token token;
    };

    template<class T, class Callback>
    class [[nodiscard]] with_continuation_awaiter
        : private continuation_result<T>
    {
    public:
        with_continuation_awaiter(Callback& callback) noexcept
            : callback(callback)
        {}

        with_continuation_awaiter(const with_continuation_awaiter&) = delete;
        with_continuation_awaiter& operator=(const with_continuation_awaiter&) = delete;

        // Awaiter may be moved by some wrappers, but only before the await
        // starts, so all we have to "move" is a reference to the callback
        with_continuation_awaiter(with_continuation_awaiter&& rhs) noexcept
            : callback(rhs.callback)
        {}

        ~with_continuation_awaiter() noexcept {
            if (auto state = state_.lock()) {
                state->cancel();
            }
        }

        bool await_ready(stop_token token = {}) noexcept {
            auto state = std::make_shared<continuation_state<T>>(
                static_cast<continuation_result<T>*>(this), std::move(token));
            state_ = state;
            std::forward<Callback>(callback)(continuation<T>(state));
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
            return this->take_value();
        }

    private:
        std::weak_ptr<continuation_state<T>> state_;
        Callback& callback;
    };

} // namespace coroactors::detail
