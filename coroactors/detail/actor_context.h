#pragma once
#include <coroactors/actor_scheduler.h>
#include <coroactors/detail/awaiters.h>
#include <coroactors/detail/mailbox.h>
#include <coroactors/intrusive_ptr.h>
#include <atomic>
#include <cassert>
#include <optional>

namespace coroactors {

    class actor_context;

} // namespace coroactors

namespace coroactors::detail {

    class actor_context_manager;

    class actor_context_state final : public intrusive_atomic_base<actor_context_state> {
        friend actor_context;
        friend actor_context_manager;

        using mailbox_t = class mailbox<std::coroutine_handle<>>;

    public:
        explicit actor_context_state(class actor_scheduler& s)
            : scheduler(s)
        {}

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
        actor_scheduler& scheduler;
        mailbox_t mailbox_{ mailbox_t::initially_unlocked };
    };

    /**
     * Coroutine management APIs so we don't clutter the actor context
     *
     * Note: many methods are noexcept, not because there may be no exceptions,
     * but because coroutines are complicated and we can only terminate when
     * scheduling fails. For example: we have scheduled current coroutine to
     * run in another thread, but scheduling of something else fails with
     * bad_alloc. But then on exception current coroutine will resume, which
     * may already be running in another thread.
     */
    class actor_context_manager {
        friend class ::coroactors::actor_context;

        explicit constexpr actor_context_manager(actor_context_state* ptr) noexcept
            : ptr(ptr)
        {}

    private:
        /**
         * Post a coroutine h to run in this context
         */
        void post(std::coroutine_handle<> h) const noexcept {
            // Note: `std::function` over a lambda capture with two pointers
            // usually doesn't allocate any additional memory. We also don't
            // need to add_ref the state, because the coroutine must keep a
            // reference to context while it's running.
            ptr->scheduler.post([h, ptr = ptr] {
                actor_context_manager(ptr).resume(h);
            });
        }

        /**
         * Defer a coroutine h to run in this context
         */
        void defer(std::coroutine_handle<> h) const noexcept {
            // Note: `std::function` over a lambda capture with two pointers
            // usually doesn't allocate any additional memory. We also don't
            // need to add_ref the state, because the coroutine must keep a
            // reference to context while it's running.
            ptr->scheduler.defer([h, ptr = ptr] {
                actor_context_manager(ptr).resume(h);
            });
        }

        /**
         * Resumes a coroutine h, which is expecting to run in this context
         *
         * Context must be exclusively locked to caller, either explicitly by
         * a call to `start`, or implicitly by `post` or `defer`. The resumed
         * coroutine is expected to correctly leave this context before
         * returning.
         */
        void resume(std::coroutine_handle<> h) const noexcept {
#ifndef NDEBUG
            size_t saved_frames = running_frames;
#endif

            enter_frame();
            h.resume();

#ifndef NDEBUG
            assert(running_frames == saved_frames);
#endif
        }

        /**
         * Reschedules coroutine when preempted and returns true, otherwise false
         *
         * Automatically leaves the context on preemption when `running` is true.
         */
        bool maybe_preempt(std::coroutine_handle<> h, bool running) const noexcept {
            if (!ptr) {
                // Cannot preempt without a scheduler
                return false;
            }

            // When running in parallel with lower frames we use post
            if (running_frames > (running ? 1 : 0)) [[unlikely]] {
                if (running) {
                    leave_frame();
                }
                post(h);
                return true;
            }

            // When preempted by the scheduler we use defer
            if (ptr->scheduler.preempt()) {
                if (running) {
                    leave_frame();
                }
                defer(h);
                return true;
            }

            return false;
        }

        /**
         * Prepare a coroutine `h` to start in this context
         *
         * This is used both for starting a detached coroutine, as well as
         * starting a coroutine with `co_await` from a non-actor coroutine.
         * When `preempt` is true it will also check for preemption.
         *
         * Returns true when `h` is not enqueued and may start in this thread
         */
        bool prepare(std::coroutine_handle<> h, bool preempt) const {
            if (!ptr) {
                return true;
            }

            // Note: when the call to run_coroutine returns false it may
            // immediately start running in another thread and destroy this
            // context.
            if (!ptr->run_coroutine(h)) {
                return false;
            }

            // This is technically a context switch, so check for preemption
            if (preempt && maybe_preempt(h, /* running */ false)) {
                return false;
            }

            return true;
        }

    public:
        /**
         * Tries to start a coroutine `h` in this thread and context
         *
         * This is mainly to support `actor<T>::detach()`.
         */
        void start(std::coroutine_handle<> h) const {
            if (prepare(h, /* preempt */ true)) {
                resume(h);
            }
        }

