#pragma once
#include <coroactors/actor_scheduler.h>
#include <coroactors/detail/awaiters.h>
#include <coroactors/detail/mailbox.h>
#include <coroactors/intrusive_ptr.h>
#include <atomic>
#include <cassert>
#include <optional>

/**
 * When this feature is enabled actor context will track which exact context
 * is running at any given time, and verify they match on context switching.
 * This is very expensive however and disabled by default.
 */
#ifndef COROACTORS_EXACT_RUNNING_CONTEXT
#define COROACTORS_EXACT_RUNNING_CONTEXT 0
#endif

namespace coroactors {

    class actor_context;

} // namespace coroactors

namespace coroactors::detail {

    class actor_context_manager;

    class actor_context_state final : public intrusive_atomic_base<actor_context_state> {
        friend actor_context;
        friend actor_context_manager;

    public:
        explicit actor_context_state(class actor_scheduler& s)
            : scheduler(s)
        {
            // Change mailbox to initially unlocked
            if (!mailbox_.try_unlock()) [[unlikely]] {
                throw std::logic_error("unexpected failure to initially unlock the mailbox");
            }
        }

        ~actor_context_state() noexcept {
#if COROACTORS_EXACT_RUNNING_CONTEXT
            assert(running_top != this && !prev && !next_count);
#endif
        }

#if COROACTORS_EXACT_RUNNING_CONTEXT
    private:
        /**
         * Returns the currently running state pointer
         */
        static actor_context_state* running_ptr() noexcept {
            return running_empty_frames == 0 ? running_top : nullptr;
        }
#endif

        /**
         * Returns true when the specified context is currently running
         */
        static bool is_running(actor_context_state* ptr) noexcept {
#if COROACTORS_EXACT_RUNNING_CONTEXT
            return running_frames > 0 && ptr == running_ptr();
#else
            // Assume it is running as long as anything is running
            return running_frames > 0;
#endif
        }

    private:
        /**
         * Add a new continuation to this actor context
         *
         * Returns a valid handle if this context becomes locked and runnable
         * as the result of this push, or an invalid handle when it is locked
         * and/or running, possibly in another thread.
         */
        std::coroutine_handle<> push(std::coroutine_handle<> c) {
            assert(c && "Attempt to push a nullptr coroutine handle");
            if (mailbox_.push(c)) {
                std::coroutine_handle<> k = mailbox_.pop_default();
                assert(k == c);
                return k;
            } else {
                return nullptr;
            }
        }

        /**
         * Returns the next continuation from this actor context or nullptr
         *
         * This context must be locked and running by the caller, otherwise
         * the behavior is undefined. When an invalid handle is returned the
         * context is unlocked and no longer runnable, which may become
         * locked by another push, even concurrently in another thread.
         */
        std::coroutine_handle<> pop() {
            std::coroutine_handle<> k = mailbox_.pop_default();
            return k;
        }

    private:
        /**
         * Unconditionally enters a context
         */
        static void enter_unconditionally(actor_context_state* ptr) noexcept {
            running_frames++;
#if COROACTORS_EXACT_RUNNING_CONTEXT
            if (running_top) {
                running_top->next_count++;
            }
            if (ptr) {
                // Entering a non-empty frame
                assert(running_top != ptr && !ptr->prev && !ptr->next_count);
                ptr->prev = running_top;
                running_top = ptr;
                running_empty_frames = 0;
            } else {
                // Entering an empty frame
                running_empty_frames++;
                if (!running_top) {
                    bottom_empty_frames++;
                }
            }
#else
            (void)ptr;
#endif
        }

        /**
         * Unconditionally leaves a context
         */
        static void leave_unconditionally(actor_context_state* ptr) noexcept {
            running_frames--;
#if COROACTORS_EXACT_RUNNING_CONTEXT
            assert(ptr == running_ptr());
            if (ptr) {
                running_top = ptr->prev;
                ptr->prev = nullptr;
                if (running_top) {
                    running_top->next_count--;
                    running_empty_frames = running_top->next_count;
                } else {
                    running_empty_frames = bottom_empty_frames;
                }
            } else {
                assert(running_empty_frames > 0);
                running_empty_frames--;
                if (running_top) {
                    running_top->next_count--;
                } else {
                    bottom_empty_frames--;
                }
            }
#else
            (void)ptr;
#endif
        }

