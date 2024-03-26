#include <coroactors/async.h>
#include <gtest/gtest.h>

#include "test_channel.h"
#include "test_scheduler.h"
#include <coroactors/actor_context.h>
#include <coroactors/detach_awaitable.h>
#include <coroactors/packaged_awaitable.h>
#include <coroactors/task_group.h>
#include <coroactors/with_stop_token.h>

using namespace coroactors;

namespace {
    detail::async_task* current_task() {
        return detail::async_task::current;
    }
}

TEST(AsyncTest, ImmediateReturn) {
    auto body = []() -> async<int> {
        auto* task = current_task();
        EXPECT_TRUE(task != nullptr);
        co_return 42;
    };

    EXPECT_EQ(current_task(), nullptr);
    auto r = packaged_awaitable(body());
    EXPECT_EQ(current_task(), nullptr);
    ASSERT_TRUE(r.success());
    ASSERT_EQ(*r, 42);
}

TEST(AsyncTest, StartWithEmptyContext) {
    auto body = []() -> async<void> {
        auto* task = current_task();
        EXPECT_NE(task, nullptr);
        EXPECT_EQ(current_actor_context(), no_actor_context);
        co_await no_actor_context();
        EXPECT_EQ(current_task(), task);
        EXPECT_EQ(current_actor_context(), no_actor_context);
    };

    EXPECT_EQ(current_task(), nullptr);
    auto r = packaged_awaitable(body());
    EXPECT_EQ(current_task(), nullptr);
    ASSERT_TRUE(r.success());
}

TEST(AsyncTest, StartWithEmptyCallerContext) {
    auto body = []() -> async<void> {
        auto* task = current_task();
        EXPECT_NE(task, nullptr);
        EXPECT_EQ(current_actor_context(), no_actor_context);
        co_await actor_context::caller_context();
        EXPECT_EQ(current_task(), task);
        EXPECT_EQ(current_actor_context(), no_actor_context);
    };

    EXPECT_EQ(current_task(), nullptr);
    auto r = packaged_awaitable(body());
    EXPECT_EQ(current_task(), nullptr);
    ASSERT_TRUE(r.success());
}

TEST(AsyncTest, StartWithSpecificContext) {
    test_scheduler scheduler;
    actor_context context(scheduler);

    auto body = [&]() -> async<void> {
        auto* task = current_task();
        EXPECT_NE(task, nullptr);
        EXPECT_EQ(current_actor_context(), no_actor_context);
        co_await context();
        EXPECT_EQ(current_task(), task);
        EXPECT_EQ(current_actor_context(), context);
    };

    EXPECT_EQ(current_task(), nullptr);
    auto r = packaged_awaitable(body());
    EXPECT_EQ(current_task(), nullptr);
    EXPECT_TRUE(r.running());
    // We are not running in the scheduler so it will preempt
    ASSERT_EQ(scheduler.queue.size(), 1u);
    EXPECT_EQ(scheduler.queue[0].deferred, false);
    scheduler.run_next();
    EXPECT_EQ(scheduler.queue.size(), 0u);
    EXPECT_TRUE(r.success());
}

TEST(AsyncTest, DetachWithSpecificContext) {
    test_scheduler scheduler;
    actor_context context(scheduler);

    auto body = [&]() -> async<void> {
        auto* task = current_task();
        EXPECT_NE(task, nullptr);
        EXPECT_EQ(current_actor_context(), no_actor_context);
        co_await context();
        EXPECT_EQ(current_task(), task);
        EXPECT_EQ(current_actor_context(), context);
    };

    EXPECT_EQ(current_task(), nullptr);
    body().detach();
    EXPECT_EQ(current_task(), nullptr);
    // We are not running in the scheduler so it will preempt
    ASSERT_EQ(scheduler.queue.size(), 1u);
    EXPECT_EQ(scheduler.queue[0].deferred, false);
    scheduler.run_next();
    EXPECT_EQ(scheduler.queue.size(), 0u);
}

TEST(AsyncTest, DetachAwaitableWithSpecificContext) {
    test_scheduler scheduler;
    actor_context context(scheduler);

    auto body = [&]() -> async<void> {
        auto* task = current_task();
        EXPECT_NE(task, nullptr);
        EXPECT_EQ(current_actor_context(), no_actor_context);
        co_await context();
        EXPECT_EQ(current_task(), task);
        EXPECT_EQ(current_actor_context(), context);
    };

    EXPECT_EQ(current_task(), nullptr);
    detach_awaitable(body());
    EXPECT_EQ(current_task(), nullptr);
    // We are not running in the scheduler so it will preempt
    ASSERT_EQ(scheduler.queue.size(), 1u);
    EXPECT_EQ(scheduler.queue[0].deferred, false);
    scheduler.run_next();
    EXPECT_EQ(scheduler.queue.size(), 0u);
}

