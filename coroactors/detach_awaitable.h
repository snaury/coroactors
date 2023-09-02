#pragma once
#include <coroactors/detail/detach_awaitable.h>

namespace coroactors {

    /**
     * Runs `co_await awaitable` and ignores the result and/or exceptions
     */
    template<class Awaitable>
    detail::detach_awaitable_ignore_coroutine<Awaitable>
    detach_awaitable(Awaitable awaitable) {
        co_return co_await std::move(awaitable);
    }

    /**
     * Runs `co_await awaitable` and calls callback with the result
     *
     * Terminates when the awaitable or the callback throw an exception.
     */
    template<class Awaitable, class Callback>
    detail::detach_awaitable_callback_coroutine<Awaitable, Callback>
    detach_awaitable(Awaitable awaitable, Callback) {
        // Note: underlying promise takes callback argument address and calls it when we return
        co_return co_await std::move(awaitable);
    }

} // namespace coroactors
