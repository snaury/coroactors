#pragma once
#include <coroactors/detail/awaiters.h>
#include <coroactors/task_group.h>

namespace coroactors::detail {

    /**
     * Matches callback that accepts a group lvalue reference and returns an awaitable
     */
    template<class Callback, class T>
    concept with_task_group_callback = requires(Callback callback, task_group<T> group) {
        { callback(group) } -> awaitable;
    };

    /**
     * The type of awaitable returned by the callback
     */
    template<class T, with_task_group_callback<T> Callback>
    using with_task_group_awaitable_t = decltype(std::declval<Callback&>()(std::declval<task_group<T>&>()));

    /**
     * The type of result returned from an awaitable
     */
    template<class T, with_task_group_callback<T> Callback>
    using with_task_group_result_t = std::decay_t<
        await_result_t<with_task_group_awaitable_t<T, Callback>>>;

} // namespace coroactors::detail
