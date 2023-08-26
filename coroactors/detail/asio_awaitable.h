#pragma once
#include <coroactors/detail/awaiters.h>
#include <tuple>
#include <boost/asio/async_result.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/any_io_executor.hpp>

namespace coroactors::detail {

    struct asio_awaitable_t;
    struct asio_awaitable_raw_args_t;

    /**
     * CompletionToken type that transforms asio operations into awaitables
     *
     * Also stores options that may modify the way operations perform.
     */
    struct asio_awaitable_t {
        // Cancels all levels by default
        boost::asio::cancellation_type cancel_type =
            boost::asio::cancellation_type::all;

        /**
         * Changes cancellation type to use on cancellation
         */
        asio_awaitable_t operator()(boost::asio::cancellation_type ct) const noexcept {
            auto a = *this;
            a.cancel_type = ct;
            return a;
        }

        /**
         * Changes behavior to return raw handler args without exceptions
         */
        asio_awaitable_raw_args_t raw_args() const noexcept;
    };

    /**
     * CompletionToken that transforms asio operations into awaitables
     *
     * However it also throws an exception instead of a providing
     * boost::system::error_code in the result type.
     */
    struct asio_awaitable_raw_args_t : public asio_awaitable_t {
        explicit asio_awaitable_raw_args_t(const asio_awaitable_t& a)
            : asio_awaitable_t(a)
        {}

        template<class... Args>
        void operator()(Args&&... args) const = delete;
    };

    inline asio_awaitable_raw_args_t asio_awaitable_t::raw_args() const noexcept {
        return asio_awaitable_raw_args_t(*this);
    }

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

    template<class Options, class... Ts>
    struct asio_awaitable_handler_result {
        using type = std::tuple<Ts...>;
    };

    template<>
    struct asio_awaitable_handler_result<asio_awaitable_t> {
        using type = void;
    };

    template<class T>
    struct asio_awaitable_handler_result<asio_awaitable_t, T> {
        using type = T;
    };

    template<class Options, class... Ts>
    using asio_awaitable_handler_result_t = typename asio_awaitable_handler_result<Options, Ts...>::type;

    template<class T>
    class asio_awaitable_handler_base {
    public:
        using return_type = T;
        using continuation_type = asio_continuation<return_type>;
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
    template<class Options, class... Ts>
    class asio_awaitable_handler
        : public asio_awaitable_handler_base<asio_awaitable_handler_result_t<Options, Ts...>>
    {
    public:
        using asio_awaitable_handler_base<asio_awaitable_handler_result_t<Options, Ts...>>::asio_awaitable_handler_base;

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
    class asio_awaitable_handler<asio_awaitable_t, boost::system::error_code, Ts...>
        : public asio_awaitable_handler_base<asio_awaitable_handler_result_t<asio_awaitable_t, Ts...>>
    {
    public:
        using asio_awaitable_handler_base<asio_awaitable_handler_result_t<asio_awaitable_t, Ts...>>::asio_awaitable_handler_base;

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
    template<class CompletionHandler, class Initiation, class... Args>
    class asio_awaiter_t {
    public:
        using return_type = typename CompletionHandler::return_type;
        using continuation_type = typename CompletionHandler::continuation_type;

        asio_awaiter_t(asio_awaitable_t options, Initiation&& initiation, Args&&... args)
            : options(options)
            , initiation(std::forward<Initiation>(initiation))
            , args(std::forward<Args>(args)...)
        {}

        asio_awaiter_t(const asio_awaiter_t&) = delete;
        asio_awaiter_t& operator=(const asio_awaiter_t&) = delete;

        asio_awaiter_t(asio_awaiter_t&& rhs)
            : options(std::move(rhs.options))
            , initiation(std::move(rhs.initiation))
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
                    emit_cancellation_t{ result.get(), options.cancel_type });

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

        return_type await_resume() {
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
            boost::asio::cancellation_type ct;

            void operator()() noexcept {
                if (r->begin_cancellation()) {
                    r->emit_cancellation(ct);
                    if (auto c = r->end_cancellation()) {
                        c.resume();
                    }
                }
            }
        };

    private:
        asio_awaitable_t options;
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
    template<class R, class... Signature>
    class async_result<::coroactors::detail::asio_awaitable_t, R(Signature...)> {
    public:
        using completion_handler_type =
            ::coroactors::detail::asio_awaitable_handler<
                ::coroactors::detail::asio_awaitable_t,
                std::decay_t<Signature>...>;

        template<class Initiation, class... Args>
        static auto initiate(Initiation&& initiation,
            ::coroactors::detail::asio_awaitable_t options, Args&&... args)
        {
            using awaiter_type = ::coroactors::detail::asio_awaiter_t<
                completion_handler_type, Initiation, Args...>;
            return awaiter_type(
                options,
                std::forward<Initiation>(initiation),
                std::forward<Args>(args)...);
        }
    };

    /**
     * Customizes async_result to transform async operations into awaitables
     */
    template<class R, class... Signature>
    class async_result<::coroactors::detail::asio_awaitable_raw_args_t, R(Signature...)> {
    public:
        using completion_handler_type =
            ::coroactors::detail::asio_awaitable_handler<
                ::coroactors::detail::asio_awaitable_raw_args_t,
                std::decay_t<Signature>...>;

        template<class Initiation, class... Args>
        static auto initiate(Initiation&& initiation,
            ::coroactors::detail::asio_awaitable_raw_args_t options, Args&&... args)
        {
            using awaiter_type = ::coroactors::detail::asio_awaiter_t<
                completion_handler_type, Initiation, Args...>;
            return awaiter_type(
                options,
                std::forward<Initiation>(initiation),
                std::forward<Args>(args)...);
        }
    };

} // namespace boost::asio
