#pragma once
#include <coroactors/actor_context.h>
#include <coroactors/detail/actor_context.h>
#include <coroactors/detail/intrusive_mailbox.h>
#include <coroactors/detail/local.h>
#include <coroactors/detail/symmetric_transfer.h>
#include <coroactors/stop_token.h>
#include <optional>

#ifndef COROACTORS_TRACK_FRAMES
#define COROACTORS_TRACK_FRAMES 0
#endif

namespace coroactors::detail {

    class actor_context_manager;

    /**
     * Usually a base class of a coroutine promise that runs in an actor context
     */
    class actor_context_frame
        : public actor_scheduler_runnable
    {
        friend actor_context_manager;

    public:
        explicit actor_context_frame(std::coroutine_handle<> self)
            : self(self)
        {}

        std::coroutine_handle<> handle() const {
            return self;
        }

        void set_stop_token_ptr(const stop_token* ptr) noexcept {
            stop_token_ptr = ptr;
        }

        void set_locals_ptr(const coroutine_local_record* ptr) noexcept {
            locals_ptr = ptr;
        }

    private:
        void run() noexcept override;

    private:
        // This is the handle to the coroutine that runs in this frame
        const std::coroutine_handle<> self;

    public:
        actor_context context;

    private:
        const stop_token* stop_token_ptr{ nullptr };
        const coroutine_local_record* locals_ptr{ nullptr };

#if COROACTORS_TRACK_FRAMES
        // The next running frame when multiple frames are running in the current thread
        actor_context_frame* next_frame{ nullptr };
#endif
    };

    /**
     * Frame management APIs for the actor context
     */
    class actor_context_manager {
        friend actor_context_frame;

    public:
        explicit actor_context_manager(const actor_context& context)
            : context(context.ptr.get())
        {}

    private:
        static void run(actor_context_frame* frame) {
#ifndef NDEBUG
            size_t saved_count = running_count();
#endif

            enter_frame(frame);
            symmetric::resume(frame->handle());

#ifndef NDEBUG
            assert(saved_count == running_count());
#endif
        }

        static void post(actor_context_state* context, actor_context_frame* frame) noexcept {
            context->scheduler.post(frame);
        }

        static void defer(actor_context_state* context, actor_context_frame* frame) noexcept {
            context->scheduler.defer(frame);
        }

    private:
        /**
         * Post the given frame to run in the scheduler
         */
        void post(actor_context_frame* frame) noexcept {
            post(context, frame);
        }

        /**
         * Defer the given frame to run in the scheduler
         */
        void defer(actor_context_frame* frame) noexcept {
            defer(context, frame);
        }

        /**
         * Either post or defer, depending on current frame nesting
         */
        void post_or_defer(actor_context_frame* frame) noexcept {
            if (running_count() > 0) [[unlikely]] {
                post(frame);
            } else {
                defer(frame);
            }
        }

        /**
         * Reschedules this frame when preempted and returns true, otherwise false
         */
        static bool maybe_preempt(actor_context_state* context, actor_context_frame* frame) noexcept {
            if (!context) {
                // Cannot preempt without a scheduler
                return false;
            }

            // When running in parallel with lower frames we use post
            if (running_count() > 0) [[unlikely]] {
                post(context, frame);
                return true;
            }

            // When preempted by the scheduler we use defer
            if (context->scheduler.preempt()) {
                defer(context, frame);
                return true;
            }

            return false;
        }

        bool maybe_preempt(actor_context_frame* frame) noexcept {
            return maybe_preempt(context, frame);
        }

        /**
         * Prepare the given frame to start in the current context
         *
         * This is used both for starting a detached coroutine, as well as
         * starting a coroutine with `co_await` from a non-actor coroutine.
         *
         * Returns the next frame to run in this thread or nullptr
         */
        actor_context_frame* prepare(actor_context_frame* frame) noexcept {
            if (!context) {
                return frame;
            }

            // Note: when the call to push_frame returns false it may
            // immediately start running in another thread and destroy both
            // this frame and context.
            if (!context->push_frame(frame)) {
                return nullptr;
            }

            // We may rarely fail to take the next frame, in which case the
            // context is unlocked and a concurrent thread is guaranteed to
            // take it instead.
            if (!(frame = context->next_frame())) {
                return nullptr;
            }

            // Note: when a new actor is detached we know it runs parallel to
            // the current activity. With `co_await` from non-actor coroutines
            // it's trickier, because we simply can't know, it could be a
            // detach_awaitable call (and thus also parallel), or it could be
            // a switch to a non-actor coroutine and then await of an actor
            // coroutine. Unfortunately we have to assume the worst.
            post(frame);
            return nullptr;
        }

