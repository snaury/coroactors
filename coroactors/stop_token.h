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

namespace coroactors {

    using std::nostopstate_t;
    using std::nostopstate;

    using std::stop_token;
    using std::stop_source;
    using std::stop_callback;

} // namespace coroactors
#else
#include <coroactors/detail/stop_token_polyfill.h>

namespace coroactors {

    using detail::nostopstate_t;
    using detail::nostopstate;

    using detail::stop_token;
    using detail::stop_source;
    using detail::stop_callback;

} // namespace coroactors
#endif

namespace coroactors {

    /**
     * Check whether cancellation propagation customization is available
     */
    template<class Awaitable>
    concept has_coroactors_propagate_stop_token = requires(Awaitable& awaitable, const stop_token& token) {
        /**
         * An ADL customization point for propagating cancellation
         */
        coroactors_propagate_stop_token(awaitable, token);
    };

    /**
     * Returns an awaitable that has the specified stop token propagated
     */
    template<class Awaitable>
    Awaitable&& with_stop_token(const stop_token& token, Awaitable&& awaitable)
        requires (has_coroactors_propagate_stop_token<Awaitable>)
    {
        coroactors_propagate_stop_token(awaitable, token);
        return std::forward<Awaitable>(awaitable);
    }

} // namespace coroactors
