#pragma once
#include <coroactors/actor_scheduler.h>
#include <coroactors/intrusive_ptr.h>
#include <asio/any_io_executor.hpp>
#include <asio/steady_timer.hpp>
#include <asio/post.hpp>
#include <asio/defer.hpp>
#include <optional>

namespace coroactors {

    /**
     * An implementation of actor scheduler using an asio executor
     */
    class asio_actor_scheduler : public actor_scheduler {
    public:
        using executor_type = asio::any_io_executor;
        using typename actor_scheduler::clock_type;
        using typename actor_scheduler::time_point;
        using typename actor_scheduler::duration;
        using typename actor_scheduler::execute_callback_type;
        using typename actor_scheduler::schedule_callback_type;

        asio_actor_scheduler(
                const executor_type& executor,
                duration preempt_duration = std::chrono::microseconds(10))
            : executor_(executor)
            , preempt_duration_(preempt_duration)
        {}

        bool preempt() override {
            if (const time_point* deadline = preempt_deadline) {
                return clock_type::now() >= *deadline;
            }

            // Not running actors right now, always preempt
            return true;
        }

        void post(execute_callback_type c) override {
            asio::post(executor_, [c = std::move(c), d = preempt_duration_]() mutable noexcept {
                resume_with_preemption(std::move(c), d);
            });
        }

        void defer(execute_callback_type c) override {
            asio::defer(executor_, [c = std::move(c), d = preempt_duration_]() mutable noexcept {
                resume_with_preemption(std::move(c), d);
            });
        }

        void schedule(schedule_callback_type c, time_point d, stop_token t) override {
            if (t.stop_requested()) {
                c(false);
                return;
            }

            // This object manages its own lifetime
            intrusive_ptr timer(
                new timer_t(executor_, d, std::move(c)));
            timer->start(std::move(t));
        }

    private:
        static void resume_with_preemption(execute_callback_type&& c,
                duration preempt_duration)
        {
            if (!preempt_deadline) {
                time_point deadline = clock_type::now() + preempt_duration;
                preempt_deadline = &deadline;
                std::move(c)();
                preempt_deadline = nullptr;
            } else {
                std::move(c)();
            }
        }

    private:
        class timer_t final : public intrusive_atomic_base<timer_t> {
        public:
            timer_t(const executor_type& executor, time_point deadline,
                    schedule_callback_type&& callback)
                : timer(executor, deadline)
                , callback(std::move(callback))
            {}

            timer_t(const timer_t&) = delete;
            timer_t& operator=(const timer_t&) = delete;

            void start(stop_token&& token) {
                timer.async_wait(
                    [p = intrusive_ptr(this)](const asio::error_code& ec) {
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
            void finish(const asio::error_code& ec) noexcept {
                callback(ec ? false : true);
            }

        private:
            struct cancel_t {
                const intrusive_ptr<timer_t> p;

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
            asio::steady_timer timer;
            schedule_callback_type callback;
            std::optional<stop_callback<cancel_t>> cancel;
        };

    private:
        executor_type executor_;
        duration preempt_duration_;

        static inline thread_local const time_point* preempt_deadline{ nullptr };
    };

} // namespace coroactors
