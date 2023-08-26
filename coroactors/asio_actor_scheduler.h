#pragma once
#include <coroactors/actor_scheduler.h>
#include <coroactors/detail/intrusive_ptr.h>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/steady_timer.hpp>
#include <optional>

namespace coroactors {

    /**
     * An implementation of actor scheduler using an asio executor
     */
    class asio_actor_scheduler : public actor_scheduler {
    public:
        using executor_type = boost::asio::any_io_executor;
        using typename actor_scheduler::clock_type;
        using typename actor_scheduler::time_point;
        using typename actor_scheduler::duration;
        using typename actor_scheduler::schedule_callback_type;

        asio_actor_scheduler(
                const executor_type& executor,
                duration preempt_duration = std::chrono::microseconds(10))
            : executor_(executor)
            , preempt_duration_(preempt_duration)
        {}

        bool preempt() const override {
            if (const time_point* deadline = preempt_deadline) {
                return clock_type::now() >= *deadline;
            }

            // Not running actors, always preempt
            return true;
        }

        void schedule(std::coroutine_handle<> h) override {
            executor_.execute([h, d = preempt_duration_]() noexcept {
                if (!preempt_deadline) {
                    time_point deadline = clock_type::now() + d;
                    preempt_deadline = &deadline;
                    h.resume();
                    preempt_deadline = nullptr;
                } else {
                    h.resume();
                }
            });
        }

        void schedule(schedule_callback_type c, time_point d, stop_token t) override {
            if (t.stop_requested()) {
                c(false);
                return;
            }

            // This object manages its own lifetime
            detail::intrusive_ptr<timer_t> timer(
                new timer_t(executor_, d, std::move(c), preempt_duration_));
            timer->start(std::move(t));
        }

    private:
        class timer_t {
        public:
            timer_t(const executor_type& executor, time_point deadline,
                    schedule_callback_type&& callback,
                    duration preempt_duration)
                : timer(executor, deadline)
                , callback(std::move(callback))
                , preempt_duration(preempt_duration)
            {}

            timer_t(const timer_t&) = delete;
            timer_t& operator=(const timer_t&) = delete;

            void add_ref() noexcept {
                refcount.fetch_add(1, std::memory_order_relaxed);
            }

            size_t release_ref() noexcept {
                return refcount.fetch_sub(1, std::memory_order_acq_rel) - 1;
            }

            void start(stop_token&& token) {
                timer.async_wait(
                    [p = detail::intrusive_ptr<timer_t>(this)](const boost::system::error_code& ec) {
                        p->finish(ec);
                    });
                // Install stop_callback after the call to async_wait, in case
                // that callback would immediately run in a different thread.
                // Timer objects are not thread safe, and since cancel is the
                // only other call we may ever do this is safe.
                if (token.stop_possible()) {
                    cancel.emplace(std::move(token), this);
                }
            }

        private:
            void finish(const boost::system::error_code& ec) noexcept {
                if (!preempt_deadline) {
                    time_point deadline = clock_type::now() + preempt_duration;
                    preempt_deadline = &deadline;
                    callback(ec ? false : true);
                    preempt_deadline = nullptr;
                } else {
                    callback(ec ? false : true);
                }
            }

        private:
            struct cancel_t {
                const detail::intrusive_ptr<timer_t> p;

                explicit cancel_t(timer_t* p)
                    : p(p)
                {}

                cancel_t(const cancel_t&) = delete;
                cancel_t& operator=(const cancel_t&) = delete;

                void operator()() noexcept {
                    p->timer.cancel();
                }
            };

        private:
            std::atomic<size_t> refcount{ 0 };
            boost::asio::steady_timer timer;
            schedule_callback_type callback;
            duration preempt_duration;
            std::optional<stop_callback<cancel_t>> cancel;
        };

    private:
        executor_type executor_;
        duration preempt_duration_;

        static inline thread_local const time_point* preempt_deadline{ nullptr };
    };

} // namespace coroactors
