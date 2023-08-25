#include <coroactors/detail/awaiters.h>
#include <coroutine>

#pragma once
#ifndef coroactors_use_std_stop_token
#if __has_include(<stop_token>)
#define coroactors_use_std_stop_token 1
#else
#define coroactors_use_std_stop_token 0
#endif
#endif

#if coroactors_use_std_stop_token
#include <stop_token>

namespace coroactors::detail {

    using std::nostopstate_t;
    using std::nostopstate;

    using std::stop_token;
    using std::stop_source;
    using std::stop_callback;

} // namespace coroactors::detail
#else
#include <coroactors/detail/stop_token_polyfill.h>
#endif

namespace coroactors::detail {

    /**
     * Awaiter with `bool await_ready(const stop_token&)` extension present
     */
    template<class Awaiter>
    concept has_await_ready_stop_token = requires(Awaiter& awaiter, const stop_token& token) {
        { awaiter.await_ready(token) } -> std::same_as<bool>;
    };

    /**
     * Awaiter with `bool await_ready(const stop_token&)` extension declared noexcept
     */
    template<class Awaiter>
    concept has_noexcept_await_ready_stop_token = requires(Awaiter& awaiter, const stop_token& token) {
        { awaiter.await_ready(token) } noexcept;
    };

    template<class Awaitable>
    concept awaitable_with_stop_token_propagation = awaitable<Awaitable> &&
        has_await_ready_stop_token<decltype(get_awaiter(std::declval<Awaitable>()))>;

    /**
     * Awaiter that overrides stop token of an awaitable
     */
    template<awaitable_with_stop_token_propagation Awaitable>
    class with_stop_token_awaiter {
        using Awaiter = decltype(get_awaiter(std::declval<Awaitable>()));

    public:
        with_stop_token_awaiter(Awaitable&& awaitable, const stop_token& token)
            : awaiter(get_awaiter(std::forward<Awaitable>(awaitable)))
            , token(token)
        {}

        bool await_ready()
            noexcept(has_noexcept_await_ready_stop_token<Awaiter>)
        {
            return awaiter.await_ready(token);
        }

        template<class Promise>
        decltype(auto) await_suspend(std::coroutine_handle<Promise> c)
            noexcept(has_noexcept_await_suspend<Awaiter, Promise>)
        {
            return awaiter.await_suspend(c);
        }

        decltype(auto) await_resume()
            noexcept(has_noexcept_await_resume<Awaiter>)
        {
            return awaiter.await_resume();
        }

    private:
        Awaiter awaiter;
        const stop_token& token;
    };

} // namespace coroactors::detail
