#pragma once
#include <concepts>

namespace coroactors::detail {

    template<class Awaiter>
    concept has_await_ready = requires(Awaiter& awaiter) {
        awaiter.await_ready() ? 1 : 0;
    };

    template<class Awaiter>
    concept has_noexcept_await_ready = requires(Awaiter& awaiter) {
        { awaiter.await_ready() ? 1 : 0 } noexcept;
    };

    template<class Awaiter, class Promise = void>
    concept has_await_suspend_void = requires(Awaiter& awaiter, std::coroutine_handle<Promise> h) {
        { awaiter.await_suspend(h) } -> std::same_as<void>;
    };

    template<class Awaiter, class Promise = void>
    concept has_await_suspend_bool = requires(Awaiter& awaiter, std::coroutine_handle<Promise> h) {
        { awaiter.await_suspend(h) } -> std::same_as<bool>;
    };

    template<class Awaiter, class Promise = void>
    concept has_await_suspend_handle = requires(Awaiter& awaiter, std::coroutine_handle<Promise> h) {
        { awaiter.await_suspend(h) } -> std::convertible_to<std::coroutine_handle<>>;
    };

    template<class Awaiter, class Promise = void>
    concept has_noexcept_await_suspend = requires(Awaiter& awaiter, std::coroutine_handle<Promise> h) {
        { awaiter.await_suspend(h) } noexcept;
    };

    template<class Awaiter>
    concept has_await_resume = requires(Awaiter& awaiter) {
        awaiter.await_resume();
    };

    template<class Awaiter>
    concept has_noexcept_await_resume = requires(Awaiter& awaiter) {
        { awaiter.await_resume() } noexcept;
    };

    template<class Awaiter, class Promise = void>
    concept awaiter =
        has_await_ready<Awaiter> &&
        has_await_resume<Awaiter> && (
            has_await_suspend_void<Awaiter, Promise> ||
            has_await_suspend_bool<Awaiter, Promise> ||
            has_await_suspend_handle<Awaiter, Promise>);

    template<class Awaiter>
    using awaiter_result_t = decltype(std::declval<Awaiter&>().await_resume());

    template<class Awaitable, class Promise = void>
    concept has_member_co_await = requires(Awaitable&& awaitable) {
        { std::forward<Awaitable>(awaitable).operator co_await() } -> awaiter<Promise>;
    };

    template<class Awaitable, class Promise = void>
    concept has_global_co_await = requires(Awaitable&& awaitable) {
        { operator co_await(std::forward<Awaitable>(awaitable)) } -> awaiter<Promise>;
    };

    template<class Awaitable, class Promise = void>
    concept awaitable =
        has_member_co_await<Awaitable, Promise> ||
        has_global_co_await<Awaitable, Promise> ||
        awaiter<Awaitable, Promise>;

    template<class Awaitable>
    decltype(auto) get_awaiter(Awaitable&& awaitable) {
        if constexpr (requires { std::forward<Awaitable>(awaitable).operator co_await(); }) {
            return std::forward<Awaitable>(awaitable).operator co_await();
        } else if constexpr (requires { operator co_await(std::forward<Awaitable>(awaitable)); }) {
            return operator co_await(std::forward<Awaitable>(awaitable));
        } else {
            return std::forward<Awaitable>(awaitable);
        }
    }

    template<class Awaitable>
    using await_result_t = decltype(get_awaiter(std::declval<Awaitable>()).await_resume());

} // namespace coroactors::detail