        /**
         * Unconditionally changes a currently running `from` context to `ptr`
         */
        static void switch_unconditionally(actor_context_state* from, actor_context_state* ptr) noexcept {
#if COROACTORS_EXACT_RUNNING_CONTEXT
            assert(from == running_ptr() && !from->next_count);
            if (from == ptr) [[unlikely]] {
                // Be on the safe side, but calling code checks for this
            } else if (from && ptr) {
                // Easy case, just changing pointers
                actor_context_state* prev = from->prev;
                from->prev = nullptr;
                ptr->prev = prev;
                running_top = ptr;
            } else {
                // Hard to optimize, combine leave and enter
                leave_unconditionally(from);
                enter_unconditionally(ptr);
            }
#else
            (void)from;
            (void)ptr;
#endif
        }

    private:
        struct frame_guard {
            size_t saved_running_frames;
#if COROACTORS_EXACT_RUNNING_CONTEXT
            actor_context_state* saved_running_top;
            size_t saved_running_empty;
            size_t saved_bottom_empty;
#endif

            frame_guard() noexcept
                : saved_running_frames(running_frames)
#if COROACTORS_EXACT_RUNNING_CONTEXT
                , saved_running_top(running_top)
                , saved_running_empty(running_empty_frames)
                , saved_bottom_empty(bottom_empty_frames)
#endif
            {}

            frame_guard(const frame_guard&) = delete;
            frame_guard& operator=(const frame_guard&) = delete;

            ~frame_guard() noexcept {
                assert(running_frames == saved_running_frames);
#if COROACTORS_EXACT_RUNNING_CONTEXT
                // Silently cleanup any leftover frames in release builds
                if (running_frames > saved_running_frames) [[unlikely]] {
                    do {
                        leave_unconditionally(running_ptr());
                    } while (running_frames > saved_running_frames);
                }
#endif
                running_frames = saved_running_frames;
#if COROACTORS_EXACT_RUNNING_CONTEXT
                running_top = saved_running_top;
                running_empty_frames = saved_running_empty;
                bottom_empty_frames = saved_bottom_empty;
#endif
            }
        };

    private:
        // Tracks the number of running contexts on the current stack
        static inline thread_local size_t running_frames{ 0 };

#if COROACTORS_EXACT_RUNNING_CONTEXT
        // Tracks the top running non-empty context
        static inline thread_local actor_context_state* running_top{ nullptr };

        // Tracks the number of running empty contexts at the top of the stack
        static inline thread_local size_t running_empty_frames{ 0 };

        // Tracks the number of empty frames at the bottom of the stack
        static inline thread_local size_t bottom_empty_frames{ 0 };
#endif

    private:
        actor_scheduler& scheduler;
        mailbox<std::coroutine_handle<>> mailbox_;
#if COROACTORS_EXACT_RUNNING_CONTEXT
        actor_context_state* prev{ nullptr };
        size_t next_count = 0;
#endif
    };

    /**
     * Continuation management APIs so we don't clutter the class itself
     */
    class actor_context_manager {
        friend class ::coroactors::actor_context;

        explicit constexpr actor_context_manager(actor_context_state* ptr) noexcept
            : ptr(ptr)
        {}

    private:
        static void fail(const char* message) {
            // Note: this function throws an exception and immediately calls
            // terminate, so the message would be printed to the console.
            try {
                throw std::logic_error(message);
            } catch(...) {
                std::terminate();
            }
        }

    private:
        /**
         * Verifies this context is running in the current thread, otherwise aborts
         */
        void verify_running(const char* message) const {
            if (!actor_context_state::is_running(ptr)) [[unlikely]] {
                fail(message);
            }
        }

    private:
        /**
         * Post coroutine h to run on this context
         */
        void post(std::coroutine_handle<> h) const {
            // Note: `std::function` over a lambda capture with two pointers
            // usually doesn't allocate any additional memory. We also don't
            // need to add_ref the state, because the coroutine must keep a
            // reference to context while it's running.
            ptr->scheduler.post([h, ptr = ptr] {
                actor_context_manager(ptr).resume(h);
            });
        }