    public:
        /**
         * Tries to start the given frame in this thread
         *
         * This is mainly to support `actor<T>::detach()`.
         */
        void start(actor_context_frame* frame) {
            if (auto* next = prepare(frame)) {
                run(next);
            }
        }

        /**
         * Tries to enter the given frame in this thread
         *
         * This is called when an actor coroutine starts due to `co_await` from
         * a non-actor coroutine. It always returns a valid coroutine handle,
         * which could be returned from an await_suspend method. When this
         * context locks it also starts running in the current thread,
         * otherwise the returned handle is a noop_coroutine().
         */
        std::coroutine_handle<> enter(actor_context_frame* frame) noexcept {
            if (auto* next = prepare(frame)) {
                enter_frame(next);
                return next->handle();
            } else {
                return std::noop_coroutine();
            }
        }

        /**
         * Tries to finish the given frame in this thread
         *
         * This is called when an actor coroutine finishes without any
         * continuation and needs something to continue. Returns the next
         * runnable coroutine in this context.
         */
        std::coroutine_handle<> finish(actor_context_frame* frame) noexcept {
            assert(is_running(frame) && "Calling finish() with a frame that is not running");

            leave_frame(frame);

            if (context) {
                if (auto* next = context->next_frame()) {
                    if (!maybe_preempt(next)) {
                        enter_frame(next);
                        return next->handle();
                    }
                }
            }

            return std::noop_coroutine();
        }

        /**
         * Leaves before returning to a non-actor coroutine
         */
        void leave(actor_context_frame* frame) noexcept {
            assert(is_running(frame) && "Calling leave() with a frame that is not running");

            leave_frame(frame);

            if (context) {
                if (auto* next = context->next_frame()) {
                    // Run the next frame parallel to current activity
                    post(next);
                }
            }
        }

        /**
         * Prepare the given frame for an async resume in another thread
         *
         * The frame will no longer be running in the current thread, but
         * context is left locked and may start running more coroutines.
         */
        void async_start(actor_context_frame* frame) noexcept {
            assert(is_running(frame) && "Calling async_start() with a frame that is not running");

            track_pop_frame(frame);
        }

        /**
         * Restores the given frame after an async suspend did not happen
         */
        void async_abort(actor_context_frame* frame) noexcept {
            assert(is_running() && "Calling async_abort() with a context that is not running");

            track_push_frame(frame);
        }

        /**
         * Tries to get the next coroutine from this context without leaving
         *
         * Returns noop_coroutine() and leaves when nothing is available.
         */
        std::coroutine_handle<> async_next() noexcept {
            assert(is_running() && "Calling async_next() with a context that is not running");

            leave_context();

            if (context) {
                if (auto* next = context->next_frame()) {
                    if (!maybe_preempt(next)) {
                        enter_frame(next);
                        return next->handle();
                    }
                }
            }

            return std::noop_coroutine();
        }

        /**
         * Leave this context, e.g. after start_async resumes to a different coroutine
         */
        void async_leave() noexcept {
            assert(is_running() && "calling leave() with a context that is not running");

            leave_context();

            if (context) {
                if (auto* next = context->next_frame()) {
                    // Run the next frame parallel to current activity
                    post(next);
                }
            }
        }