TEST(AsyncTest, ActorContextInheritance) {
    test_scheduler scheduler;
    actor_context context(scheduler);

    auto body = [&]() -> async<void> {
        auto* task = current_task();
        EXPECT_NE(task, nullptr);
        EXPECT_EQ(current_actor_context(), no_actor_context);
        // Explicitly await caller context
        co_await actor_context::caller_context();
        EXPECT_EQ(current_task(), task);
        EXPECT_EQ(current_actor_context(), no_actor_context);
        // Explicitly await specific context
        co_await context();
        EXPECT_EQ(current_task(), task);
        EXPECT_EQ(current_actor_context(), context);
        co_await [&]() -> async<void> {
            EXPECT_EQ(current_task(), task);
            EXPECT_EQ(current_actor_context(), context);
            // Explicitly await caller context
            co_await actor_context::caller_context();
            EXPECT_EQ(current_task(), task);
            EXPECT_EQ(current_actor_context(), context);
            // Explicitly change to empty context
            co_await no_actor_context();
            EXPECT_EQ(current_task(), task);
            EXPECT_EQ(current_actor_context(), no_actor_context);
        }();
        // Context is restored when we return
        EXPECT_EQ(current_task(), task);
        EXPECT_EQ(current_actor_context(), context);
    };

    auto r = packaged_awaitable(body());
    EXPECT_TRUE(r.running());
    // Note: we are entering context for the first time, deferred == false
    ASSERT_EQ(scheduler.queue.size(), 1u);
    EXPECT_EQ(scheduler.queue[0].deferred, false);
    scheduler.run_next();
    EXPECT_EQ(scheduler.queue.size(), 0u);
    EXPECT_TRUE(r.success());
}

TEST(AsyncTest, ActorWithoutContextAwaitActorWithContext) {
    test_scheduler scheduler;
    actor_context context(scheduler);

    auto body = [&]() -> async<void> {
        auto* task = current_task();
        EXPECT_NE(task, nullptr);
        EXPECT_EQ(current_actor_context(), no_actor_context);
        co_await [&]() -> async<void> {
            EXPECT_EQ(current_task(), task);
            EXPECT_EQ(current_actor_context(), no_actor_context);
            co_await context();
            EXPECT_EQ(current_task(), task);
            EXPECT_EQ(current_actor_context(), context);
        }();
        EXPECT_EQ(current_task(), task);
        EXPECT_EQ(current_actor_context(), no_actor_context);
    };

    auto r = packaged_awaitable(body());
    EXPECT_TRUE(r.running());
    // Note: we are entering context for the first time, deferred == false
    ASSERT_EQ(scheduler.queue.size(), 1u);
    EXPECT_EQ(scheduler.queue[0].deferred, false);
    scheduler.run_next();
    EXPECT_EQ(scheduler.queue.size(), 0u);
    EXPECT_TRUE(r.success());
}

TEST(AsyncTest, ActorWithContextAwaitActorWithoutContext) {
    test_scheduler scheduler;
    actor_context context(scheduler);

    auto body = [&]() -> async<void> {
        auto* task = current_task();
        EXPECT_NE(task, nullptr);
        EXPECT_EQ(current_actor_context(), no_actor_context);
        co_await context();
        EXPECT_EQ(current_task(), task);
        EXPECT_EQ(current_actor_context(), context);
        co_await [&]() -> async<void> {
            EXPECT_EQ(current_task(), task);
            EXPECT_EQ(current_actor_context(), context);
            co_await no_actor_context();
            EXPECT_EQ(current_task(), task);
            EXPECT_EQ(current_actor_context(), no_actor_context);
        }();
        EXPECT_EQ(current_task(), task);
        EXPECT_EQ(current_actor_context(), context);
    };

    auto r = packaged_awaitable(body());
    EXPECT_TRUE(r.running());
    // Note: we are entering context for the first time, deferred == false
    ASSERT_EQ(scheduler.queue.size(), 1u);
    EXPECT_EQ(scheduler.queue[0].deferred, false);
    scheduler.run_next();
    // On the way back we will resume without rescheduling
    EXPECT_EQ(scheduler.queue.size(), 0u);
    EXPECT_TRUE(r.success());
}

