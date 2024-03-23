#pragma once
#include <coroactors/detail/config.h>
#include <coroactors/intrusive_ptr.h>
#include <coroactors/result.h>
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
        continuation_state() noexcept = default;

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
                        result_.set_exception(with_continuation_error("continuation was not resumed"));
                        symmetric::resume(
                            std::coroutine_handle<>::from_address(addr));
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
            if (result_) [[unlikely]] {
                throw with_continuation_error("continuation was resumed or destroyed already");
            }
            result_.set_value(std::forward<TArgs>(args)...);
        }

        template<class E>
        void set_exception(E&& e) {
            if (result_) [[unlikely]] {
                throw with_continuation_error("continuation was resumed or destroyed already");
            }
            result_.set_exception(std::forward<E>(e));
        }

        std::coroutine_handle<> finish() noexcept {
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

        T take_value() {
            return result_.take_value();
        }

        void cancel() noexcept {
            // Try to catch attempts to resume later
            if (!result_) {
                result_.set_exception(std::exception_ptr());
            }
            // Called by awaiter when it is destroyed without resuming
            // Tries to make sure it will not be resumed later, but it's up to
            // the user to make sure there is no race with both ends trying to
            // resume/destroy the same coroutine frame concurrently.
            // Note: we don't care if it was finished or destroyed before.
            continuation.store(reinterpret_cast<void*>(MarkerDestroyed), std::memory_order_release);
        }

    private:
        std::atomic<void*> continuation{ nullptr };
        result<T> result_;
    };

    template<class T, class Callback>
    class [[nodiscard]] with_continuation_awaiter {
        using status = typename continuation_state<T>::status;

    public:
        with_continuation_awaiter(Callback&& callback) noexcept
            : callback(std::forward<Callback>(callback))
        {}

        with_continuation_awaiter(const with_continuation_awaiter&) = delete;
        with_continuation_awaiter& operator=(const with_continuation_awaiter&) = delete;

        // Support moving awaiter by wrappers before awaiting
        with_continuation_awaiter(with_continuation_awaiter&& rhs) noexcept = default;

        ~with_continuation_awaiter() noexcept {
            if (state_) {
                state_->cancel();
            }
        }

        bool await_ready() {
            state_.reset(new continuation_state<T>());
            std::move(callback)(continuation<T>(state_));
            // Avoid suspending when the result is ready
            return state_->ready();
        }

        COROACTORS_AWAIT_SUSPEND
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
            auto state = std::move(state_);
            return state->take_value();
        }

    private:
        intrusive_ptr<continuation_state<T>> state_;
        std::decay_t<Callback> callback;
    };

} // namespace coroactors::detail
