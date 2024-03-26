#include <coroactors/task_local.h>
#include <gtest/gtest.h>

#include "test_common.h"
#include "test_channel.h"
#include "test_scheduler.h"
#include <coroactors/async.h>
#include <coroactors/coro.h>
#include <coroactors/packaged_awaitable.h>
#include <coroactors/with_task_group.h>

using namespace coroactors;

TEST(TaskLocalTest, DefaultValue) {
    task_local<int> value1;
    EXPECT_EQ(value1.get(), 0);
    task_local<int> value2(42);
    EXPECT_EQ(value2.get(), 42);
}

TEST(TaskLocalTest, WithValue) {
    task_local<int> value;
    test_channel<int> provider;

    auto body = [&](int override) -> async<void> {
        EXPECT_EQ(value.get(), 0);

        co_await value.with_value(override, [&]() -> async<void> {
            // Note: we start running with the value already set
            EXPECT_EQ(value.get(), override);

            co_await [&]() -> async<void> {
                EXPECT_EQ(value.get(), override);

                co_await provider.get();

                EXPECT_EQ(value.get(), override);
            }();

            EXPECT_EQ(value.get(), override);
        }());

        EXPECT_EQ(value.get(), 0);
    };

    auto result1 = packaged_awaitable(body(51));
    ASSERT_TRUE(result1.running());
    EXPECT_EQ(value.get(), 0);
    auto a = provider.take();

    auto result2 = packaged_awaitable(body(52));
    ASSERT_TRUE(result2.running());
    EXPECT_EQ(value.get(), 0);
    auto b = provider.take();

    a.resume(1);
    EXPECT_TRUE(result1.success());
    EXPECT_EQ(value.get(), 0);

    b.resume(2);
    EXPECT_TRUE(result2.success());
    EXPECT_EQ(value.get(), 0);
}

TEST(TaskLocalTest, LocalsNotInheritedInDetachedActors) {
    task_local<int> value;
    test_channel<int> provider;

    auto detached = [&]() -> async<void> {
        EXPECT_EQ(value.get(), 0); // detached actors don't inherit coroutine locals
        co_await provider.get();
        EXPECT_EQ(value.get(), 0); // value is still unbound
    };

    auto body = [&]() -> async<void> {
        EXPECT_EQ(value.get(), 0);

        co_await value.with_value(51, [&]() -> async<void> {
            EXPECT_EQ(value.get(), 51);
            detached().detach();
            EXPECT_EQ(value.get(), 51);
            provider.take().resume(1);
            EXPECT_EQ(value.get(), 51);
            co_return;
        }());

        EXPECT_EQ(value.get(), 0);
    };

    auto result = packaged_awaitable(body());
    EXPECT_TRUE(result.success());
}

TEST(TaskLocalTest, LocalsNotInheritedInCustomTaskGroup) {
    task_local<int> value;
    test_channel<int> provider;

    auto in_task_group = [&]() -> async<void> {
        EXPECT_EQ(value.get(), 0); // value not inherited by the task group
        co_await provider.get();
        EXPECT_EQ(value.get(), 0); // value is still unbound
    };

    auto body = [&]() -> async<void> {
        co_await value.with_value(51, [&]() -> async<void> {
            EXPECT_EQ(value.get(), 51);
            task_group<void> group;
            group.add(in_task_group());
            EXPECT_EQ(value.get(), 51);
            provider.take().resume(1);
            EXPECT_EQ(value.get(), 51);
            co_await group.next();
            EXPECT_EQ(value.get(), 51);
        }());
    };

    auto result = packaged_awaitable(body());
    EXPECT_TRUE(result.success());
}

TEST(TaskLocalTest, LocalsInheritedByWithTaskGroup) {
    task_local<int> value;
    test_channel<int> provider;

    auto in_task_group = [&]() -> async<void> {
        EXPECT_EQ(value.get(), 51); // now we must see the inherited value
        co_await provider.get();
        EXPECT_EQ(value.get(), 51); // value is still inherited
    };

    auto outer = [&]() -> async<void> {
        co_await value.with_value(51, [&]() -> async<void> {
            co_await with_task_group<void>([&](task_group<void>& group) -> async<void> {
                EXPECT_EQ(value.get(), 51);
                co_await actor_context::caller_context();
                EXPECT_EQ(value.get(), 51);

                co_await value.with_value(52, [&]() -> async<void> {
                    EXPECT_EQ(value.get(), 52);
                    group.add(in_task_group());
                    EXPECT_EQ(value.get(), 52);
                    co_await group.next();
                    EXPECT_EQ(value.get(), 52);
                }());

                EXPECT_EQ(value.get(), 51);
            });
        }());
    };

    auto result = packaged_awaitable(outer());
    ASSERT_TRUE(result.running());
    provider.take().resume(1);
    EXPECT_TRUE(result.success());
}

