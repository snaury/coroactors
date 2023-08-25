#pragma once
#include <coroactors/detail/awaiters.h>
#include <coroactors/task_group.h>

namespace coroactors::detail {

    /**
     * Matches callback that accepts a group lvalue and returns an awaitable
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
    using with_task_group_result_t = remove_rvalue_reference_t<
        await_result_t<with_task_group_awaitable_t<T, Callback>>>;

    /**
     * This is used as a stop_callback callback for propagating cancellation
     */
    template<class T>
    class with_task_group_request_stop {
    public:
        explicit with_task_group_request_stop(task_group<T>& group) noexcept
            : group(group)
        {}

        void operator()() noexcept {
            group.request_stop();
        }

    private:
        task_group<T>& group;
    };

} // namespace coroactors::detail
