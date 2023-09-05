#include <coroactors/actor.h>
#include <gtest/gtest.h>

#include "test_scheduler.h"
#include <coroactors/detach_awaitable.h>
#include <coroactors/packaged_awaitable.h>
#include <coroactors/task_group.h>
#include <coroactors/with_continuation.h>

using namespace coroactors;

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
    EXPECT_EQ(result, value);
}

actor<void> actor_await_caller_context_without_context() {
    const actor_context& context = co_await actor_context::caller_context;
    EXPECT_EQ(context, no_actor_context);
}

actor<void> actor_await_current_context_without_context() {
    const actor_context& context = co_await actor_context::current_context;
    EXPECT_EQ(context, no_actor_context);
}

actor<void> actor_await_sleep_without_context() {
    co_await no_actor_context.sleep_for(std::chrono::milliseconds(100));
}

TEST(ActorTest, AwaitWithoutContext) {
    auto a = packaged_awaitable(actor_await_const_without_context(42));
    EXPECT_THROW(*a, actor_error);
    auto b = packaged_awaitable(actor_await_caller_context_without_context());
    EXPECT_THROW(*b, actor_error);
    auto c = packaged_awaitable(actor_await_current_context_without_context());
    EXPECT_THROW(*c, actor_error);
    auto d = packaged_awaitable(actor_await_sleep_without_context());
    EXPECT_THROW(*d, actor_error);
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
    // We are not running in the scheduler so it will preempt
    auto r = packaged_awaitable(actor_with_specific_context(context));
    EXPECT_TRUE(r.running());
    ASSERT_EQ(scheduler.queue.size(), 1u);
    EXPECT_EQ(scheduler.queue[0].deferred, false);
    scheduler.run_next();
    EXPECT_EQ(scheduler.queue.size(), 0u);
    EXPECT_TRUE(r.success());
}

TEST(ActorTest, DetachWithSpecificContext) {
    test_scheduler scheduler;
    actor_context context(scheduler);
    // Detached actors with a context are always posted
    actor_with_specific_context(context).detach();
    ASSERT_EQ(scheduler.queue.size(), 1u);
    EXPECT_EQ(scheduler.queue[0].deferred, false);
    scheduler.run_next();
    EXPECT_EQ(scheduler.queue.size(), 0u);
    EXPECT_EQ(scheduler.queue.size(), 0u);
}

TEST(ActorTest, DetachAwaitableWithSpecificContext) {
    test_scheduler scheduler;
    actor_context context(scheduler);
    // The detach_awaitable use case should not differ from detach
    detach_awaitable(actor_with_specific_context(context));
    ASSERT_EQ(scheduler.queue.size(), 1u);
    EXPECT_EQ(scheduler.queue[0].deferred, false);
    scheduler.run_next();
    EXPECT_EQ(scheduler.queue.size(), 0u);
    EXPECT_EQ(scheduler.queue.size(), 0u);
}

TEST(ActorTest, ActorContextInheritance) {
    test_scheduler scheduler;
    actor_context context(scheduler);

    auto r = packaged_awaitable([](const actor_context& context) -> actor<void> {
        // Explicitly inherit some caller context
        co_await actor_context::caller_context();
        EXPECT_EQ(co_await actor_context::caller_context, no_actor_context);
        EXPECT_EQ(co_await actor_context::current_context, no_actor_context);
        co_await context();
        EXPECT_EQ(co_await actor_context::caller_context, no_actor_context);
        EXPECT_EQ(co_await actor_context::current_context, context);
        co_await [](const actor_context& context) -> actor<void> {
            // Explicitly inherit some caller context
            co_await actor_context::caller_context();
            EXPECT_EQ(co_await actor_context::caller_context, context);
            EXPECT_EQ(co_await actor_context::current_context, context);
            // We can change it to an empty context
            co_await no_actor_context();
            // Caller will not change, but current context will
            EXPECT_EQ(co_await actor_context::caller_context, context);
            EXPECT_EQ(co_await actor_context::current_context, no_actor_context);
        }(context);
        // Context is restored when we return
        EXPECT_EQ(co_await actor_context::caller_context, no_actor_context);
        EXPECT_EQ(co_await actor_context::current_context, context);
    }(context));

    EXPECT_TRUE(r.running());
    ASSERT_EQ(scheduler.queue.size(), 1u);
    EXPECT_EQ(scheduler.queue[0].deferred, true);
    scheduler.run_next();
    EXPECT_EQ(scheduler.queue.size(), 0u);
    EXPECT_TRUE(r.success());
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
    // No difference to run/detach
    auto r = packaged_awaitable(actor_without_context_awaits_specific_context(context));
    EXPECT_TRUE(r.running());
    ASSERT_EQ(scheduler.queue.size(), 1u);
    EXPECT_EQ(scheduler.queue[0].deferred, true);
    scheduler.run_next();
    EXPECT_EQ(scheduler.queue.size(), 0u);
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
    EXPECT_TRUE(r.running());
    EXPECT_EQ(stage, 1);
    ASSERT_EQ(scheduler.queue.size(), 1u);
    EXPECT_EQ(scheduler.queue[0].deferred, false);
    scheduler.run_next();
    EXPECT_EQ(scheduler.queue.size(), 0u);
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
    EXPECT_EQ(scheduler.queue[0].deferred, false);
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
        ASSERT_EQ(scheduler.queue.size(), 1u);
        EXPECT_EQ(scheduler.queue[0].deferred, false);
        scheduler.run_next();
        ASSERT_EQ(scheduler.queue.size(), 0u);
        EXPECT_TRUE(r.success());
    }
    scheduler.timers_enabled = true;
    {
        SCOPED_TRACE("timer triggers");
        auto r = packaged_awaitable(actor_check_sleep(context, true));
        ASSERT_EQ(scheduler.queue.size(), 1u);
        EXPECT_EQ(scheduler.queue[0].deferred, false);
        scheduler.run_next();
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
        ASSERT_EQ(scheduler.queue.size(), 1u);
        EXPECT_EQ(scheduler.queue[0].deferred, false);
        scheduler.run_next();
        ASSERT_EQ(scheduler.queue.size(), 0u);
        ASSERT_EQ(scheduler.timers.size(), 0u);
        EXPECT_TRUE(r.success());
    }
    {
        SCOPED_TRACE("cancelled during sleep");
        stop_source source;
        auto r = packaged_awaitable(with_stop_token(source.get_token(), actor_check_sleep(context, false)));
        ASSERT_EQ(scheduler.queue.size(), 1u);
        EXPECT_EQ(scheduler.queue[0].deferred, false);
        scheduler.run_next();
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
    ASSERT_EQ(scheduler.queue.size(), 1u);
    scheduler.run_next();
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
    ASSERT_EQ(scheduler.queue.size(), 1u);
    scheduler.run_next();
    // Suspend throws an exception, so we expect to observe it in the actor
    // without context switches, double frees or any leaks.
    EXPECT_EQ(scheduler.queue.size(), 0u);
    EXPECT_TRUE(r.success());
}

