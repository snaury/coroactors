#pragma once
#include <coroactors/actor_scheduler.h>
#include <coroactors/detail/awaiters.h>
#include <coroactors/detail/intrusive_mailbox.h>
#include <coroactors/detail/mailbox.h>
#include <coroactors/intrusive_ptr.h>
#include <atomic>
#include <cassert>
#include <optional>

namespace coroactors {

    class actor_context;

} // namespace coroactors

namespace coroactors::detail {

    class actor_context_frame;
    class actor_context_manager;

    class actor_context_state final : public intrusive_atomic_base<actor_context_state> {
        friend actor_context;
        friend actor_context_frame;
        friend actor_context_manager;

        using mailbox_t = class intrusive_mailbox<actor_context_frame>;

    public:
        explicit actor_context_state(class actor_scheduler& s)
            : scheduler(s)
        {}

    private:
        /**
         * Pushes the next frame to this context's mailbox
         *
         * Returns nullptr when the mailbox is currently locked, the frame is
         * enqueued and may run in another thread. Otherwise locks the mailbox
         * and returns the next runnable frame, which may be different from the
         * passed in frame.
         *
         * This method is thread-safe and may be called by any thread. It is
         * also wait-free, never allocates and never throws exceptions.
         */
        actor_context_frame* push_frame(actor_context_frame* frame) noexcept;

        /**
         * Removes the next frame from this context's mailbox
         *
         * Returns nullptr and unlocks the mailbox when the next frame is
         * unavailable, care must be taken since this context may immediately
         * start running in another thread due to a successful push_frame.
         * Returns the next runnable frame and keeps the mailbox locked
         * otherwise.
         *
         * This method may only be called by a thread that implicitly has this
         * context locked by a previous call to push_frame. It is also
         * wait-free, never allocates and never throws exceptions.
         */
        actor_context_frame* next_frame() noexcept;

    private:
        actor_scheduler& scheduler;
        mailbox_t mailbox_{ mailbox_t::initially_unlocked };
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
