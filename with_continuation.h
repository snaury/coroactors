#pragma once
#include <atomic>
#include <cassert>
#include <coroutine>
#include <memory>
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

    class with_continuation_state {
    public:
        // Called by promise destructor exactly once.
        // Returns currently set continuation if any.
        std::coroutine_handle<> destroy() noexcept {
            void* addr = continuation.exchange(
                reinterpret_cast<void*>(MarkerDestroyed), std::memory_order_acq_rel);
            assert(addr != reinterpret_cast<void*>(MarkerDestroyed));
            if (addr && addr != reinterpret_cast<void*>(MarkerFinished)) {
                // We have a continuation, but promise is destroyed before
                // coroutine has finished. This means it was destroyed while
                // suspended and all we can do is destroy continuation as well.
                return std::coroutine_handle<>::from_address(addr);
            } else {
                return {};
            }
        }

        enum class status {
            set,
            finished,
            destroyed,
        };

        // Called by awaiter when it suspends
        // The continuation may have already been finished or destroyed
        status set_continuation(std::coroutine_handle<> c) noexcept {
            void* addr = nullptr;
            if (continuation.compare_exchange_strong(addr, c.address(), std::memory_order_acq_rel)) {
                return status::set;
            }
            if (addr == reinterpret_cast<void*>(MarkerFinished)) {
                return status::finished;
            }
            if (addr == reinterpret_cast<void*>(MarkerDestroyed)) {
                return status::destroyed;
            }
            assert(false && "Unexpected existing continuation");
        }

        // Called by awaiter destructor to prevent it from resuming in the
        // future. There are two cases to consider. One where user continuation
        // is destroyed, which destroys the promise, which in turn destroys the
        // frame with awaiter, which tries to cancel because it was suspended.
        // Another where awaiter frame is destroyed while suspended, because
        // destruction is going bottom up from some root frame, and user must
        // somehow guarantee continuation will not be resumed concurrently or
        // in the future, otherwise it's very racy. Returns the current/previous
        // state of continuation.
        status cancel() noexcept {
            void* addr = continuation.load(std::memory_order_relaxed);
            for (;;) {
                assert(addr && "Trying to unset a missing continuation");
                if (addr == reinterpret_cast<void*>(MarkerFinished)) {
                    return status::finished;
                }
                if (addr == reinterpret_cast<void*>(MarkerDestroyed)) {
                    return status::destroyed;
                }
                if (continuation.compare_exchange_weak(addr, nullptr, std::memory_order_release)) {
                    return status::set;
                }
            }
        }

        // Called by the promise when it is resumed
        // Returns currently set continuation, if any
        std::coroutine_handle<> finish() noexcept {
            void* addr = continuation.exchange(
                reinterpret_cast<void*>(MarkerFinished), std::memory_order_acq_rel);
            assert(addr != reinterpret_cast<void*>(MarkerFinished));
            assert(addr != reinterpret_cast<void*>(MarkerDestroyed)
                && "Unexpected race between destroy and resume");
            if (addr) {
                return std::coroutine_handle<>::from_address(addr);
            } else {
                return {};
            }
        }

        // Returns true if the promise has finished normally
        bool finished() const noexcept {
            void* addr = continuation.load(std::memory_order_acquire);
            return addr == reinterpret_cast<void*>(MarkerFinished);
        }

    private:
        static constexpr uintptr_t MarkerFinished = 1;
        static constexpr uintptr_t MarkerDestroyed = 2;

    private:
        std::atomic<void*> continuation{ nullptr };
    };

    template<class TCallback>
    class with_continuation_promise {
    public:
        with_continuation_promise(TCallback& callback) noexcept
            : callback(callback)
        {}

        ~with_continuation_promise() noexcept {
            if (auto continuation = state->destroy()) {
                // Promise was destroyed instead of resuming
                // Propagate it to the continuation
                continuation.destroy();
            }
        }

        [[nodiscard]] with_continuation_coroutine<TCallback> get_return_object() noexcept {
            return with_continuation_coroutine(
                with_continuation_handle<TCallback>::from_promise(*this),
                state);
        }

        auto initial_suspend() noexcept { return std::suspend_always{}; }

        struct TFinalSuspend {
            bool await_ready() noexcept { return false; }

            __attribute__((__noinline__))
            std::coroutine_handle<> await_suspend(with_continuation_handle<TCallback> c) noexcept {
                auto& self = c.promise();
                if (auto continuation = self.state->finish()) {
                    // Resumed after continuation was set
                    // destroy ourselves and switch to that continuation
                    c.destroy();
                    return continuation;
                }
                // We resumed before continuation was set
                // Suspend and give awaiter a chance to clean us up
                return std::noop_coroutine();
            }

            void await_resume() noexcept {}
        };

        auto final_suspend() noexcept { return TFinalSuspend{}; }

        void run_callback(std::coroutine_handle<> c) {
            // Note: we may be destroyed before callback returns
            std::forward<TCallback>(callback)(std::move(c));
        }

        void unhandled_exception() noexcept {
            // our coroutine never throws exceptions
        }

        void return_void() noexcept {
            // nothing
        }

    private:
        static constexpr uintptr_t MarkerFinished = 1;
        static constexpr uintptr_t MarkerDestroyed = 2;

    private:
        TCallback& callback;
        std::shared_ptr<with_continuation_state> state = std::make_shared<with_continuation_state>();
    };

    template<class TCallback>
    class with_continuation_coroutine {
        friend class with_continuation_promise<TCallback>;

        with_continuation_coroutine(
                with_continuation_handle<TCallback> handle,
                const std::shared_ptr<with_continuation_state>& state) noexcept
            : handle(handle)
            , state(state)
        {}

    public:
        using promise_type = with_continuation_promise<TCallback>;

        with_continuation_coroutine(with_continuation_coroutine&& rhs) noexcept
            : handle(std::exchange(rhs.handle, {}))
            , state(std::move(rhs.state))
            , suspended(rhs.suspended)
        {}

        ~with_continuation_coroutine() {
            if (handle) {
                if (suspended) {
                    switch (state->cancel()) {
                    case with_continuation_state::status::set:
                        // Continue bottom-up frame cleanup
                        handle.destroy();
                        break;
                    case with_continuation_state::status::finished:
                    case with_continuation_state::status::destroyed:
                        // Nothing to clean up
                        break;
                    }
                } else {
                    handle.destroy();
                }
            }
        }

        with_continuation_coroutine& operator=(const with_continuation_coroutine&) = delete;

        bool await_ready() {
            assert(handle && !suspended);
            handle.promise().run_callback(handle);
            // Note: handle may be destroyed already, in that case we will suspend
            return state->finished();
        }

        __attribute__((__noinline__))
        bool await_suspend(std::coroutine_handle<> c) noexcept {
            suspended = true;
            switch (state->set_continuation(c)) {
            case with_continuation_state::status::set:
                // May resume concurrently
                return true;
            case with_continuation_state::status::finished:
                // Actually resumed already
                suspended = false;
                return false;
            case with_continuation_state::status::destroyed:
                // Actually destroyed already
                c.destroy();
                return true;
            }
        }

        void await_resume() noexcept {
            // caller resumed
        }

    private:
        with_continuation_handle<TCallback> handle;
        std::shared_ptr<with_continuation_state> state;
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
