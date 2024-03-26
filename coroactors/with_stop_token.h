#pragma once
#include <coroactors/async.h>
#include <coroactors/stop_token.h>
#include <coroactors/detail/awaiters.h>
#include <coroactors/detail/scope_guard.h>

namespace coroactors {

    /**
     * Wraps awaitable with the specified stop token override
     */
    template<detail::awaitable Awaitable>
    async<detail::await_result_t<Awaitable>> with_stop_token(stop_token token, Awaitable awaitable) {
        detail::async_task* task = detail::async_task::current;
        assert(task);

        swap(task->token, token);
        detail::scope_guard guard([&]{
            swap(task->token, token);
        });

        co_return co_await std::move(awaitable);
    }

} // namespace coroactors