        /**
         * Restore the given frame for a resuming coroutine
         *
         * Returns the next runnable for the current thread, which may also
         * be a noop_coroutine(), in which case context is not restored and
         * the provided coroutine will run somewhere else.
         */
        std::coroutine_handle<> restore(actor_context_frame* frame) noexcept {
            if (!context) {
                // Enter an empty context frame
                enter_frame(frame);
                return frame->handle();
            }

            if (!context->push_frame(frame) || !(frame = context->next_frame())) {
                return std::noop_coroutine();
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
            if (maybe_preempt(frame)) {
                return std::noop_coroutine();
            }

            // This frame continues in the current thread
            enter_frame(frame);
            return frame->handle();
        }

        /**
         * Switches the given frame from this context to an empty context
         */
        void switch_to_empty(actor_context_frame* frame) noexcept {
            assert(is_running(frame) && "calling switch_to_empty() with a frame that is not running");

            if (context) {
                if (auto* next = context->next_frame()) {
                    // This activity runs parallel to current thread
                    post(next);
                }
            }

            // Note: this may cause this context to be destroyed
            frame->context = {};
        }

        /**
         * Enter from `from_context` to this context and run `frame`
         *
         * The `returning` argument specifies the direction of switching: when
         * it is true the switch is returning from a `co_await`.
         *
         * It's mostly equivalent to `async_leave()` followed by `restore()`,
         * but more efficient and has subtle differences where new continuation
         * is prioritized over additional work in the `from_context`.
         *
         * Returns the next runnable coroutine for the current thread, which
         * may also be a noop_coroutine(), in which case it's as if
         * `async_leave()` has been called.
         */
        std::coroutine_handle<> switch_from(actor_context_state* from_context,
                actor_context_frame* frame, bool returning) noexcept
        {
            assert(is_running() && "trying to switch without a context running");

            // Leave current context, so maybe_preempt doesn't always preempt
            leave_context();

            // Special case: the context is not changing
            if (context == from_context) {
                // Note: we only preempt on the return path, because otherwise
                // it is a co_await of a new actor coroutine, which might be
                // setting up some async activities. We don't want to cause
                // thrashing where we resume later only to immediately leave
                // the context. A new coroutine will likely suspend soon by
                // making more calls, or return from at least one call, and
                // that's when we would try to preempt.
                if (returning && maybe_preempt(frame)) {
                    return std::noop_coroutine();
                }

                enter_frame(frame);
                return frame->handle();
            }

            // Special case: switching to an empty context
            if (!context) {
                if (auto* next = from_context->next_frame()) {
                    // Run the next frame parallel to the new frame
                    post(from_context, next);
                }

                // New frame continues in this thread
                enter_frame(frame);
                return frame->handle();
            }

            // Take the next runnable from the current context. It will either
            // stay locked with more work, or will unlock and possibly run in
            // another thread. Note: we cannot attempt running in a new context
            // first, because when a coroutine starts running it may free
            // local variables which are keeping contexts in memory.
            actor_context_frame* more = nullptr;
            if (from_context) {
                // Note: when more != nullptr context is locked and safe
                more = from_context->next_frame();
            }

            auto from_continue = [from_context, more]() -> std::coroutine_handle<> {
                if (!more || maybe_preempt(from_context, more)) {
                    return std::noop_coroutine();
                }

                // The original context continues in this thread
                enter_frame(more);
                return more->handle();
            };

            // Try running `frame` in this context. It will either lock and
            // give us a frame (likely the same we pushed, but not guaranteed),
            // which we will try running in the current thread, or it will be
            // added to the queue and we will try running some work from the
            // original context instead.
            if (context->push_frame(frame) && (frame = context->next_frame())) {
                // Special case: check if scheduler is changing, in which case
                // we must always post new frame to the new scheduler, since it
                // likely runs in a different thread pool.
                if (from_context && &from_context->scheduler != &context->scheduler) [[unlikely]] {
                    post(frame);
                    return from_continue();
                }

                // Check for preemption by this context. We do this either on
                // the return path (see a comment above), or when switching
                // from an empty context, because it should behave similar to
                // restore().
                if ((returning || !from_context) && maybe_preempt(frame)) {
                    if (more) {
                        post(from_context, more);
                    }
                    return std::noop_coroutine();
                }

                if (more) {
                    post(from_context, more);
                }

                // New frame continues in this thread
                enter_frame(frame);
                return frame->handle();
            }

            return from_continue();
        }

        std::coroutine_handle<> switch_context(actor_context_frame* frame,
                bool returning) noexcept
        {
            assert(is_running(frame) && "Switching context for a frame that is not running");

            auto saved = std::move(frame->context);
            frame->context.ptr.reset(context);
            track_pop_frame(frame);

            return switch_from(saved.ptr.get(), frame, returning);
        }

        std::coroutine_handle<> switch_frame(actor_context_frame* from_frame,
                actor_context_frame* to_frame, bool returning) noexcept
        {
            assert(is_running(from_frame) && "Switching from frame that is not running");

            actor_context_state* from_context = from_frame->context.ptr.get();
            track_pop_frame(from_frame);

            return switch_from(from_context, to_frame, returning);
        }

        /**
         * Yields to other frames in this context
         */
        std::coroutine_handle<> yield(actor_context_frame* frame) noexcept {
            assert(is_running(frame) && "calling yield(h) with a frame that is not running");

            if (!context) {
                return frame->handle();
            }

            track_pop_frame(frame);

            // Push current frame to the end of the queue
            if (context->push_frame(frame)) [[unlikely]] {
                assert(false && "unexpected push_frame() lock on an already locked context");
            }

            if (!(frame = context->next_frame())) {
                leave_context();
                return std::noop_coroutine();
            }

            if (maybe_preempt(frame)) {
                leave_context();
                return std::noop_coroutine();
            }

            // Run the next frame in this thread
            track_push_frame(frame);
            return frame->handle();
        }

        /**
         * Forces preemption without yielding in the current context
         */
        std::coroutine_handle<> preempt(actor_context_frame* frame) noexcept {
            assert(is_running(frame) && "calling preempt() with a frame that is not running");

            if (!context) {
                return frame->handle();
            }

            leave_frame(frame);
            post_or_defer(frame);
            return std::noop_coroutine();
        }

    private:
        static size_t running_count() noexcept {
            return running_frames;
        }

        static bool is_running() noexcept {
            return running_frames > 0;
        }

        static void enter_context() noexcept {
            running_frames++;
        }

        static void leave_context() noexcept {
            assert(running_frames > 0);
            running_frames--;
        }

        static bool is_running(actor_context_frame* frame) noexcept {
#if COROACTORS_TRACK_FRAMES
            return running_frame == frame;
#else
            (void)frame;
            return is_running();
#endif
        }

        static void track_push_frame(actor_context_frame* frame) noexcept {
            std::swap(current_stop_token_ptr, frame->stop_token_ptr);
            std::swap(current_coroutine_local_ptr, frame->locals_ptr);
#if COROACTORS_TRACK_FRAMES
            frame->next_frame = running_frame;
            running_frame = frame;
#else
            (void)frame;
#endif
        }

        static void track_pop_frame(actor_context_frame* frame) noexcept {
#if COROACTORS_TRACK_FRAMES
            assert(running_frame == frame);
            running_frame = frame->next_frame;
            frame->next_frame = nullptr;
#else
            (void)frame;
#endif
            std::swap(current_coroutine_local_ptr, frame->locals_ptr);
            std::swap(current_stop_token_ptr, frame->stop_token_ptr);
        }

    public:
        static void enter_frame(actor_context_frame* frame) noexcept {
            enter_context();
            track_push_frame(frame);
        }

        static void leave_frame(actor_context_frame* frame) noexcept {
            track_pop_frame(frame);
            leave_context();
        }

    private:
        // Tracks the number of currently running context frames in the current thread
        static inline thread_local size_t running_frames{ 0 };

#if COROACTORS_TRACK_FRAMES
        // The top running frame in the current thread
        static inline thread_local actor_context_frame* running_frame{ nullptr };
#endif

    private:
        actor_context_state* context;
    };

    inline void actor_context_frame::run() noexcept {
        actor_context_manager::run(this);
    }

    inline bool actor_context_state::push_frame(actor_context_frame* frame) noexcept {
        return mailbox_.push(frame);
    }

    inline actor_context_frame* actor_context_state::next_frame() noexcept {
        return mailbox_.pop();
    }

} // namespace coroactors::detail

namespace coroactors {

    inline detail::actor_context_manager actor_context::manager() const {
        return detail::actor_context_manager(*this);
    }

} // namespace coroactors
