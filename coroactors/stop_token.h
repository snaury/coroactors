#pragma once
#include <coroactors/detail/stop_token.h>

namespace coroactors {

    using detail::nostopstate_t;
    using detail::nostopstate;

    using detail::stop_token;
    using detail::stop_source;
    using detail::stop_callback;

    /**
     * Wraps awaitable with the specified stop token override
     */
    template<detail::awaitable_with_stop_token_propagation Awaitable>
    auto with_stop_token(const stop_token& token, Awaitable&& awaitable) {
        return detail::with_stop_token_awaiter<Awaitable>(std::forward<Awaitable>(awaitable), token);
    }

} // namespace coroactors
