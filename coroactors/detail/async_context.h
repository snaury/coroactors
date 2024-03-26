#pragma once
#include <coroactors/actor_context_base.h>
#include <coroactors/detail/async_task.h>

namespace coroactors::detail {

    /**
     * A collection of helper methods for dealing with context switches
     */
    class async_context_manager {
    public:
        /**
         * Enter the specified context with the specified runnable, e.g.
         * because a previously inprogress async operation has resumed.
         *
         * Returns true when the specified context was successfully locked
         * for execution, and the specified runnable should run in the current
         * thread. Otherwise returns false, and the specified runnable may run
         * concurrently in another thread or scheduled for execution.
         */
        static bool enter(
                const actor_context& context,
                actor_scheduler_runnable& r,
                bool initial = false) noexcept
        {
            return do_enter(context.ptr.get(), &r, initial);
        }

        /**
         * Atomically leave `from_context` and enter `to_context` with the
         * specified runnable. When `returning` is true, this function will
         * consider this transfer to be a return from one coroutine to another.
         */
        static bool transfer(
                const actor_context& from_context,
                const actor_context& to_context,
                actor_scheduler_runnable& r,
                bool returning) noexcept
        {
            return do_transfer(from_context.ptr.get(), to_context.ptr.get(), &r, returning);
        }

        /**
         * Leave the specified context, scheduling other euqueued runnables
         */
        static void leave(const actor_context& from_context) noexcept {
            return do_leave(from_context.ptr.get());
        }

        /**
         * Yields thread to other runnables, including those in the same context
         */
        static bool yield(const actor_context& context, actor_scheduler_runnable& r) noexcept {
            return do_yield(context.ptr.get(), &r);
        }

        /**
         * Yields thread to other runnables, but not those in the same context
         */
        static bool preempt(const actor_context& context, actor_scheduler_runnable& r) noexcept {
            return do_preempt(context.ptr.get(), &r);
        }

    private:
        static bool do_enter(
                actor_context_state* context,
                actor_scheduler_runnable* r,
                bool initial) noexcept
        {
            // Continue running without a context
            if (!context) {
                return true;
            }

            // Try pushing runnable to the context queue, which will run
            // eventually or concurrently when it is locked by someone else.
            // Otherwise we locked and now own the context with some runnable.
            actor_scheduler_runnable* next;
            if (!context->push_runnable(r) || !(next = context->next_runnable())) {
                return false;
            }

            // Task is entering a non-empty context for the first time
            if (initial) {
                context->scheduler.post(next);
                return false;
            }

            // We have returned from an async activity, and usually don't want
            // to block a caller of resume(). This includes completed network
            // calls (in which case we want to defer, as it is a continuation
            // of the return), but also calls to `continuation<T>::resume`,
            // which really starts a new activity parallel to resumer. We rely
            // on common logic in maybe_preempt here and scheduler preempting
            // in unexpected threads and handlers. This should be consistent
            // when we return to actor directly, and when we return to actor
            // via a context-less actor call.
            if (maybe_preempt(context, next)) {
                return false;
            }

            // We pulled a different runnable, defer it to run in the same thread
            if (next != r) [[unlikely]] {
                context->scheduler.defer(next);
                return false;
            }

            // We may resume in the current thread
            return true;
        }

