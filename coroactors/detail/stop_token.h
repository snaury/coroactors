#pragma once
#include <coroactors/detail/awaiters.h>
#include <coroactors/detail/config.h>
#include <coroutine>

#pragma once
#ifndef coroactors_use_std_stop_token
#include <version>
#if __has_include(<stop_token>) && defined(__cpp_lib_jthread)
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
     * Thread local pointer to currently running coroutine stop token
     */
    inline thread_local const stop_token* current_stop_token_ptr{ nullptr };

    /**
     * Returns stop token for the currently running coroutine
     */
    const stop_token& current_stop_token() noexcept {
        if (const stop_token* p = current_stop_token_ptr) {
            return *p;
        } else {
            static stop_token empty;
            return empty;
        }
    }

    /**
     * Temporarily sets current_stop_token_ptr and restores it on scope exit
     */
    class current_stop_token_ptr_guard {
    public:
        current_stop_token_ptr_guard(const stop_token* ptr) noexcept
            : saved(current_stop_token_ptr)
        {
            current_stop_token_ptr = ptr;
        }

        ~current_stop_token_ptr_guard() noexcept {
            current_stop_token_ptr = saved;
        }

    private:
        const stop_token* saved;
    };

    /**
     * Awaiter that changes current stop token for an awaitable
     */
    template<awaitable Awaitable>
    class with_stop_token_awaiter {
        using Awaiter = std::decay_t<awaiter_type_t<Awaitable>>;

    public:
        using wrapped_awaiter_type = awaiter_unwrap_awaiter_type<Awaiter>;

        with_stop_token_awaiter(stop_token token, Awaitable&& awaitable)
            : awaiter(get_awaiter(std::forward<Awaitable>(awaitable)))
            , token(std::move(token))
        {}

        with_stop_token_awaiter(const with_stop_token_awaiter&) = delete;
        with_stop_token_awaiter& operator=(const with_stop_token_awaiter&) = delete;

        with_stop_token_awaiter(with_stop_token_awaiter&& rhs)
            : awaiter(std::move(rhs.awaiter))
            , token(std::move(rhs.token))
        {}

        bool await_ready()
            noexcept(has_noexcept_await_ready<Awaiter>)
        {
            current_stop_token_ptr_guard guard(&token);
            return awaiter.await_ready();
        }

        template<class Promise>
        COROACTORS_AWAIT_SUSPEND
        decltype(auto) await_suspend(std::coroutine_handle<Promise> c)
            noexcept(has_noexcept_await_suspend<Awaiter, Promise>)
            requires has_await_suspend<Awaiter, Promise>
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
        stop_token token;
    };

} // namespace coroactors::detail
