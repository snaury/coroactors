#include <coroactors/actor_scheduler.h>
#include <deque>
#include <map>

struct test_scheduler
    : public coroactors::actor_scheduler
{
    struct continuation_t {
        execute_callback_type callback;
        bool deferred;

        explicit continuation_t(execute_callback_type&& callback, bool deferred)
            : callback(std::move(callback))
            , deferred(deferred)
        {}

        void operator()() {
            callback();
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

    bool preempt() const override {
        return false;
    }

    void post(execute_callback_type c) override {
        queue.emplace_back(std::move(c), false);
    }

    void defer(execute_callback_type c) override {
        queue.emplace_back(std::move(c), true);
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

    void run_next() {
        assert(!queue.empty());
        auto cont = queue.front();
        queue.pop_front();
        cont();
    }

    void wake_next() {
        assert(!timers.empty());
        auto it = timers.begin();
        it->second.stop.reset();
        assert(!it->second.cancelled);
        it->second.callback(true);
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
    bool timers_enabled = false;
    bool locked = false;
};
