#pragma once
#include <coroactors/detail/awaiters.h>
#include <tuple>
#include <boost/asio/async_result.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/any_io_executor.hpp>

namespace coroactors {

    /**
     * CompletionToken type that transforms asio operations into awaitables
     *
     * Also stores options that may modify the way operations perform.
     */
    template<class Executor = boost::asio::any_io_executor,
        boost::asio::cancellation_type CancelType = boost::asio::cancellation_type::all>
    struct asio_awaitable_t {
        using executor_type = Executor;

        template<class OtherExecutor>
        struct rebind_executor {
            using other = asio_awaitable_t<OtherExecutor, CancelType>;
        };

        /**
         * Wrapped executor with asio_awaitable_t as the default completion token
         */
        template<class WrappedExecutor>
        struct executor_with_default : public WrappedExecutor {
            using default_completion_token_type = asio_awaitable_t;

            template<class ExecutorArg>
            executor_with_default(ExecutorArg&& executor)
                requires (
                    // Don't break synthesized constructors
                    !std::is_same_v<std::decay_t<ExecutorArg>, executor_with_default> &&
                    std::is_convertible_v<ExecutorArg&&, WrappedExecutor>
                )
                : WrappedExecutor(std::forward<ExecutorArg>(executor))
            {}
        };

        /**
         * Changes object type to use asio_awaitable_t as the default completion token
         */
        template<class T>
        using as_default_on_t = typename T::template rebind_executor<
            executor_with_default<typename T::executor_type>>::other;

        /**
         * Specifies cancellation type used for stop token propagation
         */
        static constexpr boost::asio::cancellation_type cancel_type = CancelType;

        template<boost::asio::cancellation_type OtherCancelType>
        using with_cancel_type_t = asio_awaitable_t<Executor, OtherCancelType>;
    };

} // namespace coroactors

namespace coroactors::detail {

    template<class T>
    class asio_continuation : public result<T> {
    public:
        asio_continuation() = default;

        asio_continuation(const asio_continuation&) = delete;
        asio_continuation& operator=(const asio_continuation&) = delete;

        ~asio_continuation() {
            void* addr = without_markers(continuation.load(std::memory_order_acquire));
            if (addr) {
                // It shouldn't be possible, since awaiter holds a strong
                // reference and unsets continuation in the destructor
                std::coroutine_handle<>::from_address(addr).destroy();
            }
        }

        void add_ref() noexcept {
            refcount.fetch_add(1, std::memory_order_relaxed);
        }

        size_t release_ref() noexcept {
            return refcount.fetch_sub(1, std::memory_order_acq_rel) - 1;
        }

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

        boost::asio::cancellation_slot slot() noexcept {
            return cs.slot();
        }

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

        void emit_cancellation(boost::asio::cancellation_type ct) noexcept {
            cs.emit(ct);
        }

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
        std::atomic<size_t> refcount{ 0 };
        std::atomic<void*> continuation{ nullptr };
        boost::asio::cancellation_signal cs;
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
        using cancellation_slot_type = boost::asio::cancellation_slot;

        explicit asio_awaitable_handler_base(continuation_type* r)
            : result(r)
        {}

        cancellation_slot_type get_cancellation_slot() const noexcept {
            return result->slot();
        }

    protected:
        detail::intrusive_ptr<continuation_type> result;
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
                c.resume();
            }
        }
    };

    /**
     * The error code handler, transform it into an optional exception
     */
    template<class... Ts>
    class asio_awaitable_handler<boost::system::error_code, Ts...>
        : public asio_awaitable_handler_base<Ts...>
    {
    public:
        using asio_awaitable_handler_base<Ts...>::asio_awaitable_handler_base;

        template<class... Args>
        void operator()(const boost::system::error_code& ec, Args&&... args) {
            if (ec) {
                this->result->set_exception(std::make_exception_ptr(boost::system::system_error(ec)));
            } else {
                this->result->emplace_value(std::forward<Args>(args)...);
            }
            if (auto c = this->result->finish()) {
                c.resume();
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
                result->unset_continuation();
            }
        }

        bool await_ready(stop_token token = {}) {
            result.reset(new continuation_type);

            // Initiate the async operation
            std::apply(
                start_initiation_t{ initiation, CompletionHandler(result.get()) },
                std::move(args));

            // The operation has started and may have already completed in
            // another thread, avoid setting stop token in that case.
            if (result->ready()) {
                return true;
            }

            // Setup passthru from stop token to cancellation slot
            if (token.stop_possible()) {
                stop.emplace(
                    std::move(token),
                    emit_cancellation_t{ result.get() });

                // It is possible cancellation causes result to become available
                if (result->ready()) {
                    return true;
                }
            }

            return false;
        }

        __attribute__((__noinline__))
        bool await_suspend(std::coroutine_handle<> c) {
            return result->set_continuation(c);
        }

        result_type await_resume() {
            // All resume paths should have acquire sync on the result value
            return result->take_value();
        }

    private:
        struct start_initiation_t {
            Initiation& initiation;
            CompletionHandler&& handler;

            template<class... InitArgs>
            void operator()(InitArgs&&... args) {
                std::move(initiation)(std::move(handler), std::forward<InitArgs>(args)...);
            }
        };

        struct emit_cancellation_t {
            continuation_type* r;

            void operator()() noexcept {
                if (r->begin_cancellation()) {
                    r->emit_cancellation(Options::cancel_type);
                    if (auto c = r->end_cancellation()) {
                        c.resume();
                    }
                }
            }
        };

    private:
        std::decay_t<Initiation> initiation;
        std::tuple<std::decay_t<Args>...> args;
        detail::intrusive_ptr<continuation_type> result;
        std::optional<stop_callback<emit_cancellation_t>> stop;
    };

} // namespace coroactors::detail

namespace boost::asio {

    /**
     * Customizes async_result to transform async operations into awaitables
     */
    template<class Executor,
        boost::asio::cancellation_type CancelType,
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

} // namespace boost::asio
