#include <coroactors/actor.h>
#include <coroactors/packaged_awaitable.h>
#include <coroactors/with_continuation.h>
#include <gtest/gtest.h>
#include <deque>
#include <map>

using namespace coroactors;

struct test_scheduler : public actor_scheduler {
    using clock_type = actor_scheduler::clock_type;
    using time_point = actor_scheduler::time_point;

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
        std::optional<stop_callback<std::function<void()>>> stop;
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

    void schedule(schedule_callback_type c, time_point d, stop_token t) override {
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

    std::deque<continuation_t> queue;
    std::multimap<actor_scheduler::time_point, timer_t> timers;
    bool timers_enabled = false;
    bool locked = false;
};

actor<int> actor_return_const(int value) {
    co_return value;
}

TEST(ActorTest, ImmediateReturn) {
    auto r = packaged_awaitable(actor_return_const(42));
    ASSERT_TRUE(r.success());
    ASSERT_EQ(*r, 42);
}

actor<void> actor_await_const_without_context(int value) {
    int result = co_await actor_return_const(value);
    (void)result;
    ADD_FAILURE() << "unexpected success";
}

actor<void> actor_await_caller_context_without_context() {
    const actor_context& context = co_await actor_context::caller_context;
    (void)context;
    ADD_FAILURE() << "unexpected success";
}

actor<void> actor_await_current_context_without_context() {
    const actor_context& context = co_await actor_context::current_context;
    (void)context;
    ADD_FAILURE() << "unexpected success";
}

actor<void> actor_await_sleep_without_context() {
    co_await no_actor_context.sleep_for(std::chrono::milliseconds(100));
    ADD_FAILURE() << "unexpected success";
}

TEST(ActorTest, CannotAwaitWithoutContext) {
    auto a = packaged_awaitable(actor_await_const_without_context(42));
    EXPECT_TRUE(a.has_exception());
    auto b = packaged_awaitable(actor_await_caller_context_without_context());
    EXPECT_TRUE(b.has_exception());
    auto c = packaged_awaitable(actor_await_current_context_without_context());
    EXPECT_TRUE(c.has_exception());
    auto d = packaged_awaitable(actor_await_sleep_without_context());
    EXPECT_TRUE(d.has_exception());
}

actor<void> actor_empty_context() {
    co_await no_actor_context();
    EXPECT_EQ(co_await actor_context::current_context, no_actor_context);
}

actor<void> actor_empty_caller_context() {
    co_await actor_context::caller_context();
    EXPECT_EQ(co_await actor_context::current_context, no_actor_context);
}

TEST(ActorTest, StartWithEmptyContext) {
    auto a = packaged_awaitable(actor_empty_context());
    EXPECT_TRUE(a.success());
    auto b = packaged_awaitable(actor_empty_caller_context());
    EXPECT_TRUE(b.success());
}

actor<void> actor_with_specific_context(const actor_context& context) {
    co_await context();
    actor_context current = co_await actor_context::current_context;
    EXPECT_EQ(current, context);
    EXPECT_NE(current, no_actor_context);
    actor_context caller = co_await actor_context::caller_context;
    EXPECT_EQ(caller, no_actor_context);
}

TEST(ActorTest, StartWithSpecificContext) {
    test_scheduler scheduler;
    actor_context context(scheduler);
    // Not nested, so runs in the same thread
    auto r = packaged_awaitable(actor_with_specific_context(context));
    EXPECT_TRUE(r.success());
}

TEST(ActorTest, DetachWithSpecificContext) {
    test_scheduler scheduler;
    actor_context context(scheduler);
    // Not nested, so runs in the same thread
    actor_with_specific_context(context).detach();
    ASSERT_EQ(scheduler.queue.size(), 0u);
}

actor<void> actor_without_context_awaits_specific_context(const actor_context& context) {
    co_await no_actor_context();
    EXPECT_EQ(co_await actor_context::current_context, no_actor_context);
    co_await actor_with_specific_context(context);
    EXPECT_EQ(co_await actor_context::current_context, no_actor_context);
}

TEST(ActorTest, AwaitWithSpecificContext) {
    test_scheduler scheduler;
    actor_context context(scheduler);
    // The same thread of execution, no difference to run/detach
    auto r = packaged_awaitable(actor_without_context_awaits_specific_context(context));
    EXPECT_TRUE(r.success());
}

actor<void> actor_with_context_awaits_empty_context(int& stage, const actor_context& context) {
    stage = 1;
    co_await context();
    stage = 2;
    EXPECT_EQ(co_await actor_context::current_context, context);
    stage = 3;
    co_await actor_empty_context();
    stage = 4;
}

TEST(ActorTest, AwaitEmptyFromSpecificContext) {
    test_scheduler scheduler;
    actor_context context(scheduler);
    int stage = 0;
    auto r = packaged_awaitable(actor_with_context_awaits_empty_context(stage, context));
    EXPECT_EQ(stage, 3); // we should defer on the return path
    ASSERT_EQ(scheduler.queue.size(), 1u);
    EXPECT_EQ(scheduler.queue[0].deferred, true);
    scheduler.run_next();
    EXPECT_EQ(stage, 4);
    EXPECT_TRUE(r.success());
}

actor<void> actor_without_context_runs_specific_context(const actor_context& context,
        std::optional<packaged_awaitable<void>>& r,
        std::function<void()> before_return)
{
    co_await no_actor_context();
    r.emplace(actor_with_specific_context(context));
    before_return();
}

TEST(ActorTest, StartNestedWithSpecificContext) {
    test_scheduler scheduler;
    actor_context context(scheduler);
    std::optional<packaged_awaitable<void>> r1;
    auto r = packaged_awaitable(actor_without_context_runs_specific_context(context, r1,
        [&]{
            EXPECT_TRUE(r1);
            EXPECT_FALSE(*r1);
        }));
    EXPECT_TRUE(r.success());
    EXPECT_TRUE(r1->running());
    ASSERT_EQ(scheduler.queue.size(), 1u);
    scheduler.run_next();
    ASSERT_EQ(scheduler.queue.size(), 0u);
    EXPECT_TRUE(r1->success());
}

actor<void> actor_check_sleep(const actor_context& context, bool expected,
        std::function<void()> before_sleep = {})
{
    co_await context();
    if (before_sleep) {
        before_sleep();
    }
    bool success = co_await context.sleep_for(std::chrono::milliseconds(100));
    EXPECT_EQ(success, expected);
}

TEST(ActorTest, Sleep) {
    test_scheduler scheduler;
    actor_context context(scheduler);
    {
        SCOPED_TRACE("no actor context");
        auto r = packaged_awaitable(actor_check_sleep(no_actor_context, false));
        EXPECT_TRUE(r.success());
    }
    {
        SCOPED_TRACE("timers disabled");
        auto r = packaged_awaitable(actor_check_sleep(context, false));
        ASSERT_EQ(scheduler.queue.size(), 0u);
        EXPECT_TRUE(r.success());
    }
    scheduler.timers_enabled = true;
    {
        SCOPED_TRACE("timer triggers");
        auto r = packaged_awaitable(actor_check_sleep(context, true));
        ASSERT_EQ(scheduler.queue.size(), 0u);
        ASSERT_EQ(scheduler.timers.size(), 1u);
        scheduler.wake_next();
        ASSERT_EQ(scheduler.timers.size(), 0u);
        // Returning from a timer will defer
        ASSERT_EQ(scheduler.queue.size(), 1u);
        scheduler.run_next();
        ASSERT_EQ(scheduler.queue.size(), 0u);
        EXPECT_TRUE(r.success());
    }
    {
        SCOPED_TRACE("cancelled before sleep");
        stop_source source;
        auto before_sleep = [&]{
            source.request_stop();
        };
        auto r = packaged_awaitable(with_stop_token(source.get_token(), actor_check_sleep(context, false, before_sleep)));
        ASSERT_EQ(scheduler.queue.size(), 0u);
        ASSERT_EQ(scheduler.timers.size(), 0u);
        EXPECT_TRUE(r.success());
    }
    {
        SCOPED_TRACE("cancelled during sleep");
        stop_source source;
        auto r = packaged_awaitable(with_stop_token(source.get_token(), actor_check_sleep(context, false)));
        ASSERT_EQ(scheduler.queue.size(), 0u);
        ASSERT_EQ(scheduler.timers.size(), 1u);
        source.request_stop();
        ASSERT_EQ(scheduler.timers.size(), 0u);
        ASSERT_EQ(scheduler.queue.size(), 1u);
        scheduler.run_next();
        ASSERT_EQ(scheduler.queue.size(), 0u);
        EXPECT_TRUE(r.success());
    }
}

struct aborted_suspend {
    bool await_ready() {
        return false;
    }

