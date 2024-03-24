#pragma once
#include <coroactors/detail/stop_token.h>

namespace coroactors {

    using detail::nostopstate_t;
    using detail::nostopstate;

    using detail::stop_token;
    using detail::stop_source;
    using detail::stop_callback;

    /**
     * Returns stop token for the currently running coroutine
     */
    const stop_token& current_stop_token() noexcept {
        return detail::current_stop_token();
    }

    /**
     * Wraps awaitable with the specified stop token override
     */
    template<detail::awaitable Awaitable>
    auto with_stop_token(stop_token token, Awaitable&& awaitable) {
        return detail::with_stop_token_awaiter<Awaitable>(std::move(token), std::forward<Awaitable>(awaitable));
    }

    /**
     * scoped_stop_source calls request_stop automatically on destruction
     */
    class scoped_stop_source : public stop_source {
    public:
        scoped_stop_source() = default;
        scoped_stop_source(nostopstate_t) noexcept
            : stop_source(nostopstate)
        {}

        ~scoped_stop_source() noexcept {
            request_stop();
        }

        // Don't allow copies (to avoid multiple scopes)
        scoped_stop_source(const scoped_stop_source&) = delete;
        scoped_stop_source& operator=(const scoped_stop_source&) = delete;

        // Allow moving to a different scope
        scoped_stop_source(scoped_stop_source&& rhs) noexcept = default;
        scoped_stop_source& operator=(scoped_stop_source&& rhs) noexcept = default;
    };

} // namespace coroactors
