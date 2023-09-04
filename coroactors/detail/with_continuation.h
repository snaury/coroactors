#pragma once
#include <coroactors/intrusive_ptr.h>
#include <coroactors/result.h>
#include <coroactors/stop_token.h>
#include <coroactors/with_continuation_error.h>
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
    concept is_with_continuation_callback = requires(std::decay_t<Callback> callback) {
        { std::move(callback)(std::declval<continuation<T>>()) } -> std::same_as<void>;
    };

    template<class T>
    class continuation_state final : public intrusive_atomic_base<continuation_state<T>> {
    private:
        static constexpr uintptr_t MarkerFinished = 1;
        static constexpr uintptr_t MarkerDestroyed = 2;

    public:
        continuation_state(result<T>* result_ptr, stop_token&& token) noexcept
            : result_ptr(result_ptr)
            , token(std::move(token))
        {}

        continuation_state(const continuation_state&) = delete;
        continuation_state& operator=(const continuation_state&) = delete;

        bool destroy() noexcept {
            void* addr = continuation.load(std::memory_order_relaxed);
            for (;;) {
                if (addr == reinterpret_cast<void*>(MarkerFinished)) {
                    // finish() has been called already
                    return false;
                }
                if (addr == reinterpret_cast<void*>(MarkerDestroyed)) {
                    // destroy() has been called already
                    return false;
                }
                // Note: we need memory_order_acq_rel on success here, because
                // we need set_continuation to happen before the destruction,
                // but also we need everything else to happen before destroy
                // marker is possibly observed in an awaiter, where it would
                // call destructors.
                if (continuation.compare_exchange_weak(
                        addr, reinterpret_cast<void*>(MarkerDestroyed),
                        std::memory_order_acq_rel, std::memory_order_relaxed))
                {
                    if (addr) {
                        result_ptr->set_exception(with_continuation_error("continuation was not resumed"));
                        std::coroutine_handle<>::from_address(addr).resume();
                    }
                    return true;
                }
            }
        }

        bool ready() const noexcept {
            // We use memory_order_acquire to also synchronize with the result
            void* addr = continuation.load(std::memory_order_acquire);
            return addr == reinterpret_cast<void*>(MarkerFinished);
        }

        enum class status : uintptr_t {
            success = 0,
            finished = MarkerFinished,
            destroyed = MarkerDestroyed,
        };

        status set_continuation(std::coroutine_handle<> c) noexcept {
            void* addr = nullptr;
            // Note: this synchronizes with later resume (release), and also
            // synchronizes with possible finish or destroy calls.
            if (continuation.compare_exchange_strong(
                    addr, c.address(), std::memory_order_acq_rel))
            {
                return status::success;
            }
            assert(
                addr == reinterpret_cast<void*>(MarkerFinished) ||
                addr == reinterpret_cast<void*>(MarkerDestroyed));
            return static_cast<status>(reinterpret_cast<uintptr_t>(addr));
        }

        void destroy_explicit() {
            if (!destroy()) [[unlikely]] {
                throw with_continuation_error("continuation was resumed or destroyed already");
            }
        }

        template<class... TArgs>
        void set_value(TArgs&&... args) {
            if (!result_ptr) [[unlikely]] {
                throw with_continuation_error("continuation was resumed or destroyed already");
            }
            result_ptr->set_value(std::forward<TArgs>(args)...);
        }

        template<class E>
        void set_exception(E&& e) {
            if (!result_ptr) [[unlikely]] {
                throw with_continuation_error("continuation was resumed or destroyed already");
            }
            result_ptr->set_exception(std::forward<E>(e));
        }

        std::coroutine_handle<> finish() noexcept {
            // Try to catch attempts to resume again
            result_ptr = nullptr;
            // This synchronizes with acquire/release in set_continuation
            void* addr = continuation.exchange(
                reinterpret_cast<void*>(MarkerFinished),
                std::memory_order_acq_rel);
            assert(addr != reinterpret_cast<void*>(MarkerFinished));
            assert(addr != reinterpret_cast<void*>(MarkerDestroyed));
            if (addr) {
                return std::coroutine_handle<>::from_address(addr);
            } else {
                return {};
            }
        }

        void cancel() noexcept {
            // Try to catch attempts to resume later
            result_ptr = nullptr;
            // Called by awaiter when it is destroyed without resuming
            // Tries to make sure it will not be resumed later, but it's up to
            // the user to make sure there is no race with both ends trying to
            // resume/destroy the same coroutine frame concurrently.
            // Note: we don't care if it was finished or destroyed before.
            continuation.store(reinterpret_cast<void*>(MarkerDestroyed), std::memory_order_release);
        }

        const stop_token& get_stop_token() const noexcept {
            return token;
        }

    private:
        std::atomic<void*> continuation{ nullptr };
        result<T>* result_ptr;
        stop_token token;
    };

    template<class T, class Callback>
    class [[nodiscard]] with_continuation_awaiter
        : private result<T>
    {
        using status = typename continuation_state<T>::status;

    public:
        with_continuation_awaiter(Callback&& callback) noexcept
            : callback(std::forward<Callback>(callback))
        {}

        with_continuation_awaiter(const with_continuation_awaiter&) = delete;
        with_continuation_awaiter& operator=(const with_continuation_awaiter&) = delete;

        // Awaiter may be moved by some wrappers, but only before the await
        // starts, so all we have to "move" is a reference to the callback
        with_continuation_awaiter(with_continuation_awaiter&& rhs) noexcept
            : callback(std::move(rhs.callback))
        {}

        ~with_continuation_awaiter() noexcept {
            if (state_) {
                state_->cancel();
            }
        }

        bool await_ready(stop_token token = {}) {
            state_.reset(new continuation_state<T>(
                static_cast<result<T>*>(this), std::move(token)));
            std::move(callback)(continuation<T>(state_));
            // Avoid suspending when the result is ready
            return state_->ready();
        }

        __attribute__((__noinline__))
        bool await_suspend(std::coroutine_handle<> c) {
            switch (state_->set_continuation(c)) {
            case status::success:
                // Continuation is set and this coroutine will be resumed or
                // destroyed eventually, possibly concurrently with us in
                // another thread.
                return true;

            case status::finished:
                // Continuation was resumed concurrently in another thread
                return false;

            case status::destroyed:
            default:
                // All continuation object references have been destroyed
                // before we could set the continuation.
                throw with_continuation_error("continuation was not resumed");
            }
        }

        T await_resume() {
            state_.reset();
            return this->take_value();
        }

    private:
        intrusive_ptr<continuation_state<T>> state_;
        std::decay_t<Callback> callback;
    };

} // namespace coroactors::detail