        /**
         * Tries to enter this context with a coroutine `h`
         *
         * This is called when an actor coroutine starts due to `co_await` from
         * a non-actor coroutine. It always returns a valid coroutine handle,
         * which could be returned from an await_suspend method. When this
         * context locks it also starts running in the current thread,
         * otherwise the returned handle is a noop_coroutine().
         */
        std::coroutine_handle<> enter(std::coroutine_handle<> h) const {
            if (prepare(h, /* preempt */ true)) {
                enter_frame();
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
        std::coroutine_handle<> next_coroutine() const noexcept {
            assert(running() && "calling next_coroutine() with a context that is not running");

            leave_frame();

            if (ptr) {
                if (auto next = ptr->next_coroutine()) {
                    if (!maybe_preempt(next, /* running */ false)) {
                        // This context continues in the current thread
                        enter_frame();
                        return next;
                    }
                }
            }

            return std::noop_coroutine();
        }

        /**
         * Leave this context, e.g. after starting some async activity
         */
        void leave() const noexcept {
            assert(running() && "calling leave() with a context that is not running");

            leave_frame();

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
        std::coroutine_handle<> restore(std::coroutine_handle<> h) const noexcept {
            if (!ptr) {
                // Enter an empty context frame
                enter_frame();
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
                    // This context continues in the current thread
                    enter_frame();
                    return h;
                }
            }

            return std::noop_coroutine();
        }

        /**
         * Switches from this context to an empty context
         */
        void switch_to_empty() const noexcept {
            assert(running() && "calling switch_to_empty() with a context that is not running");

            if (ptr) {
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
                std::coroutine_handle<> h, bool returning) const noexcept
        {
            assert(from.running() && "calling switch_from() with a context that is not running");
            assert(h && "Switching context with an invalid handle");

            // This currently does not change behavior
            (void)returning;

            if (ptr == from.ptr) {
                if (maybe_preempt(h, /* running */ true)) {
                    return std::noop_coroutine();
                }

                // Same context continues in this thread
                return h;
            }

            from.leave_frame();

            if (!ptr) {
                if (auto next = from.ptr->next_coroutine()) {
                    // Run the next activity parallel to h
                    from.post(next);
                }

                // Empty context continues in this thread
                enter_frame();
                return h;
            }

            // Take the next runnable from the current context. It will either
            // stay locked with more work, or will unlock and possibly run in
            // another thread. Note: we cannot attempt running in a new context
            // first, because when a coroutine starts running it may free
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

                // This context continues in this thread
                enter_frame();
                return h;
            }

            if (!more) {
                return std::noop_coroutine();
            }

            if (from.maybe_preempt(more, /* running */ false)) {
                return std::noop_coroutine();
            }

            // The `from` context continues in this thread
            from.enter_frame();
            return more;
        }

        /**
         * Yields to other coroutines in this context
         */
        std::coroutine_handle<> yield(std::coroutine_handle<> h) const noexcept {
            assert(running() && "calling yield(h) with a context that is not running");

            if (!ptr) {
                return h;
            }

            leave_frame();

            // Push current coroutine to the end of the queue
            if (ptr->run_coroutine(h)) [[unlikely]] {
                assert(false && "unexpected run_coroutine(h) result on a locked context");
            }

            // Check the front of the queue, there's at least one coroutine there
            auto next = ptr->next_coroutine();
            if (!next) [[unlikely]] {
                assert(false && "unexpected unlock when removing the next coroutine");
            }

            if (maybe_preempt(next, /* running */ false)) {
                return std::noop_coroutine();
            }

            // Run the next coroutine in the same context
            enter_frame();
            return next;
        }

        /**
         * Forces preemption without yielding in the current context
         */
        std::coroutine_handle<> preempt(std::coroutine_handle<> h) {
            assert(running() && "calling preempt(h) with a context that is not running");

            if (!ptr) {
                return h;
            }

            leave_frame();

            if (running_frames > 0) {
                post(h);
            } else {
                defer(h);
            }

            return std::noop_coroutine();
        }

    private:
        static bool running() noexcept {
            return running_frames > 0;
        }

        static void enter_frame() noexcept {
            running_frames++;
        }

        static void leave_frame() noexcept {
            assert(running_frames > 0 && "leaving a frame that is not running");
            running_frames--;
        }

    private:
        // Tracks the number of running context frames in the current thread stack
        static inline thread_local size_t running_frames{ 0 };

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
