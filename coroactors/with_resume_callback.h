#pragma once
#include <coroactors/detail/with_resume_callback.h>

namespace coroactors {

    /**
     * Returns a coroutine handle that, when resumed, calls the specified callback.
     *
     * Callback may optionally return a coroutine handle that will run after callback returns.
     */
    template<class Callback>
    [[nodiscard]] std::coroutine_handle<>
    with_resume_callback(Callback&& callback)
        requires (
            detail::is_resume_callback_void<std::decay_t<Callback>> ||
            detail::is_resume_callback_handle<std::decay_t<Callback>>)
    {
        // Note: the internal coroutine stores callback in its arguments
        return detail::make_resume_callback_coroutine(std::forward<Callback>(callback)).handle;
    }

} // namespace coroactors
