#pragma once
#include <coroactors/detail/with_resume_callback.h>

namespace coroactors {

    /**
     * Returns a coroutine handle that, when resumed, calls the specified callback.
     *
     * The callback may optionally return a coroutine handle that will run after
     * it returns, similar to await_suspend. Caller must destroy the handle
     * when it is no longer needed, even after the coroutine is resumed.
     */
    template<class Callback>
    [[nodiscard]] std::coroutine_handle<>
    with_resume_callback(Callback&& callback)
        requires (
            detail::is_resume_callback_void<std::decay_t<Callback>> ||
            detail::is_resume_callback_handle<std::decay_t<Callback>>)
    {
        // Note: the internal coroutine stores callback copy in its arguments
        return detail::make_resume_callback_coroutine(std::forward<Callback>(callback)).handle;
    }

} // namespace coroactors
