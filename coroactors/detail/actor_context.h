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
         * Run a coroutine `c` in this actor context
         *
         * Returns true when a previously empty context is locked for exclusive
         * access, and this would have been the first coroutine in the queue,
         * so it may run immediately. Returns false when `c` was added to the
         * queue, because the context is currently locked and possibly running
         * in another thread.
         */
        bool run_coroutine(std::coroutine_handle<> c) {
            assert(c && "Attempt to push a nullptr coroutine handle");
            if (mailbox_.push(c)) {
                std::coroutine_handle<> k = mailbox_.pop_default();
                assert(k == c);
                (void)k;
                return true;
            } else {
                return false;
            }
        }

        /**
         * Returns the next coroutine in this context queue or nullptr
         *
         * This context must be locked for exclusive access by the caller,
         * otherwise the behavior is undefined. When a nullptr is returned the
         * context is also unlocked, and may be concurrently locked in another
         * thread.
         */
        std::coroutine_handle<> next_coroutine() {
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
     * Coroutine management APIs so we don't clutter the actor context
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

        /**
         * Returns the number of currently running frames
         */
        static size_t running_frames() noexcept {
            return actor_context_state::running_frames;
        }

    private:
        /**
         * Post coroutine h to run in this context
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
         * Defer coroutine h to run in this context
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
         * Reschedules coroutine when preempted and returns true, otherwise false
         */
        bool maybe_preempt(std::coroutine_handle<> h, bool running) const noexcept {
            if (!ptr) {
                // Cannot preempt without a scheduler
                return false;
            }

            // When running in parallel with lower frames we use post
            if (running_frames() > (running ? 1 : 0)) [[unlikely]] {
                if (running) {
                    actor_context_state::leave_unconditionally(ptr);
                }
                post(h);
                return true;
            }

            // When preempted by the scheduler we use defer
            if (ptr->scheduler.preempt()) {
                if (running) {
                    actor_context_state::leave_unconditionally(ptr);
                }
                defer(h);
                return true;
            }

            return false;
        }

    public:
        /**
         * Resumes a coroutine h, which is expecting to run in this context
         *
         * Context must be exclusively locked to caller, e.g. by a successful
         * call to `start`. The resumed coroutine is expected to correctly
         * leave this context before returning.
         */
        void resume(std::coroutine_handle<> h) const {
            actor_context_state::frame_guard guard;
            actor_context_state::enter_unconditionally(ptr);
            h.resume();
        }

        /**
         * Starts a coroutine `h` in this context
         *
         * This is called when an actor coroutine attempts to start in some
         * datached way (e.g. `actor<T>::detach()`). When it returns true the
         * context is locked and the result must be resumed using a manager's
         * resume method.
         *
         * When optional `use_scheduler` is false this method will never post
         * provided coroutine to the scheduler, but it may still return false
         * when this context is currently locked and `h` is enqueued.
         */
        bool start(std::coroutine_handle<> h, bool use_scheduler = true) const {
            if (!ptr) {
                return true;
            }

            // Note: when the call to run_coroutine returns false it may
            // immediately start running in another thread and destroy this
            // context.
            if (!ptr->run_coroutine(h)) {
                return false;
            }

            // Entering actors may lead to a long chain of context switches
            // which is undesirable when a call is detached or added to a task
            // group. This is impossible to detect with generic coroutines,
            // however with actors we can, simply by detecting that this is
            // done while another actor is currently running. For generic
            // coroutines actor scheduler usually preempts context switches
            // when they happen in unexpected threads.
            if (use_scheduler) {
                if (maybe_preempt(h, /* running */ false)) {
                    return false;
                }
            }

            return true;
        }

        /**
         * Tries to enter this context with a coroutine `h`
         *
         * This is called when an actor coroutine starts due to `co_await` from
         * a non-actor coroutine. It always returns a valid coroutine handle,
         * which could be returned from an await_suspend method. When this
         * context locks it also starts running in the current thread,
         * otherwise the returned handle is noop_coroutine.
         */
        std::coroutine_handle<> enter(std::coroutine_handle<> h) const {
            if (start(h)) {
                actor_context_state::enter_unconditionally(ptr);
                return h;
            } else {
                return std::noop_coroutine();
            }
        }

        /**
         * Tries to get the next coroutine from this context without leaving
         *
         * Returns noop_coroutine() and leaves when nothing is available.
         */
        std::coroutine_handle<> next_coroutine() const {
            verify_running("calling next_coroutine() with a context that is not running");

            // Leave first because next_coroutine() may invalidate current context
            actor_context_state::leave_unconditionally(ptr);

            if (ptr) {
                if (auto next = ptr->next_coroutine()) {
                    if (!maybe_preempt(next, /* running */ false)) {
                        // Re-enter and continue in the same thread
                        actor_context_state::enter_unconditionally(ptr);
                        return next;
                    }
                }
            }

            return std::noop_coroutine();
        }

        /**
         * Leave this context, e.g. after starting some async activity
         */
        void leave() const {
            verify_running("calling leave() with a context that is not running");

            // Leave first becasue next_coroutine() may invalidate current context
            actor_context_state::leave_unconditionally(ptr);

            if (ptr) {
                if (auto next = ptr->next_coroutine()) {
                    // Run the next continuation parallel to current activity
                    post(next);
                }
            }
        }

        /**
         * Restore this context for a resuming coroutine h
         *
         * Returns the next runnable for the current thread, which may also
         * be a noop_coroutine(), in which case context is not restored and
         * the provided coroutine will run somewhere else.
         */
        std::coroutine_handle<> restore(std::coroutine_handle<> h) const {
            if (!ptr) {
                // Enter an empty context frame
                actor_context_state::enter_unconditionally(ptr);
                return h;
            }

            if (ptr->run_coroutine(h)) {
                // We have returned from an async activity, and usually don't
                // want to block a caller of resume(). This includes completed
                // network calls (in which case we want to defer, as it is a
                // continuation of the return), but also calls to
                // `continuation<T>::resume`, which really start a new
                // activity parallel to resumer. We rely on common logic in
                // maybe_preempt here and scheduler preempting in unexpected
                // threads and handlers. This should be consistent when we
                // return to actor directly, and when we return to actor via
                // a context-less actor call.
                if (!maybe_preempt(h, /* running */ false)) {
                    actor_context_state::enter_unconditionally(ptr);
                    return h;
                }
            }

            return std::noop_coroutine();
        }

        /**
         * Switches from this context to an empty context
         */
        void switch_to_empty() const {
            verify_running("calling switch_to_empty() with a context that is not running");

            if (ptr) {
                // We need to switch context first, because next_coroutine()
                // may unlock this context and then free it in another thread.
                actor_context_state::switch_unconditionally(ptr, nullptr);

                if (auto next = ptr->next_coroutine()) {
                    // This activity runs parallel to current thread
                    post(next);
                }
            }
        }

        /**
         * Switches to this context from a context `from` with a coroutine `h`
         *
         * The `returning` argument specifies the direction of switching: when
         * it is true the switch is returning from `co_await`.
         *
         * This is mostly equivalent to leave() followed by restore(), but it
         * is more efficient and has subtle differences where new continuation
         * is prioritized over additional work in the `from` context.
         *
         * Returns the next runnable coroutine for the current thread, which
         * may also be a noop_coroutine(), in which case it's as if leave() has
         * been called.
         */
        std::coroutine_handle<> switch_from(actor_context_manager from,
                std::coroutine_handle<> h, bool returning) const
        {
            from.verify_running("calling switch_from() with a context that is not running");

            assert(h && "Switching context with an invalid handle");
            (void)returning;

            if (ptr == from.ptr) {
                // We are not changing contexts, but we may want to preempt
                if (maybe_preempt(h, /* running */ true)) {
                    return std::noop_coroutine();
                }

                return h;
            }

            if (!ptr) {
                // Switching to a context without a scheduler. We need to
                // switch the context first, because next_coroutine() may
                // unlock and allow it to run in another thread.
                actor_context_state::switch_unconditionally(from.ptr, ptr);

                if (auto next = from.ptr->next_coroutine()) {
                    // Run the next activity parallel to h
                    from.post(next);
                }

                // Continue with this empty context
                return h;
            }

            // Leave current context first, because some coroutine may resume
            // in another thread with the same context when it is unblocked.
            actor_context_state::leave_unconditionally(from.ptr);

            // Take the next runnable from the current context. It will either
            // stay locked with more work, or will unlock and possibly run in
            // another thread. Note: we cannot attempt running in a new context
            // first, because when the coroutine starts running it may free
            // local variables which are keeping contexts in memory.
            std::coroutine_handle<> more;
            if (from.ptr) {
                // Note: when more != nullptr context is locked and safe
                more = from.ptr->next_coroutine();
            }

            // Try running h in this context. It will either be the first and
            // lock, so we will try running it in the current thread, or it
            // will be added to the queue and we will try running some work
            // from the original context.
            if (ptr->run_coroutine(h)) {
                // Check for preemption by this context
                if (maybe_preempt(h, /* running */ false)) {
                    if (more) {
                        from.post(more);
                    }
                    return std::noop_coroutine();
                }

                if (more) {
                    from.post(more);
                }

                actor_context_state::enter_unconditionally(ptr);
                return h;
            }

            if (!more) {
                return std::noop_coroutine();
            }

            if (from.maybe_preempt(more, /* running */ false)) {
                return std::noop_coroutine();
            }

            // Re-enter the original context with the next coroutine
            actor_context_state::enter_unconditionally(from.ptr);
            return more;
        }

        /**
         * Yields to other coroutines in this context
         */
        std::coroutine_handle<> yield(std::coroutine_handle<> h) {
            verify_running("calling yield(h) with a context that is not running");

            if (!ptr) {
                return h;
            }

            // Push current coroutine to the end of the queue
            if (ptr->run_coroutine(h)) [[unlikely]] {
                fail("unexpected run_coroutine(h) result on a locked context");
            }

            // Check the front of the queue, there's at least one coroutine there
            auto next = ptr->next_coroutine();
            if (!next) [[unlikely]] {
                fail("unexpected unlock when removing the next coroutine");
            }

            if (maybe_preempt(next, /* running */ true)) {
                return std::noop_coroutine();
            }

            // Run the next coroutine in the same context
            return next;
        }

        /**
         * Forces preemption without yielding in the current context
         */
        std::coroutine_handle<> preempt(std::coroutine_handle<> h) {
            verify_running("calling preempt(h) with a context that is not running");

            if (!ptr) {
                return h;
            }

            actor_context_state::leave_unconditionally(ptr);
            if (running_frames() > 0) {
                post(h);
            } else {
                defer(h);
            }
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
