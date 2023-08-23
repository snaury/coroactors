#pragma once
#include <coroactors/detail/with_resume_callback.h>

namespace coroactors {

    /**
     * Returns a coroutine handle that, when resumed, calls the specified callback.
     *
     * Callback may optionally return a coroutine handle that will run after callback returns.
     */
    template<class Callback>
    std::coroutine_handle<> with_resume_callback(Callback&& callback)
        requires (
            detail::is_resume_callback_void<Callback> ||
            detail::is_resume_callback_handle<Callback>)
    {
        // Note: if callback is an rvalue then Callback will not be a reference
        // and we will move the object to coroutine frame. However in cases
        // where callback is an lvalue reference Callback will be a reference
        // and we will store that reference in the frame, not an object copy.
        return detail::make_resume_callback_coroutine<Callback>(std::forward<Callback>(callback)).handle;
    }

} // namespace coroactors