        /**
         * Defer coroutine h to run on this context
         */
        void defer(std::coroutine_handle<> h) const {
            // Note: `std::function` over a lambda capture with two pointers
            // usually doesn't allocate any additional memory. We also don't
            // need to add_ref the state, because the coroutine must keep a
            // reference to context while it's running.
            ptr->scheduler.defer([h, ptr = ptr] {
                actor_context_manager(ptr).resume(h);
            });
        }

        /**
         * Returns true when switches to this context should be preempted
         */
        bool preempt() const {
            if (!ptr) [[unlikely]] {
                // Cannot preempt without a scheduler
                return false;
            }

            if (actor_context_state::running_frames > 1) [[unlikely]] {
                // More than one frame running, always preempt
                return true;
            }

            return ptr->scheduler.preempt();
        }

    public:
        /**
         * Resumes a coroutine h, which is expecting to run on this context
         *
         * Context must be exclusively locked to caller, e.g. by a previous
         * call to enter. Primarily used by actor schedulers to run actor
         * coroutines. The resumed coroutine is expected to correctly leave
         * this context before before returning.
         */
        void resume(std::coroutine_handle<> h) const {
            actor_context_state::frame_guard guard;
            actor_context_state::enter_unconditionally(ptr);
            h.resume();
        }

        /**
         * Tries to enter this context with a coroutine h
         *
         * This is called when an actor coroutine attempts to start in some
         * datached way (e.g. `actor<T>::detach()`). When it returns a valid
         * coroutine handle the context is locked and the result must be
         * resumed using actor_context::resume or scheduled to a scheduler.
         *
         * When optional use_scheduler is false this method will never post
         * provided coroutine to a scheduler, but it may still return an
         * invalid handle when this context is currently running and cannot
         * be locked.
         */
        std::coroutine_handle<> enter_for_resume(std::coroutine_handle<> h, bool use_scheduler = true) const {
            if (!ptr) {
                return h;
            }

            // Note: when the call to push returns nullptr it may immediately
            // start running in another thread and destroy this context.
            if (auto next = ptr->push(h)) {
                // Entering actors may lead to a long chain of context
                // switches which is undesirable when a call is detached or
                // added to a task group. This is impossible to detect with
                // generic coroutines, however with actors we can, simply
                // by detecting that this is done while another actor is
                // currently running. Otherwise actor scheduler is supposed
                // to preempt chains of context switches from happening in
                // an unexpected state.
                if (use_scheduler && actor_context_state::running_frames > 0) {
                    post(next);
                    return {};
                }

                return next;
            }

            return {};
        }

        /**
         * Tries to enter this context with a coroutine h
         *
         * This is called when an actor coroutine starts due to co_await
         * from another non-actor coroutine. It always returns a valid
         * coroutine handle, which should be returned from an await_suspend
         * method. When this context is locked it also becomes running in
         * the current thread, otherwise the returned handle is
         * noop_coroutine and context will not be running.
         */
        std::coroutine_handle<> enter_for_await(std::coroutine_handle<> h) const {
            if (auto handle = enter_for_resume(h)) {
                actor_context_state::enter_unconditionally(ptr);
                return handle;
            } else {
                return std::noop_coroutine();
            }
        }

        /**
         * Tries to get the next runnable from this context without leaving
         *
         * Returns noop_coroutine() and leaves when nothing is available.
         */
        std::coroutine_handle<> next() const {
            verify_running("calling next() with a context that is not running");

            // Leave first because pop() may invalidate current context
            actor_context_state::leave_unconditionally(ptr);

            if (ptr) {
                if (auto next = ptr->pop()) {
                    if (!preempt()) {
                        // Re-enter and continue in the same thread
                        actor_context_state::enter_unconditionally(ptr);
                        return next;
                    }

                    // We always defer because we don't create new threads of activity here
                    defer(next);
                }
            }

            return std::noop_coroutine();
        }

        /**
         * Leave this context, e.g. after starting some async activity
         */
        void leave() const {
            verify_running("calling leave() with a context that is not running");

            // Leave first becasue pop() may invalidate current context
            actor_context_state::leave_unconditionally(ptr);

            if (ptr) {
                if (auto next = ptr->pop()) {
                    // Run the next continuation parallel to current activity
                    post(next);
                }
            }
        }

