#pragma once
#include <coroactors/detail/async.h>
#include <coroactors/detail/awaiters.h>
#include <coroactors/detail/config.h>
#include <coroactors/detail/tag_invoke.h>

namespace coroactors::detail {

    struct with_task_local_guard {
        async_task* task;
        async_task_local* binding;

        with_task_local_guard(async_task* task, async_task_local* binding) noexcept
            : task(task)
            , binding(binding)
        {
            task->push_local(binding);
        }

        ~with_task_local_guard() noexcept {
            task->pop_local(binding);
        }
    };

    template<awaitable Awaitable>
    async<await_result_t<Awaitable>> with_task_local_impl(async_task_local binding, Awaitable awaitable) {
        // Making it an async coroutine is not very efficient, but state machine is too complex otherwise
        async_task* task = async_task::current();
        assert(task);
        with_task_local_guard guard(task, &binding);
        co_return co_await std::move(awaitable);
    }

} // namespace coroactors::detail
