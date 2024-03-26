#pragma once
#include <coroactors/actor_context_base.h>
#include <coroactors/detail/stop_token.h>
#include <coroactors/detail/tag_invoke.h>

namespace coroactors::detail {

    struct async_task_local {
        const async_task_local* next{ nullptr };
        const void* key;
        const void* value;

        async_task_local() noexcept = default;

        async_task_local(const void* key, const void* value) noexcept
            : key(key)
            , value(value)
        {}
    };

    struct async_task {
        // Points to the top-most running task
        static inline thread_local async_task* current{ nullptr };

        // Points to the next running task on the stack
        // Most of the time this is nullptr, unless tasks start recursively
        async_task* next{ nullptr };

        // Currently active stop token for cancellation
        stop_token token;

        // Currently bound actor context
        actor_context context;

        // Current head of a list of bound task_local<T> values
        const async_task_local* locals{ nullptr };

        // Becomes true the first time task is resumed by a scheduler.
        // This is used to decide when context switching should prefer post
        // over defer, since a task that just started and has never been resumed
        // is likely running recursively on top of some other activity, which
        // should be considered parallel to current task.
        bool scheduled = false;

        void enter() noexcept {
            assert(current != this);
            assert(next == nullptr);
            next = current;
            current = this;
        }

        void leave() noexcept {
            assert(current == this);
            current = next;
            next = nullptr;
        }

        void push_local(async_task_local* local) noexcept {
            assert(local->next == nullptr);
            assert(locals != local);
            local->next = locals;
            locals = local;
        }

        void pop_local(async_task_local* local) noexcept {
            assert(locals == local);
            locals = local->next;
            local->next = nullptr;
        }

        const void* find_local(const void* key) noexcept {
            for (const auto* p = locals; p; p = p->next) {
                if (p->key == key) {
                    return p->value;
                }
            }
            return nullptr;
        }
    };

    inline const void* find_task_local(const void* key) noexcept {
        if (async_task* task = async_task::current) {
            return task->find_local(key);
        } else {
            return nullptr;
        }
    }

    /**
     * Returns a stop_token of the currently running task
     */
    inline const stop_token& current_stop_token() noexcept {
        if (async_task* task = async_task::current) {
            return task->token;
        } else {
            static stop_token empty;
            return empty;
        }
    }

    /**
     * Returns an actor_context of the currently running task
     */
    inline const actor_context& current_actor_context() noexcept {
        if (async_task* task = async_task::current) {
            return task->context;
        } else {
            return no_actor_context;
        }
    }

    inline constexpr struct inherit_async_task_fn {
        template<class Awaitable>
            requires tag_invocable<inherit_async_task_fn, Awaitable&>
        void operator()(Awaitable& awaitable) const {
            tag_invoke(*this, awaitable);
        }

        template<class Awaitable>
            requires tag_invocable<inherit_async_task_fn, Awaitable&, const actor_context&>
        void operator()(Awaitable& awaitable, const actor_context& context) const {
            tag_invoke(*this, awaitable, context);
        }
    } inherit_async_task;

    template<class Awaitable>
    concept inherit_async_task_invocable =
        tag_invocable<inherit_async_task_fn, Awaitable&>;

    template<class Awaitable>
    concept inherit_async_task_with_context_invocable =
        tag_invocable<inherit_async_task_fn, Awaitable&, const actor_context&>;

} // namespace coroactors::detail