TEST(TaskLocalTest, LocalsNotBoundOutsideActors) {
    task_local<int> value;
    test_channel<int> provider;

    auto outer = [&]() -> async<void> {
        co_await value.with_value(51, [&]() -> async<void> {
            EXPECT_EQ(value.get(), 51);
            co_await provider.get();
            EXPECT_EQ(value.get(), 51);

            co_await [&]() -> coro<void> {
                EXPECT_EQ(value.get(), 0);
                co_await provider.get();
                EXPECT_EQ(value.get(), 0);
            }();

            EXPECT_EQ(value.get(), 51);
        }());
    };

    auto result = packaged_awaitable(outer());
    EXPECT_EQ(value.get(), 0);
    ASSERT_TRUE(result.running());
    provider.take().resume(1);
    EXPECT_EQ(value.get(), 0);
    ASSERT_TRUE(result.running());
    provider.take().resume(2);
    EXPECT_TRUE(result.success());
}

TEST(TaskLocalTest, LocalsAcrossContextSwitches) {
    task_local<int> value;
    test_channel<int> provider;
    test_scheduler scheduler;
    actor_context context1(scheduler);
    actor_context context2(scheduler);

    auto outer2 = [&](int expected) -> async<void> {
        co_await context2();
        EXPECT_EQ(value.get(), expected);
    };

    int outer1blocks = 0;
    int outer1resumes = 0;
    auto outer1 = [&](int val) -> async<void> {
        co_await value.with_value(val, [&]() -> async<void> {
            EXPECT_EQ(value.get(), val);
            co_await context1();
            EXPECT_EQ(value.get(), val);
            ++outer1blocks;
            co_await outer2(val);
            ++outer1resumes;
            EXPECT_EQ(value.get(), val);
        }());
    };

    auto a = packaged_awaitable(outer1(51));
    auto b = packaged_awaitable(outer1(52));
    auto c = packaged_awaitable(outer2(0));
    EXPECT_EQ(outer1blocks, 0);
    ASSERT_EQ(scheduler.queue.size(), 2u); // context1 first, then context2
    EXPECT_EQ(scheduler.queue[0].deferred, false); // new parallel activity in outer1
    EXPECT_EQ(scheduler.queue[1].deferred, false); // new parallel activity in outer2
    auto context2cont = scheduler.remove(1); // we will unblock context2 later

    EXPECT_EQ(value.get(), 0); // value must not be bound
    scheduler.run_next();
    EXPECT_EQ(value.get(), 0); // value must not be bound

    // the first outer1 activated and blocked in the call to outer2(), and
    // now scheduler has activation for the second outer1
    EXPECT_EQ(outer1blocks, 1);
    EXPECT_EQ(outer1resumes, 0);
    ASSERT_EQ(scheduler.queue.size(), 1u); // the second outer1 is waiting
    EXPECT_EQ(scheduler.queue[0].deferred, true); // continuation of outer1
    scheduler.run_next();
    EXPECT_EQ(value.get(), 0); // value must not be bound

    // the second outer1 activated and blocked in the call to outer2()
    EXPECT_EQ(outer1blocks, 2);
    EXPECT_EQ(outer1resumes, 0);
    ASSERT_EQ(scheduler.queue.size(), 0u);

    // now we unblock the first call to context2
    scheduler.queue.push_back(std::move(context2cont));
    EXPECT_TRUE(c.running());
    scheduler.run_next();
    EXPECT_TRUE(c.success());
    EXPECT_EQ(value.get(), 0); // value must not be bound
    EXPECT_EQ(outer1resumes, 0);

    // unblock the first call
    ASSERT_EQ(scheduler.queue.size(), 1u); // waiting for the next outer2
    EXPECT_EQ(scheduler.queue[0].deferred, false); // it was in parallel with return from outer2
    scheduler.run_next();
    EXPECT_TRUE(a.success());
    EXPECT_EQ(value.get(), 0); // value must not be bound
    EXPECT_EQ(outer1resumes, 1);

    // unblock the second call
    ASSERT_EQ(scheduler.queue.size(), 1u); // waiting for the next outer2
    EXPECT_EQ(scheduler.queue[0].deferred, false); // it was in parallel with return from outer2
    scheduler.run_next();
    EXPECT_TRUE(b.success());
    EXPECT_EQ(value.get(), 0); // value must not be bound
    EXPECT_EQ(outer1resumes, 2);

    ASSERT_EQ(scheduler.queue.size(), 0u);
}