TEST(TestActor, StartNestedActorsBeforeContext) {
    test_scheduler scheduler;
    actor_context context(scheduler);
    std::vector<int> events;

    std::optional<packaged_awaitable<int>> ropt;

    // We need to start everything inside a scheduler
    scheduler.post([&]{
        ropt.emplace(
            [](const actor_context& context, std::vector<int>& events) -> actor<int> {
                task_group<int> g;

                events.push_back(11);
                g.add([](const actor_context& context, std::vector<int>& events) -> actor<int> {
                    events.push_back(21);
                    co_await context();
                    events.push_back(22);
                    co_return 42;
                }(context, events));

                events.push_back(12);
                g.add([](const actor_context& context, std::vector<int>& events) -> actor<int> {
                    events.push_back(31);
                    co_await context();
                    events.push_back(32);
                    co_return 58;
                }(context, events));

                events.push_back(13);
                co_await context();

                events.push_back(14);
                int a = co_await g.next();

                events.push_back(15);
                int b = co_await g.next();

                events.push_back(16);
                co_return a + b;
            }(context, events));
    });
    scheduler.run_next();

    ASSERT_TRUE(ropt);
    auto r = std::move(*ropt);

    // We must be blocked on nested context activation (running inside an actor)
    // And parent coroutine then enqueues behind the two other activations
    EXPECT_TRUE(r.running());
    EXPECT_EQ(events, std::vector<int>({ 11, 21, 12, 31, 13 }));
    events.clear();

    ASSERT_EQ(scheduler.queue.size(), 1u);
    EXPECT_EQ(scheduler.queue[0].deferred, false); // parallel activity
    scheduler.run_next();

    // We return to a non-actor coroutine (task group), next one is posted
    EXPECT_EQ(events, std::vector<int>({ 22 }));
    events.clear();

    ASSERT_EQ(scheduler.queue.size(), 1u);
    EXPECT_EQ(scheduler.queue[0].deferred, false); // parallel activity
    scheduler.run_next();

    // Same for the second task
    EXPECT_EQ(events, std::vector<int>({ 32 }));
    events.clear();

    ASSERT_EQ(scheduler.queue.size(), 1u);
    EXPECT_EQ(scheduler.queue[0].deferred, false); // parallel activity
    scheduler.run_next();

    // The parent wakes up and finishes everything
    EXPECT_EQ(events, std::vector<int>({ 14, 15, 16 }));
    EXPECT_EQ(scheduler.queue.size(), 0u);
    EXPECT_EQ(*r, 100);
}

TEST(TestActor, SwitchBetweenSchedulers) {
    int stage = 0;
    test_scheduler scheduler1;
    actor_context context1(scheduler1);
    test_scheduler scheduler2;
    actor_context context2(scheduler2);

    auto r = packaged_awaitable(
        [](int& stage, const actor_context& context1, const actor_context& context2) -> actor<void> {
            stage = 1;
            co_await context1();
            stage = 2;
            co_await [](int& stage, const actor_context& context2) -> actor<void> {
                stage = 3;
                co_await context2();
                stage = 4;
            }(stage, context2);
            stage = 5;
        }(stage, context1, context2));

    EXPECT_EQ(stage, 1);
    ASSERT_EQ(scheduler1.queue.size(), 1u);
    EXPECT_EQ(scheduler1.queue[0].deferred, false);
    scheduler1.run_next();
    EXPECT_EQ(stage, 3);
    ASSERT_EQ(scheduler2.queue.size(), 1u);
    EXPECT_EQ(scheduler2.queue[0].deferred, false);
    scheduler2.run_next();
    EXPECT_EQ(stage, 4);
    ASSERT_EQ(scheduler1.queue.size(), 1u);
    EXPECT_EQ(scheduler1.queue[0].deferred, false);
    scheduler1.run_next();
    EXPECT_EQ(stage, 5);
    EXPECT_TRUE(r.success());
}
