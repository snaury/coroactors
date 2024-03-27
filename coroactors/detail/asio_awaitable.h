#pragma once
#include <coroactors/asio_awaitable.h>
#include <coroactors/detail/awaiters.h>
#include <coroactors/detail/config.h>
#include <coroactors/detail/symmetric_transfer.h>
#include <coroactors/intrusive_ptr.h>
#include <asio/any_io_executor.hpp>
#include <asio/async_result.hpp>
#include <asio/cancellation_signal.hpp>
#include <tuple>
#include <utility>

namespace coroactors::detail {

    template<class T>
    class asio_continuation final
        : public intrusive_atomic_base<asio_continuation<T>>
        , public result<T>
    {
    public:
        explicit asio_continuation(bool allow_cancellation) {
            if (allow_cancellation) {
                cs.emplace();
            }
        }

        asio_continuation(const asio_continuation&) = delete;
        asio_continuation& operator=(const asio_continuation&) = delete;

        bool set_continuation(std::coroutine_handle<> c) noexcept {
            // Make sure it is aligned to at least 4 bytes
            assert(get_marker(c.address()) == 0);

            void* addr = nullptr;

            // Fast path, assume no concurrent activity yet
            if (continuation.compare_exchange_strong(addr, c.address(), std::memory_order_acq_rel)) {
                // We installed our continuation (no result or cancel yet)
                return true;
            }

            if (addr == reinterpret_cast<void*>(MarkerCancel)) {
                // Try to set continuation with a cancel mark
                void* target = with_marker(c.address(), MarkerCancel);
                do {
                    if (continuation.compare_exchange_weak(addr, target, std::memory_order_acq_rel)) {
                        // We installed our continuation while another thread
                        // is running cancellation. That thread will unset the
                        // cancel bit, or even resume it if the result is also
                        // avaiable by that time.
                        return true;
                    }
                } while (addr == reinterpret_cast<void*>(MarkerCancel));
            }

            // We should have the result already
            assert(addr == reinterpret_cast<void*>(MarkerResult));
            return false;
        }

        void unset_continuation() noexcept {
            void* addr = continuation.load(std::memory_order_relaxed);
            for (;;) {
                void* target = with_marker(nullptr, get_marker(addr));
                if (addr == target) {
                    break;
                }
                if (continuation.compare_exchange_weak(addr, target, std::memory_order_release)) {
                    break;
                }
            }
        }

        asio::cancellation_slot slot() noexcept {
            if (cs) {
                return cs->slot();
            } else {
                return {}; // unconnected slot
            }
        }

        /**
         * This method sets the cancellation flag in the continuation pointer,
         * making sure continuation will not be resumed until (a potentially
         * very long) cancellation finishes. There would be no races otherwise,
         * because awaiter resume synchronizes on stop callback destruction,
         * and it would wait anyway. However asio uses a global mutex on the
         * cancellation path, and it would cause two threads not doing any
         * useful work, so it's better to avoid waking up in the first place.
         *
         * Returns true when cancellation flag is set, false when there's no
         * point in cancellation, because there's a result available already.
         */
        bool begin_cancellation() noexcept {
            void* addr = continuation.load(std::memory_order_relaxed);
            while (addr != reinterpret_cast<void*>(MarkerResult)) {
                // We shouldn't have any markers
                assert(get_marker(addr) == 0);
                // Do a CAS setting the cancellation flag.
                void* target = with_marker(addr, MarkerCancel);
                if (continuation.compare_exchange_weak(addr, target, std::memory_order_relaxed)) {
                    // We have marked cancellation as active, it may or may not
                    // have a continuation address currently set. No other
                    // thread will touch shared state until we complete.
                    return true;
                }
            }
            // There's no point in emitting cancellation, there's a result already
            return false;
        }

        /**
         * Emits the specified cancellation type, can be done as long as no
         * other thread potentially calls methods on the same io object.
         */
        void emit_cancellation(asio::cancellation_type ct) noexcept {
            cs->emit(ct);
        }

        /**
         * This ends cancellation started with begin_cancellation, and removes
         * the cancellation flag. It may be the case that the result was
         * provided already, but resume was blocked because the cancellation
         * was in progress. In which case it removes the marked continuation
         * and returns a valid handle, which must be resumed.
         */
        std::coroutine_handle<> end_cancellation() noexcept {
            void* addr = continuation.load(std::memory_order_relaxed);
            for (;;) {
                assert(get_marker(addr) & MarkerCancel);
                void* target = without_marker(addr, MarkerCancel);
                if (continuation.compare_exchange_weak(addr, target, std::memory_order_release)) {
                    // We have removed the cancellation flag
                    if (get_marker(target) & MarkerResult &&
                        target != reinterpret_cast<void*>(MarkerResult))
                    {
                        // We have both continuation and the result flag. It
                        // means result handler finished while we were emitting
                        // cancellation, and it's now our responsibility to
                        // resume. Note: acquire here synchronizes with release
                        // when the result is published.
                        if (continuation.compare_exchange_strong(target, nullptr, std::memory_order_acquire)) {
                            target = without_marker(target, MarkerResult);
                            assert(get_marker(target) == 0);
                            return std::coroutine_handle<>::from_address(target);
                        }
                        // Lost the race: probably due to unset_continuation
                    }
                    break;
                }
            }
            return {};
        }

        /**
         * Publishes the result of operation and returns the continuation
         * handle unless it's not installed yet, or blocked by cancellation.
         */
        std::coroutine_handle<> finish() noexcept {
            void* addr = continuation.load(std::memory_order_acquire);
            for (;;) {
                void* target = (get_marker(addr) & MarkerCancel)
                    ? with_marker(addr, MarkerResult)
                    : with_marker(nullptr, MarkerResult);
                if (continuation.compare_exchange_weak(addr, target, std::memory_order_acq_rel)) {
                    if (get_marker(addr) == 0) {
                        // There was no marker, we should resume coroutine
                        return std::coroutine_handle<>::from_address(addr);
                    }
                    break;
                }
            }
            return {};
        }

        bool ready() const noexcept {
            void* addr = continuation.load(std::memory_order_acquire);
            return addr == reinterpret_cast<void*>(MarkerResult);
        }

    private:
        static uintptr_t get_marker(void* addr) {
            return reinterpret_cast<uintptr_t>(addr) & MarkerMask;
        }

        static void* without_markers(void* addr) {
            return reinterpret_cast<void*>(
                reinterpret_cast<uintptr_t>(addr) & ~MarkerMask);
        }

        static void* with_marker(void* addr, uintptr_t marker) {
            return reinterpret_cast<void*>(
                reinterpret_cast<uintptr_t>(addr) | marker);
        }

        static void* without_marker(void* addr, uintptr_t marker) {
            return reinterpret_cast<void*>(
                reinterpret_cast<uintptr_t>(addr) & ~marker);
        }

    private:
        static constexpr uintptr_t MarkerCancel = 1;
        static constexpr uintptr_t MarkerResult = 2;
        static constexpr uintptr_t MarkerMask = 3;

    private:
        std::atomic<void*> continuation{ nullptr };
        std::optional<asio::cancellation_signal> cs;
    };