        static bool do_transfer(
                actor_context_state* from_context,
                actor_context_state* to_context,
                actor_scheduler_runnable* r,
                bool returning) noexcept
        {
            // Special case: the context is not changing
            if (from_context == to_context) {
                // Note: we only preempt on the return path, because otherwise
                // it is a co_await of a new actor coroutine, which might be
                // setting up some async activities. We don't want to cause
                // thrashing where we resume later only to immediately leave
                // the context. A new coroutine will likely suspend soon by
                // making more calls, or return from at least one call, and
                // that's when we would try to preempt.
                if (returning && maybe_preempt(to_context, r)) {
                    return false;
                }

                // New runnable continues in this thread
                return true;
            }

            // Special case: switching to an empty context
            if (!to_context) {
                do_leave(from_context);
                return true;
            }

            // Take the next runnable from the current context. It will either
            // stay locked with more != nullptr, or will unlock and possibly run
            // in another thread. Note: we cannot attempt running in the new
            // context first, because when a new coroutine starts running it
            // may free memory we are pointing to.
            actor_scheduler_runnable* more = nullptr;
            if (from_context) {
                // Note: when more != nullptr from_context is locked and safe
                // Otherwise we rely on it staying in memory until `r` is executed
                more = from_context->next_runnable();
            }

            // Try running `r` in the new context
            actor_scheduler_runnable* next = nullptr;
            if (to_context->push_runnable(r) && (next = to_context->next_runnable())) {
                // Special case: check if scheduler is changing, in which case
                // we must always post new runnable to the new scheduler, since
                // it's likely running in a different thread pool.
                if (from_context && &from_context->scheduler != &to_context->scheduler) [[unlikely]] {
                    to_context->scheduler.post(next);
                    if (more) {
                        from_context->scheduler.defer(more);
                    }
                    return false;
                }

                // Check for preemption by the new context. We do this either on
                // the return path (see a comment above), or when switching
                // from an empty context, because it should behave similar to
                // enter().
                if ((returning || !from_context) && maybe_preempt(to_context, next)) {
                    if (more) {
                        from_context->scheduler.post(more);
                    }
                    return false;
                }

                // When the next runnable is the same we got initially we want
                // to continue in the current thread. The other runnable is
                // posted and will run in parallel.
                if (next == r) [[likely]] {
                    if (more) {
                        from_context->scheduler.post(more);
                    }
                    return true;
                }

                // Otherwise we want the next runnable in the new context to
                // run in the current thread, and the other runnable to run
                // in parallel.
                to_context->scheduler.defer(next);
                if (more) {
                    from_context->scheduler.post(more);
                }
                return false;
            }

            // The next runnable is scheduled to run somewhere else
            // Try scheduling `more` to run in the current thread
            if (more) {
                from_context->scheduler.defer(more);
            }
            return false;
        }

        static void do_leave(actor_context_state* from_context) noexcept {
            actor_scheduler_runnable* next;
            if (from_context && (next = from_context->next_runnable())) {
                from_context->scheduler.post(next);
            }
        }

        static bool do_yield(actor_context_state* context, actor_scheduler_runnable* r) noexcept {
            if (!context) {
                return true;
            }

            if (context->push_runnable(r)) [[unlikely]] {
                assert(false && "unexpected push_runnable() lock on an already locked context");
            }

            actor_scheduler_runnable* next;
            if (!(next = context->next_runnable())) {
                return false;
            }

            post_or_defer(context, r);
            return false;
        }

        static bool do_preempt(actor_context_state* context, actor_scheduler_runnable* r) noexcept {
            if (!context) {
                return true;
            }

            post_or_defer(context, r);
            return false;
        }

    private:
        static bool maybe_preempt(
                actor_context_state* context,
                actor_scheduler_runnable* r) noexcept
        {
            // Cannot preempt without a scheduler
            if (!context) {
                return false;
            }

            // When another task is running we use post (new parallel activity)
            if (async_task::current != nullptr) [[unlikely]] {
                context->scheduler.post(r);
                return true;
            }

            // When preempted by the scheduler we use defer
            if (context->scheduler.preempt()) {
                context->scheduler.defer(r);
                return true;
            }

            return false;
        }

        static void post_or_defer(actor_context_state* context, actor_scheduler_runnable* r) noexcept {
            if (async_task::current != nullptr) [[unlikely]] {
                context->scheduler.post(r);
            } else {
                context->scheduler.defer(r);
            }
        }
    };

    inline bool actor_context_state::push_runnable(actor_scheduler_runnable* r) noexcept {
        return mailbox_.push(r);
    }

    inline actor_scheduler_runnable* actor_context_state::next_runnable() noexcept {
        return mailbox_.pop();
    }

} // namespace coroactors::detail
