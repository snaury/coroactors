#pragma once
#include <coroactors/actor_context_base.h>
#include <coroactors/async.h>

namespace coroactors::detail {

    template<awaitable Awaitable>
    async<await_result_t<Awaitable>> with_deadline_impl(
            actor_scheduler* scheduler,
            actor_scheduler::time_point deadline,
            Awaitable awaitable)
    {
        async_task* task = async_task::current();
        assert(task);

        if (scheduler && !task->token.stop_requested()) [[likely]] {
            scoped_stop_source source;
            stop_token token = source.get_token();
            stop_callback propagate(task->token, [&source]{
                source.request_stop();
            });

            if (!token.stop_requested()) [[likely]] {
                // We schedule a timer that stops our scoped stop source
                // There are multiple ways it may be stopped:
                // - Existing stop token is requested to stop, in that case
                //   the same token will stop our scoped stop source, and that
                //   in turn will cancel this timer.
                // - Callback is called with deadline == true because the timer
                //   expired, in that case we will stop our scoped stop source
                //   and that will eventually stop downstream activity.
                // - Callback is called with deadline == false because of stop
                //   propagation (no additional actions needed), or because
                //   scheduler doesn't support timers. In the latter case we
                //   don't want to stop our scoped stop source, it will
                //   effectively works like "infinite timeout".
                // - Downstream activity finished early, in which case our
                //   scopped stop source will stop itself, that will stop
                //   the provided token, and cancel the timer.
                scheduler->schedule(
                    [source = stop_source(source)](bool deadline) mutable {
                        if (deadline) {
                            source.request_stop();
                        }
                    },
                    deadline,
                    token);

                // Swap task token with our token and restore on the way back
                swap(task->token, token);
                scope_guard guard([&]{
                    swap(task->token, token);
                });

                co_return co_await std::move(awaitable);
            } else {
                assert(task->token.stop_requested());
            }
        }

        // Either no scheduler, or the original token has stopped already
        co_return co_await std::move(awaitable);
    }

} // namespace coroactors::detail