    template<class... Ts>
    struct asio_awaitable_handler_result {
        using type = std::tuple<Ts...>;
    };

    template<>
    struct asio_awaitable_handler_result<> {
        using type = void;
    };

    template<class T>
    struct asio_awaitable_handler_result<T> {
        using type = T;
    };

    template<class... Ts>
    using asio_awaitable_handler_result_t = typename asio_awaitable_handler_result<Ts...>::type;

    template<class... Ts>
    class asio_awaitable_handler_base {
    public:
        using result_type = asio_awaitable_handler_result_t<Ts...>;
        using continuation_type = asio_continuation<result_type>;
        using cancellation_slot_type = asio::cancellation_slot;

        explicit asio_awaitable_handler_base(continuation_type* r)
            : result(r)
        {}

        cancellation_slot_type get_cancellation_slot() const noexcept {
            return result->slot();
        }

    protected:
        intrusive_ptr<continuation_type> result;
    };

    /**
     * The default handler, transforms all args into result arguments
     */
    template<class... Ts>
    class asio_awaitable_handler
        : public asio_awaitable_handler_base<Ts...>
    {
    public:
        using asio_awaitable_handler_base<Ts...>::asio_awaitable_handler_base;

        template<class... Args>
        void operator()(Args&&... args) {
            this->result->emplace_value(std::forward<Args>(args)...);
            if (auto c = this->result->finish()) {
                symmetric::resume(c);
            }
        }
    };

    /**
     * The error code handler, transform it into an optional exception
     */
    template<class... Ts>
    class asio_awaitable_handler<asio::error_code, Ts...>
        : public asio_awaitable_handler_base<Ts...>
    {
    public:
        using asio_awaitable_handler_base<Ts...>::asio_awaitable_handler_base;

        template<class... Args>
        void operator()(const asio::error_code& ec, Args&&... args) {
            if (ec) {
                this->result->set_exception(asio::system_error(ec));
            } else {
                this->result->emplace_value(std::forward<Args>(args)...);
            }
            if (auto c = this->result->finish()) {
                symmetric::resume(c);
            }
        }
    };

