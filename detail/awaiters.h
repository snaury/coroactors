#pragma once
#include <concepts>

namespace coroactors::detail {

    template<class TAwaiter>
    concept has_await_ready = requires(TAwaiter& awaiter) {
        awaiter.await_ready() ? 1 : 0;
    };

    template<class TAwaiter>
    concept has_noexcept_await_ready = requires(TAwaiter& awaiter) {
        { awaiter.await_ready() ? 1 : 0 } noexcept;
    };

    template<class TAwaiter, class TPromise = void>
    concept has_await_suspend_void = requires(TAwaiter& awaiter, std::coroutine_handle<TPromise> h) {
        { awaiter.await_suspend(h) } -> std::same_as<void>;
    };

    template<class TAwaiter, class TPromise = void>
    concept has_await_suspend_bool = requires(TAwaiter& awaiter, std::coroutine_handle<TPromise> h) {
        { awaiter.await_suspend(h) } -> std::same_as<bool>;
    };

    template<class TAwaiter, class TPromise = void>
    concept has_await_suspend_handle = requires(TAwaiter& awaiter, std::coroutine_handle<TPromise> h) {
        { awaiter.await_suspend(h) } -> std::convertible_to<std::coroutine_handle<>>;
    };

    template<class TAwaiter, class TPromise = void>
    concept has_noexcept_await_suspend = requires(TAwaiter& awaiter, std::coroutine_handle<TPromise> h) {
        { awaiter.await_suspend(h) } noexcept;
    };

    template<class TAwaiter>
    concept has_await_resume = requires(TAwaiter& awaiter) {
        awaiter.await_resume();
    };

    template<class TAwaiter>
    concept has_noexcept_await_resume = requires(TAwaiter& awaiter) {
        { awaiter.await_resume() } noexcept;
    };

    template<class TAwaiter, class TPromise = void>
    concept awaiter =
        has_await_ready<TAwaiter> &&
        has_await_resume<TAwaiter> && (
            has_await_suspend_void<TAwaiter, TPromise> ||
            has_await_suspend_bool<TAwaiter, TPromise> ||
            has_await_suspend_handle<TAwaiter, TPromise>);

    template<class TAwaiter>
    using awaiter_result_t = decltype(std::declval<TAwaiter&>().await_resume());

    template<class TAwaitable, class TPromise = void>
    concept has_member_co_await = requires(TAwaitable&& awaitable) {
        { ((TAwaitable&&) awaitable).operator co_await() } -> awaiter<TPromise>;
    };

    template<class TAwaitable, class TPromise = void>
    concept has_global_co_await = requires(TAwaitable&& awaitable) {
        { operator co_await((TAwaitable&&) awaitable) } -> awaiter<TPromise>;
    };

    template<class TAwaitable, class TPromise = void>
    concept awaitable =
        has_member_co_await<TAwaitable, TPromise> ||
        has_global_co_await<TAwaitable, TPromise> ||
        awaiter<TAwaitable, TPromise>;

    template<class TAwaitable>
    decltype(auto) get_awaiter(TAwaitable&& awaitable) {
        if constexpr (requires { ((TAwaitable&&) awaitable).operator co_await(); }) {
            return ((TAwaitable&&) awaitable).operator co_await();
        } else if constexpr (requires { operator co_await((TAwaitable&&) awaitable); }) {
            return operator co_await((TAwaitable&&) awaitable);
        } else {
            return ((TAwaitable&&) awaitable);
        }
    }

    template<class TAwaitable>
    using awaitable_awaiter_t = std::remove_reference_t<decltype(get_awaiter(std::declval<TAwaitable>()))>;

    template<class TAwaitable>
    using await_result_t = decltype(get_awaiter(std::declval<TAwaitable>()).await_resume());

} // namespace coroactors::detail