        /**
         * Restores this context for a resuming coroutine h
         *
         * Returns the next runnable for the current thread, which may also
         * be a noop_coroutine(), in which case context is not restored and
         * the provided coroutine will run on a scheduler.
         */
        std::coroutine_handle<> restore(std::coroutine_handle<> h) const {
            if (!ptr) {
                // Enter an empty context frame
                actor_context_state::enter_unconditionally(ptr);
                return h;
            }

            if (auto next = ptr->push(h)) {
                // We have returned from an async activity, but we don't
                // want to block caller of resume(). This may include
                // completed network calls (in which case we want to defer,
                // as it is a continuation of the return), but also calls to
                // continuation<T>::resume, which really start a new
                // activity parallel to resumer. We use heuristics here to
                // check if some actor is already running on the stack, and
                // when it does it usually means this was a nested resume
                // and we need to post. This may be wrong when resume is
                // called from non-actor coroutines, but we may hope
                // scheduler preemption would kick in.
                if (actor_context_state::running_frames > 0) {
                    post(next);
                } else {
                    defer(next);
                }
            }

            return std::noop_coroutine();
        }

        /**
         * Switches to this context from context `from` with a coroutine `h`
         *
         * The `returning` argument specifies the direction of switching: when
         * it is true the switch is returning from co_await, and vice versa.
         *
         * This is mostly equivalent to leave() followed by restore(), but
         * it is more efficient and has subtle differences where new
         * continuation is prioritized over additional work in the `from`
         * context.
         *
         * Returns the next runnable for the current thread, which may also
         * be a noop_coroutine(), in which case it's as if leave() has been
         * called.
         *
         * Note: when this context is empty `h` may be an invalid handle
         * and is simply returned after switching. Useful to avoid
         * suspending, and when there is no h in that case.
         *
         */
        std::coroutine_handle<> switch_from(actor_context_manager from,
                std::coroutine_handle<> h, bool returning) const
        {
            from.verify_running("calling switch_from() with a context that is not running");

            if (from.ptr == ptr) {
                // Context is not changing
                return h;
            }

            if (!ptr) {
                // Switching to a context without a scheduler. We need to
                // switch context first, because pop may unlock and allow
                // current context to run in another thread.
                actor_context_state::switch_unconditionally(from.ptr, nullptr);

                if (auto next = from.ptr->pop()) {
                    // Run the next activity parallel to h
                    from.post(next);
                }

                // Continue with this empty context
                return h;
            }

            assert(h && "Switching context with an invalid handle");

            if (!from.ptr) {
                // Leave current context first
                actor_context_state::leave_unconditionally(nullptr);

                // Switching from empty to non-empty context, so it is
                // similar to entering, but with a thread of activity
                // inherited from the actor we are switching from. The
                // downside is that we could be switching from some context-
                // less wrapper around this actor, which may have been
                // detached or added to a task group, and then it's really
                // a parallel activity to some initial caller. We use
                // heuristics here to detect when another actor is running
                // underneath the current context, similar to what enter()
                // does. But note we need to account for the current
                // context occupying a frame.
                if (auto next = ptr->push(h)) {
                    // Check for one more frame besides ours on the stack
                    if (actor_context_state::running_frames > 1) {
                        // Post this parallel activity and return to caller
                        post(next);
                        return std::noop_coroutine();
                    }

                    // Similar to restore() on the return path we assume we are
                    // returning from some async activity and don't want to
                    // start monopolizing this thread, which is likely stuck
                    // in resume() call in some handler.
                    if (returning) {
                        // Defer this activity and return to caller
                        defer(next);
                        return std::noop_coroutine();
                    }

                    // Enter new context without involving the scheduler
                    actor_context_state::enter_unconditionally(ptr);
                    return next;
                }

                // Continuation may run in another thread
                return std::noop_coroutine();
            }

            // We need leave current context and grab the next task from it
            // before we do anything else. This is because current context may
            // be owned by the only reference in the current frame, which could
            // be destroyed as soon as the continuation starts running in
            // another thread. By leaving and potentially unlocking first we
            // make sure we won't touch current context when it's unlocked.
            // However when there is one more task in the current context we
            // know it's supposed to keep itself alive.
            actor_context_state::leave_unconditionally(from.ptr);

            // This will be valid only when current context is still locked
            auto more = from.ptr->pop();

            // We switch between two different contexts. First we need to
            // push continuation to this context's mailbox, and note that
            // a successful push locks this context and implies it starts
            // running, which forks a parallel activity unless current
            // context is empty and unlocks. When push returns nullptr
            // however it implies this context is running somewhere else,
            // and may immediately become invalid and free, so we cannot
            // work with ptr after that.
            if (auto next = ptr->push(h)) {
                // We have both contexts locked for now
                // Defer when preempted (it's a continuation of caller)
                if (preempt()) {
                    // We defer current task, because it's a continuation
                    // of the caller and should run before other tasks
                    // from the current context.
                    defer(next);

                    // Other activities fork from the caller
                    if (more) {
                        from.post(more);
                    }

                    return std::noop_coroutine();
                }

                // Other activities fork from the caller
                if (more) {
                    from.post(more);
                }

                // Enter this context with the continuation
                actor_context_state::enter_unconditionally(ptr);
                return next;
            }

            if (!more) {
                // We have nothing to run in this thread
                return std::noop_coroutine();
            }

            if (from.preempt()) {
                // We are preempted, and we should defer, because a failed
                // push did not introduce new threads of activity (it was
                // running somewhere else already), and we are continuing
                // the same activity here.
                from.defer(more);
                return std::noop_coroutine();
            }

            // Re-enter and continue in the same thread
            actor_context_state::enter_unconditionally(from.ptr);
            return more;
        }

