#pragma once
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/cancellation_type.hpp>
#include <type_traits>

namespace coroactors {

    /**
     * CompletionToken type that transforms asio operations into awaitables
     *
     * Also stores options that may modify the way operations perform.
     */
    template<class Executor = boost::asio::any_io_executor,
        boost::asio::cancellation_type CancelType = boost::asio::cancellation_type::all>
    struct asio_awaitable_t {
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
         * Changes object to use asio_awaitable_t as the default completion token
         */
        template<class T>
        static as_default_on_t<std::decay_t<T>> as_default_on(T&& obj) {
            return as_default_on_t<std::decay_t<T>>(std::forward<T>(obj));
        }

        /**
         * Specifies cancellation type used for stop token propagation
         */
        static constexpr boost::asio::cancellation_type cancel_type = CancelType;

        template<boost::asio::cancellation_type OtherCancelType>
        using with_cancel_type_t = asio_awaitable_t<Executor, OtherCancelType>;
    };

    /**
     * A completion token object suitable for any executor
     */
    inline constexpr asio_awaitable_t<> asio_awaitable{};

} // namespace coroactors

// We must include asio_awaitable_t implementation last
#include <coroactors/detail/asio_awaitable.h>
