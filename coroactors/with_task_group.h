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

        // Current stop token we inherited from the caller
        const stop_token& token = co_await actor_context::current_stop_token;

        // Propagate cancellation to group when current call is cancelled
        std::optional<stop_callback<detail::with_task_group_request_stop<T>>> stop;
        if (token.stop_possible()) {
            stop.emplace(token, group);
        }

        using Result = detail::with_task_group_result_t<T, Callback>;
        detail::result<Result> r;

        try {
            if constexpr (std::is_void_v<Result>) {
                co_await callback(group);
                r.set_value();
            } else {
                r.set_value(co_await callback(group));
            }
        } catch (...) {
            r.set_exception(std::current_exception());
        }

        // Remove cancellation propagation as soon as possible
        stop.reset();

        // When callback returns all unawaited tasks are cancelled
        group.request_stop();

        // Wait for all tasks to finish, but ignore results
        while (group) {
            (void) co_await group.next_result();
        }

        co_return r.take_value();
    }

} // namespace coroactors
