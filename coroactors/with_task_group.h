#pragma once
#include <coroactors/detail/with_task_group.h>
#include <coroactors/actor.h>
#include <coroactors/task_group.h>
#include <optional>

namespace coroactors {

    /**
     * Runs a callback (which may be a lambda, but must be a coroutine) with
     * a task_group<T> reference, which may be used to start additional tasks
     * concurrently and process their results in some way. All tasks in the
     * task group are cancelled and awaited before returning the callback's
     * coroutine result or rethrowing an exception.
     */
    template<class T, detail::with_task_group_callback<T> Callback>
    actor<detail::with_task_group_result_t<T, Callback>> with_task_group(Callback callback) {
        // We bind to caller context, so callback doesn't change context
        co_await actor_context::caller_context();

        // The task group we will be passing to callback
        task_group<T> group;

        using Result = detail::with_task_group_result_t<T, Callback>;
        result<Result> r;

        try {
            // Propagate cancellation to group when current call is cancelled
            stop_callback propagate(
                co_await actor_context::current_stop_token,
                [&group]() noexcept {
                    group.request_stop();
                });

            // Propagate current coroutine locals to all tasks in the task
            // group. Note we co_await all tasks before returning, so current
            // record is guaranteed to outlive all tasks in the task group.
            group.set_inherited_locals(detail::current_coroutine_local_ptr);

            if constexpr (std::is_void_v<Result>) {
                co_await callback(group);
                r.set_value();
            } else {
                r.set_value(co_await callback(group));
            }
        } catch (...) {
            r.set_exception(std::current_exception());
        }

        // When callback returns all unawaited tasks are cancelled
        group.request_stop();

        // Wait for all tasks to finish, but ignore results
        while (group) {
            (void) co_await group.next_result();
        }

        co_return r.take_value();
    }

} // namespace coroactors
