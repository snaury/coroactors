#pragma once
#include <coroactors/actor_scheduler.h>
#include <deque>
#include <map>

struct test_scheduler
    : public coroactors::actor_scheduler
{
    struct continuation_t {
        coroactors::actor_scheduler_runnable* r;
        bool deferred;

        explicit continuation_t(coroactors::actor_scheduler_runnable* r, bool deferred)
            : r(r)
            , deferred(deferred)
        {}

        void run() noexcept {
            r->run();
        }
    };

    struct timer_t {
        schedule_callback_type callback;
        std::optional<coroactors::stop_callback<std::function<void()>>> stop;
        bool cancelled = false;

        explicit timer_t(schedule_callback_type&& callback)
            : callback(std::move(callback))
        {}
    };

    bool preempt() override {
        return in_run_next == 0;
    }

    void post(coroactors::actor_scheduler_runnable* r) override {
        queue.emplace_back(r, false);
    }

    void defer(coroactors::actor_scheduler_runnable* r) override {
        queue.emplace_back(r, true);
    }

    void schedule(schedule_callback_type c, time_point d, coroactors::stop_token t) override {
        if (!timers_enabled) {
            c(false);
            return;
        }
        auto it = timers.emplace(
            std::piecewise_construct,
            std::forward_as_tuple(d),
            std::forward_as_tuple(std::move(c)));
        if (t.stop_possible()) {
            locked = true;
            it->second.stop.emplace(std::move(t), [this, it]{
                it->second.cancelled = true;
                if (locked) {
                    return; // triggered during emplace
                }
                // The call is synchronized with destructor (i.e. removal)
                it->second.callback(false);
                // We destroy ourselves here, copy iterator first
                auto copy = it;
                timers.erase(copy);
            });
            locked = false;
            if (it->second.cancelled) {
                it->second.stop.reset();
                it->second.callback(false);
                timers.erase(it);
            }
        }
    }

    template<class TCallback>
    void run_in_scheduler(TCallback&& callback) {
        ++in_run_next;
        callback();
        --in_run_next;
    }

    continuation_t remove(size_t index) {
        assert(index < queue.size());
        auto it = queue.begin() + index;
        auto cont = std::move(*it);
        queue.erase(it);
        return cont;
    }

    void run_next() {
        assert(!queue.empty());
        auto cont = queue.front();
        queue.pop_front();
        ++in_run_next;
        cont.run();
        --in_run_next;
    }

    void wake_next() {
        assert(!timers.empty());
        auto it = timers.begin();
        it->second.stop.reset();
        assert(!it->second.cancelled);
        ++in_wake_next;
        it->second.callback(true);
        --in_wake_next;
        timers.erase(it);
    }

    void run() {
        for (;;) {
            if (!timers.empty()) {
                wake_next();
                continue;
            }
            if (!queue.empty()) {
                run_next();
                continue;
            }
            break;
        }
    }

    std::deque<continuation_t> queue;
    std::multimap<actor_scheduler::time_point, timer_t> timers;
    size_t in_run_next = 0;
    size_t in_wake_next = 0;
    bool timers_enabled = false;
    bool locked = false;
};
