#pragma once
#include <concepts>
#include <coroutine>
#include <utility>

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
    concept has_await_suspend = (
        has_await_suspend_void<Awaiter, Promise> ||
        has_await_suspend_bool<Awaiter, Promise> ||
        has_await_suspend_handle<Awaiter, Promise>);

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
        has_await_suspend<Awaiter, Promise> &&
        has_await_resume<Awaiter>;

    template<class Awaitable, class Promise = void>
    concept has_member_co_await = requires(Awaitable&& awaitable) {
        { std::forward<Awaitable>(awaitable).operator co_await() } -> awaiter<Promise>;
    };

    template<class Awaitable, class Promise = void>
    concept has_global_co_await = requires(Awaitable&& awaitable) {
        { operator co_await(std::forward<Awaitable>(awaitable)) } -> awaiter<Promise>;
    };

    template<class Awaitable, class Promise = void>
    concept has_co_await =
        has_member_co_await<Awaitable, Promise> ||
        has_global_co_await<Awaitable, Promise>;

    template<class Awaitable, class Promise = void>
    concept awaitable =
        has_co_await<Awaitable, Promise> ||
        awaiter<Awaitable, Promise>;

    template<class Awaitable>
    inline decltype(auto) get_awaiter(Awaitable&& awaitable) {
        if constexpr (requires { std::forward<Awaitable>(awaitable).operator co_await(); }) {
            return std::forward<Awaitable>(awaitable).operator co_await();
        } else if constexpr (requires { operator co_await(std::forward<Awaitable>(awaitable)); }) {
            return operator co_await(std::forward<Awaitable>(awaitable));
        } else {
            return std::forward<Awaitable>(awaitable);
        }
    }

    /**
     * Returns an Awaiter type from an Awaitable deduced type, which is often
     * an rvalue reference when Awaitable is also an Awaiter.
     *
     * Best for await_transform classes, where it's guaranteed that original
     * awaitable and a transformed awaiter have the same lifetime, similar to
     * having an `Awaiter&&` deduced argument in a wrapper function. When
     * lifetimes are not guaranteed to match use std::decay_t on the result.
     */
    template<class Awaitable, class Promise = void>
        requires awaitable<Awaitable, Promise>
    using awaiter_type_t = decltype(get_awaiter(std::declval<Awaitable&&>()));

    /**
     * The return type of an awaiter's await_suspend
     */
    template<class Awaiter, class Promise = void>
        requires awaiter<Awaiter, Promise>
    using awaiter_suspend_result_t = decltype(std::declval<Awaiter&>()
        .await_suspend(std::declval<std::coroutine_handle<Promise>>()));

    /**
     * The result type returned from an awaiter (not an awaitable)
     */
    template<class Awaiter, class Promise = void>
        requires awaiter<Awaiter, Promise>
    using awaiter_result_t = decltype(std::declval<Awaiter&>().await_resume());

    /**
     * The result type returned from an awaiter (i.e. what co_await returns)
     */
    template<class Awaitable, class Promise = void>
        requires awaitable<Awaitable, Promise>
    using await_result_t = awaiter_result_t<awaiter_type_t<Awaitable>>;

} // namespace coroactors::detail