    bool await_suspend(std::coroutine_handle<> h) {
        // Abort suspend and resume
        return false;
    }

    int await_resume() {
        return 42;
    }
};

actor<void> actor_aborted_suspend(const actor_context& context) {
    co_await context();

    int value = co_await aborted_suspend{};
    EXPECT_EQ(value, 42);
}

TEST(TestActor, AbortedSuspend) {
    test_scheduler scheduler;
    actor_context context(scheduler);

    auto r = packaged_awaitable(actor_aborted_suspend(context));
    // Suspend is aborted, so we expect no context switch on the return path
    EXPECT_EQ(scheduler.queue.size(), 0u);
    EXPECT_TRUE(r.success());
}

struct throw_during_suspend {
    bool await_ready() {
        return false;
    }

    void await_suspend(std::coroutine_handle<> h) {
        throw std::runtime_error("throw during suspend");
    }

    void await_resume() {
        // should be unreachable
    }
};

actor<void> actor_throw_during_suspend(const actor_context& context) {
    co_await context();

    EXPECT_THROW(co_await throw_during_suspend{}, std::runtime_error);
}

TEST(TestActor, ThrowDuringSuspend) {
    test_scheduler scheduler;
    actor_context context(scheduler);

    auto r = packaged_awaitable(actor_throw_during_suspend(context));
    // Suspend throws an exception, so we expect to observe it in the actor
    // without context switches, double frees or any leaks.
    EXPECT_EQ(scheduler.queue.size(), 0u);
    EXPECT_TRUE(r.success());
}