        /**
         * Yields to other continuations in the current context
         */
        std::coroutine_handle<> yield(std::coroutine_handle<> h) {
            verify_running("calling yield(c) with a context that is not running");

            if (!ptr) {
                return h;
            }

            // Push current coroutine to the end of the queue
            if (auto next = ptr->push(h)) [[unlikely]] {
                fail("unexpected continuation when pushing to a locked context");
            }

            // Check the front of the queue, there's at least one continuation now
            auto next = ptr->pop();
            if (!next) [[unlikely]] {
                fail("unexpected unlock when removing the next continuation");
            }

            if (preempt()) {
                // Leave and defer when preempted
                actor_context_state::leave_unconditionally(ptr);
                defer(next);
                return std::noop_coroutine();
            }

            // Run the next continuation in the same context
            return next;
        }

        /**
         * Forces preemption without yielding in the current context
         */
        std::coroutine_handle<> preempt(std::coroutine_handle<> h) {
            verify_running("calling preempt() with a context that is not running");

            if (!ptr) {
                return h;
            }

            // Leave and defer unconditionally
            actor_context_state::leave_unconditionally(ptr);
            defer(h);
            return std::noop_coroutine();
        }

    private:
        actor_context_state* const ptr;
    };

    class sleep_until_context final : public intrusive_atomic_base<sleep_until_context> {
    public:
        sleep_until_context() noexcept = default;

        sleep_until_context(const sleep_until_context&) = delete;
        sleep_until_context& operator=(const sleep_until_context&) = delete;

        ~sleep_until_context() noexcept {
            void* addr = continuation.exchange(reinterpret_cast<void*>(MarkerFailure), std::memory_order_acquire);
            if (addr &&
                addr != reinterpret_cast<void*>(MarkerSuccess) &&
                addr != reinterpret_cast<void*>(MarkerFailure))
            {
                // We still have a continuation which needs to be destroyed
                std::coroutine_handle<>::from_address(addr).destroy();
            }
        }

        bool set_continuation(std::coroutine_handle<> c) noexcept {
            void* expected = nullptr;
            return continuation.compare_exchange_strong(expected, c.address(), std::memory_order_release);
        }

        void cancel() noexcept {
            continuation.store(nullptr, std::memory_order_release);
        }

        void finish(bool success) noexcept {
            void* addr = continuation.exchange(
                reinterpret_cast<void*>(success ? MarkerSuccess : MarkerFailure),
                std::memory_order_acq_rel);
            if (addr) {
                assert(addr != reinterpret_cast<void*>(MarkerSuccess));
                assert(addr != reinterpret_cast<void*>(MarkerFailure));
                std::coroutine_handle<>::from_address(addr).resume();
            }
        }

        bool ready() const noexcept {
            return continuation.load(std::memory_order_relaxed) != nullptr;
        }