TEST(AsyncTest, StartNestedWithContext) {
    test_scheduler scheduler;
    actor_context context(scheduler);

    std::optional<packaged_awaitable<void>> r1;
    int stage = 0;
    auto nested = [&]() -> async<void> {
        auto* task = current_task();
        EXPECT_NE(task, nullptr);
        EXPECT_EQ(current_actor_context(), no_actor_context);
        stage = 1;
        co_await context();
        stage = 2;
        EXPECT_EQ(current_task(), task);
        EXPECT_EQ(current_actor_context(), context);
    };
    auto body = [&]() -> async<void> {
        auto* task = current_task();
        EXPECT_NE(task, nullptr);
        EXPECT_EQ(current_actor_context(), no_actor_context);
        r1.emplace(nested());
        EXPECT_TRUE(r1->running());
        co_return;
    };

    auto r = packaged_awaitable(body());
    // Note: we are entering context for the first time, deferred == false
    EXPECT_EQ(stage, 1);
    ASSERT_EQ(scheduler.queue.size(), 1u);
    EXPECT_EQ(scheduler.queue[0].deferred, false);
    scheduler.run_next();
    EXPECT_EQ(scheduler.queue.size(), 0u);
    EXPECT_TRUE(r1->success());
}

async<void> actor_check_sleep(const actor_context& context, bool expected,
        std::function<void()> before_sleep = {})
{
    co_await context();
    if (before_sleep) {
        before_sleep();
    }
    bool success = co_await context.sleep_for(std::chrono::milliseconds(100));
    EXPECT_EQ(success, expected);
}

