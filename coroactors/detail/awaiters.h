#pragma once
#include <concepts>
#include <coroutine>
#include <utility>

namespace coroactors::detail {

    template<class T>
    struct remove_rvalue_reference {
        using type = T;
    };

    template<class T>
    struct remove_rvalue_reference<T&&> {
        using type = T;
    };

    template<class T>
    using remove_rvalue_reference_t = typename remove_rvalue_reference<T>::type;

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
     * Transforms awaitable into its awaitable type, which may be an rvalue reference
     *
     * Best for await_transform classes, where it's guaranteed that original
     * awaitable and a transformed awaiter have the same lifetime. Similar to
     * having an `Awaiter&&` deduced argument in a wrapper function.
     */
    template<class Awaitable, class Promise = void>
        requires awaitable<Awaitable, Promise>
    using awaiter_transform_type_t = decltype(get_awaiter(std::declval<Awaitable&&>()));

    /**
     * Transforms awaitable into its awaitable type, but removes an rvalue
     *
     * This is useful for wrapper classes, where the result may be transferred
     * somewhere else (detach_awaitable, task_group, etc.) and the original
     * awaitable may be destroyed before the result finished awaiting. Similar
     * to having an `Awaiter` deduced argument in a wrapper function.
     */
    template<class Awaitable, class Promise = void>
        requires awaitable<Awaitable, Promise>
    using awaiter_safe_type_t = remove_rvalue_reference_t<awaiter_transform_type_t<Awaitable, Promise>>;

    /**
     * The result type returned from an awaiter (not awaitable)
     */
    template<class Awaiter, class Promise = void>
        requires awaiter<Awaiter, Promise>
    using awaiter_result_t = decltype(std::declval<Awaiter&>().await_resume());

    /**
     * The result type returned from an awaiter (i.e. what co_await returns)
     */
    template<class Awaitable, class Promise = void>
        requires awaitable<Awaitable, Promise>
    using await_result_t = awaiter_result_t<awaiter_transform_type_t<Awaitable>>;

    /**
     * Concept for awaiters that have a wrapped_awaiter_type typedef defined
     */
    template<class Awaiter>
    concept has_wrapped_awaiter_type = requires {
        typename Awaiter::wrapped_awaiter_type;
    };

    template<class Awaiter>
    struct awaiter_wrapped_awaiter_type_impl {
        using type = Awaiter;
    };

    template<has_wrapped_awaiter_type Awaiter>
    struct awaiter_wrapped_awaiter_type_impl<Awaiter> {
        using type = typename Awaiter::wrapped_awaiter_type;
    };

    template<class Awaiter>
    using awaiter_unwrap_awaiter_type =
        typename awaiter_wrapped_awaiter_type_impl<Awaiter>::type;

    template<class Awaitable, class Promise = void>
        requires awaitable<Awaitable, Promise>
    using awaitable_unwrap_awaiter_type = awaiter_unwrap_awaiter_type<
        std::conditional_t<
            has_co_await<Awaitable, Promise>,
            awaiter_safe_type_t<Awaitable, Promise>,
            Awaitable>>;

} // namespace coroactors::detail