        bool status() const {
            void* addr = continuation.load(std::memory_order_acquire);
            if (addr == reinterpret_cast<void*>(MarkerSuccess)) {
                return true;
            }
            if (addr == reinterpret_cast<void*>(MarkerFailure)) {
                return false;
            }
            throw std::logic_error("unexpected sleep status");
        }

    private:
        static constexpr uintptr_t MarkerSuccess = 1;
        static constexpr uintptr_t MarkerFailure = 2;

    private:
        std::atomic<void*> continuation{ nullptr };
    };

    class sleep_until_awaiter {
    public:
        sleep_until_awaiter(actor_scheduler* scheduler, actor_scheduler::time_point deadline)
            : scheduler(scheduler)
            , deadline(deadline)
        {}

        sleep_until_awaiter(const sleep_until_awaiter&) = delete;
        sleep_until_awaiter& operator=(const sleep_until_awaiter&) = delete;

        ~sleep_until_awaiter() {
            if (context) {
                // Support for bottom-up destruction (awaiter destroyed before
                // it is resumed). It is up to user to ensure there are no
                // concurrent resume attempts.
                context->cancel();
            }
        }

        bool await_ready(stop_token token = {}) {
            context.reset(new sleep_until_context);
            if (scheduler) {
                scheduler->schedule(
                    [context = this->context](bool success) {
                        context->finish(success);
                    },
                    deadline,
                    std::move(token));
            } else {
                context->finish(false);
            }
            return context->ready();
        }

        __attribute__((__noinline__))
        bool await_suspend(std::coroutine_handle<> c) noexcept {
            return context->set_continuation(c);
        }

        bool await_resume() noexcept {
            auto context = std::move(this->context);
            return context->status();
        }

    private:
        actor_scheduler* scheduler;
        actor_scheduler::time_point deadline;
        intrusive_ptr<sleep_until_context> context;
    };

    class with_deadline_request_stop {
    public:
        explicit with_deadline_request_stop(stop_source source)
            : source(std::move(source))
        {}

        /**
         * Called by stop_callback
         */
        void operator()() noexcept {
            std::move(source).request_stop();
        }

        /**
         * Called by scheduler on deadline
         */
        void operator()(bool deadline) noexcept {
            if (deadline) {
                std::move(source).request_stop();
            }
        }

    private:
        stop_source source;
    };

    template<awaitable_with_stop_token_propagation Awaitable>
    class with_deadline_awaiter {
        using Awaiter = std::decay_t<awaiter_type_t<Awaitable>>;

    public:
        using wrapped_awaiter_type = awaiter_unwrap_awaiter_type<Awaiter>;

        with_deadline_awaiter(Awaitable&& awaitable,
                actor_scheduler* scheduler,
                actor_scheduler::time_point deadline)
            : awaiter(get_awaiter(std::forward<Awaitable>(awaitable)))
            , scheduler(scheduler)
            , deadline(deadline)
        {}

        with_deadline_awaiter(const with_deadline_awaiter&) = delete;
        with_deadline_awaiter& operator=(const with_deadline_awaiter&) = delete;

        with_deadline_awaiter(with_deadline_awaiter&& rhs)
            : awaiter(std::move(rhs.awaiter))
            , scheduler(rhs.scheduler)
            , deadline(rhs.deadline)
        {}

        bool await_ready(stop_token token = {}) {
            if (scheduler) {
                stop_source source;
                if (token.stop_possible()) {
                    propagate.emplace(token, source);
                }
                if (!source.stop_requested()) {
                    scheduler->schedule(with_deadline_request_stop(source), deadline, std::move(token));
                    token = source.get_token();
                }
            }
            return awaiter.await_ready(std::move(token));
        }

        template<class Promise>
        __attribute__((__noinline__))
        decltype(auto) await_suspend(std::coroutine_handle<Promise> c)
            noexcept(has_noexcept_await_suspend<Awaiter, Promise>)
            requires has_await_suspend<Awaiter, Promise>
        {
            return awaiter.await_suspend(c);
        }

        decltype(auto) await_resume()
            noexcept(has_noexcept_await_resume<Awaiter>)
        {
            return awaiter.await_resume();
        }

    private:
        Awaiter awaiter;
        actor_scheduler* scheduler;
        actor_scheduler::time_point deadline;
        std::optional<stop_callback<with_deadline_request_stop>> propagate;
    };

} // namespace coroactors::detail