TEST(AsyncTest, Sleep) {
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

TEST(AsyncTest, AwaitAsyncVariants) {
    auto body = []() -> async<int> {
        auto* task = current_task();
        EXPECT_TRUE(task != nullptr);
        int value = co_await [&]() -> async<int> {
            EXPECT_EQ(current_task(), task);
            co_return 42;
        }();
        EXPECT_EQ(current_task(), task);
        co_await [&]() -> async<void> {
            EXPECT_EQ(current_task(), task);
            co_return;
        }();
        EXPECT_EQ(current_task(), task);
        auto r = co_await [&]() -> async<void> {
            EXPECT_EQ(current_task(), task);
            co_return;
        }().result();
        EXPECT_TRUE(r.has_value());
        EXPECT_EQ(current_task(), task);
        co_return value;
    };

    EXPECT_EQ(current_task(), nullptr);
    auto r = packaged_awaitable(body());
    EXPECT_EQ(current_task(), nullptr);
    ASSERT_TRUE(r.success());
    ASSERT_EQ(*r, 42);
}

TEST(AsyncTest, AwaitOtherNonAsync) {
    test_channel<int> provider;

    auto body = [&]() -> async<int> {
        auto* task = current_task();
        EXPECT_TRUE(task != nullptr);
        int value = co_await provider.get();
        EXPECT_EQ(current_task(), task);
        co_return value;
    };

    EXPECT_EQ(current_task(), nullptr);
    auto r = packaged_awaitable(body());
    EXPECT_EQ(current_task(), nullptr);
    ASSERT_TRUE(!r.success());
    provider.take().resume(42);
    EXPECT_EQ(current_task(), nullptr);
    ASSERT_TRUE(r.success());
    ASSERT_EQ(*r, 42);
}

TEST(AsyncTest, AwaitContext) {
    test_scheduler scheduler;
    test_channel<int> provider;
    actor_context context(scheduler);
    actor_context context2(scheduler);
    int stage = 0;

    auto body = [&]() -> async<int> {
        auto* task = current_task();
        EXPECT_TRUE(task != nullptr);
        EXPECT_EQ(task->context, no_actor_context);
        stage = 1;
        co_await context();
        stage = 2;
        EXPECT_EQ(current_task(), task);
        EXPECT_EQ(task->context, context);
        co_await [&]() -> async<void> {
            EXPECT_EQ(current_task(), task);
            EXPECT_EQ(task->context, context);
            stage = 3;
            co_await context2();
            stage = 4;
            co_await actor_context::caller_context([&]() -> async<void> {
                EXPECT_EQ(current_task(), task);
                EXPECT_EQ(task->context, context2);
                co_return;
            }());
            EXPECT_EQ(current_task(), task);
            EXPECT_EQ(task->context, context);
            co_await context2([&]() -> async<void> {
                EXPECT_EQ(current_task(), task);
                EXPECT_EQ(task->context, context);
                co_return;
            }());
            co_await [&]() -> async<void> {
                EXPECT_EQ(task->context, context2);
                co_await actor_context::caller_context([&]() -> async<void> {
                    EXPECT_EQ(task->context, context2);
                    co_await context();
                    EXPECT_EQ(task->context, context);
                    co_return;
                }());
                EXPECT_EQ(task->context, context2);
            }();
            EXPECT_EQ(current_task(), task);
            EXPECT_EQ(task->context, context2);
        }();
        stage = 5;
        EXPECT_EQ(current_task(), task);
        EXPECT_EQ(task->context, context);
        co_return 42;
    };

    EXPECT_EQ(current_task(), nullptr);
    auto r = packaged_awaitable(body());
    EXPECT_EQ(current_task(), nullptr);
    EXPECT_EQ(stage, 1);
    EXPECT_FALSE(r.success());
    ASSERT_EQ(scheduler.queue.size(), 1u);
    EXPECT_EQ(scheduler.queue[0].deferred, false);
    scheduler.run_next();
    EXPECT_EQ(stage, 5);
    ASSERT_TRUE(r.success());
}

struct aborted_suspend {
    bool await_ready() {
        return false;
    }

    bool await_suspend(std::coroutine_handle<>) {
        // Abort suspend and resume
        return false;
    }

    int await_resume() {
        return 42;
    }
};

TEST(AsyncTest, AbortedSuspend) {
    test_scheduler scheduler;
    actor_context context(scheduler);

    auto body = [&]() -> async<void> {
        auto* task = current_task();
        EXPECT_NE(task, nullptr);
        co_await context();
        EXPECT_EQ(current_task(), task);

        int value = co_await aborted_suspend{};
        EXPECT_EQ(value, 42);
        EXPECT_EQ(current_task(), task);
        EXPECT_EQ(current_actor_context(), context);
    };

    auto r1 = packaged_awaitable(body());
    auto r2 = packaged_awaitable(body());
    ASSERT_EQ(scheduler.queue.size(), 1u);
    EXPECT_EQ(scheduler.queue[0].deferred, false); // initial post
    scheduler.run_next();
    // Suspend is aborted, so we expect no context switch on the return path
    EXPECT_TRUE(r1.success());
    ASSERT_EQ(scheduler.queue.size(), 1u);
    EXPECT_EQ(scheduler.queue[0].deferred, false); // parallel return and new activity
    scheduler.run_next();
    EXPECT_TRUE(r2.success());
    EXPECT_EQ(scheduler.queue.size(), 0u);
}

struct throw_during_suspend {
    bool await_ready() {
        return false;
    }

    void await_suspend(std::coroutine_handle<>) {
        throw std::runtime_error("throw during suspend");
    }

    void await_resume() {
        // should be unreachable
    }
};

TEST(AsyncTest, ThrowDuringSuspend) {
    test_scheduler scheduler;
    actor_context context(scheduler);

    auto body = [&]() -> async<void> {
        auto* task = current_task();
        EXPECT_NE(task, nullptr);
        co_await context();
        EXPECT_EQ(current_task(), task);

        EXPECT_THROW(co_await throw_during_suspend{}, std::runtime_error);
        EXPECT_EQ(current_task(), task);
        EXPECT_EQ(current_actor_context(), context);
    };

    auto r = packaged_awaitable(body());
    ASSERT_EQ(scheduler.queue.size(), 1u);
    EXPECT_EQ(scheduler.queue[0].deferred, false); // initial post
    scheduler.run_next();
    EXPECT_TRUE(r.success());
    EXPECT_EQ(scheduler.queue.size(), 0u);
}

TEST(AsyncTest, StartNestedActorsBeforeContext) {
    test_scheduler scheduler;
    actor_context context(scheduler);
    std::vector<int> events;

    std::optional<packaged_awaitable<int>> ropt;

    // We need to start everything inside a scheduler
    scheduler.run_in_scheduler([&]{
        ropt.emplace(
            [](const actor_context& context, std::vector<int>& events) -> async<int> {
                task_group<int> g;

                events.push_back(11);
                g.add([](const actor_context& context, std::vector<int>& events) -> async<int> {
                    events.push_back(21);
                    co_await context();
                    events.push_back(22);
                    co_return 42;
                }(context, events));

                events.push_back(12);
                g.add([](const actor_context& context, std::vector<int>& events) -> async<int> {
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

TEST(AsyncTest, SwitchBetweenSchedulers) {
    int stage = 0;
    test_scheduler scheduler1;
    actor_context context1(scheduler1);
    test_scheduler scheduler2;
    actor_context context2(scheduler2);

    auto r = packaged_awaitable(
        [](int& stage, const actor_context& context1, const actor_context& context2) -> async<void> {
            stage = 1;
            co_await context1();
            stage = 2;
            co_await [](int& stage, const actor_context& context2) -> async<void> {
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
