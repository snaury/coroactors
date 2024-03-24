#pragma once
#include <coroactors/detail/awaiters.h>
#include <coroactors/detail/config.h>

namespace coroactors::detail {

    struct coroutine_local_record {
        const void* key;
        const void* value;
        const coroutine_local_record* prev;

        coroutine_local_record(const void* key, const void* value) noexcept
            : key(key)
            , value(value)
            , prev(nullptr)
        {}
    };

    inline thread_local const coroutine_local_record* current_coroutine_local_ptr{ nullptr };

    inline const void* get_coroutine_local(const void* key) noexcept {
        const coroutine_local_record* p = current_coroutine_local_ptr;
        while (p) {
            if (p->key == key) {
                return p->value;
            }
            p = p->prev;
        }
        return nullptr;
    }

    class current_coroutine_local_ptr_guard {
    public:
        current_coroutine_local_ptr_guard(const coroutine_local_record* ptr) noexcept
            : saved(current_coroutine_local_ptr)
        {
            current_coroutine_local_ptr = ptr;
        }

        ~current_coroutine_local_ptr_guard() noexcept {
            current_coroutine_local_ptr = saved;
        }

    private:
        const coroutine_local_record* saved;
    };

    template<awaitable Awaitable>
    class with_coroutine_local_awaiter {
        using Awaiter = std::decay_t<awaiter_type_t<Awaitable>>;

    public:
        using wrapped_awaiter_type = awaiter_unwrap_awaiter_type<Awaiter>;

        with_coroutine_local_awaiter(const void* key, const void* value, Awaitable&& awaitable)
            : awaiter(get_awaiter(std::forward<Awaitable>(awaitable)))
            , record(key, value)
        {}

        with_coroutine_local_awaiter(const with_coroutine_local_awaiter&) = delete;
        with_coroutine_local_awaiter& operator=(const with_coroutine_local_awaiter&) = delete;

        with_coroutine_local_awaiter(with_coroutine_local_awaiter&& rhs)
            : awaiter(std::move(rhs.awaiter))
            , record(std::move(rhs.record))
        {}

        bool await_ready()
            noexcept(has_noexcept_await_ready<Awaiter>)
        {
            record.prev = current_coroutine_local_ptr;
            current_coroutine_local_ptr_guard guard(&record);
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
        coroutine_local_record record;
    };

} // namespace coroactors::detail