    /**
     * The optional exception handler, rethrows it in the awaiter
     */
    template<class... Ts>
    class asio_awaitable_handler<std::exception_ptr, Ts...>
        : public asio_awaitable_handler_base<Ts...>
    {
    public:
        using asio_awaitable_handler_base<Ts...>::asio_awaitable_handler_base;

        template<class... Args>
        void operator()(std::exception_ptr e, Args&&... args) {
            if (e) {
                this->result->set_exception(std::move(e));
            } else {
                this->result->emplace_value(std::forward<Args>(args)...);
            }
            if (auto c = this->result->finish()) {
                symmetric::resume(c);
            }
        }
    };

    /**
     * The awaiter class that, when awaited, initiates the async operation and
     * transforms eventual result into some value. Supports cancellation
     * propagation from actors into asio cancellation slots.
     */
    template<class CompletionHandler, class Options, class Initiation, class... Args>
    class asio_awaiter_t {
    public:
        using result_type = typename CompletionHandler::result_type;
        using continuation_type = typename CompletionHandler::continuation_type;

        asio_awaiter_t(Initiation&& initiation, Args&&... args)
            : initiation(std::forward<Initiation>(initiation))
            , args(std::forward<Args>(args)...)
        {}

        asio_awaiter_t(const asio_awaiter_t&) = delete;
        asio_awaiter_t& operator=(const asio_awaiter_t&) = delete;

        asio_awaiter_t(asio_awaiter_t&& rhs)
            : initiation(std::move(rhs.initiation))
            , args(std::move(rhs.args))
        {}

        ~asio_awaiter_t() {
            if (result) {
                // Support bottom-up destruction and cancel a possible future
                // resume. This relies on handler not running concurrently.
                result->unset_continuation();
            }
        }

        bool await_ready() {
            stop_token token = current_stop_token();
            result.reset(new continuation_type(token.stop_possible()));

            // Initiate the async operation
            initiate();

            // The operation has started and may have already completed in
            // another thread, avoid setting stop token in that case.
            if (result->ready()) {
                return true;
            }

            // Setup passthru from stop_token to cancellation_slot
            if (token.stop_possible()) {
                if (token.stop_requested()) {
                    // The token is cancelled already. There's no risk in us
                    // racing with await_resume, so emit without begin/end.
                    result->emit_cancellation(Options::cancel_type);

                    // There's a chance handler was called after emit
                    if (result->ready()) {
                        return true;
                    }
                } else {
                    // Otherwise install the stop callback that would emit
                    // cancellation while we are suspended.
                    stop.emplace(
                        std::move(token),
                        emit_cancellation_t{ result.get() });
                }
            }

            return false;
        }

        COROACTORS_AWAIT_SUSPEND
        bool await_suspend(std::coroutine_handle<> c) {
            return result->set_continuation(c);
        }

        result_type await_resume() {
            // All resume paths should have acquire sync on the result value
            return result->take_value();
        }

    private:
        void initiate() {
            initiate(std::index_sequence_for<Args...>());
        }

        template<size_t... I>
        void initiate(std::index_sequence<I...>) {
            std::move(initiation)(
                CompletionHandler(result.get()),
                std::get<I>(std::move(args))...);
        }

        struct emit_cancellation_t {
            continuation_type* r;

            void operator()() noexcept {
                if (r->begin_cancellation()) {
                    r->emit_cancellation(Options::cancel_type);
                    if (auto c = r->end_cancellation()) {
                        symmetric::resume(c);
                    }
                }
            }
        };

    private:
        std::decay_t<Initiation> initiation;
        std::tuple<std::decay_t<Args>...> args;
        intrusive_ptr<continuation_type> result;
        std::optional<stop_callback<emit_cancellation_t>> stop;
    };

} // namespace coroactors::detail

namespace asio {

    /**
     * Customizes async_result to transform async operations into awaitables
     */
    template<class Executor,
        asio::cancellation_type CancelType,
        class R, class... Ts>
    class async_result<
        ::coroactors::asio_awaitable_t<Executor, CancelType>,
        R(Ts...)>
    {
    public:
        using completion_handler_type =
            ::coroactors::detail::asio_awaitable_handler<
                std::decay_t<Ts>...>;

        template<class Initiation, class Options, class... Args>
        static auto initiate(Initiation&& initiation, Options,
            Args&&... args)
        {
            using awaiter_type = ::coroactors::detail::asio_awaiter_t<
                completion_handler_type, Options, Initiation, Args...>;
            return awaiter_type(
                std::forward<Initiation>(initiation),
                std::forward<Args>(args)...);
        }
    };

} // namespace asio
