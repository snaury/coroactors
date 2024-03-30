#pragma once
#include <coroactors/actor_context_base.h>
#include <coroactors/detail/config.h>
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

        COROACTORS_NOINLINE
        static async_task* current() noexcept {
            return current_;
        }

        COROACTORS_NOINLINE
        void enter() noexcept {
            if (next != reinterpret_cast<void*>(InactiveMarker)) [[unlikely]] {
                fail("cannot enter async_task more than once");
            }
            next = current_;
            current_ = this;
        }

        COROACTORS_NOINLINE
        void leave() noexcept {
            if (current_ != this) [[unlikely]] {
                fail("cannot leave async_task that is not on top of the stack");
            }
            current_ = reinterpret_cast<async_task*>(next);
            next = reinterpret_cast<void*>(InactiveMarker);
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

    private:
        /**
         * When used in a noexcept method causes std::terminate with a message
         */
        [[noreturn]] static void fail(const char* message) {
            throw std::logic_error(message);
        }

    private:
        static constexpr uintptr_t InactiveMarker = 1;

        // Points to the next running task on the stack
        // This uses a special marker when task is not on the stack
        void* next{ reinterpret_cast<void*>(InactiveMarker) };

    private:
        // Points to the top-most running task
        static inline thread_local async_task* current_{ nullptr };
    };

    inline const void* find_task_local(const void* key) noexcept {
        if (async_task* task = async_task::current()) {
            return task->find_local(key);
        } else {
            return nullptr;
        }
    }

    /**
     * Returns a stop_token of the currently running task
     */
    inline const stop_token& current_stop_token() noexcept {
        if (async_task* task = async_task::current()) {
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
        if (async_task* task = async_task::current()) {
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
