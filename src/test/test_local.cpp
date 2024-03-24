#include <coroactors/local.h>
#include <gtest/gtest.h>

#include "test_common.h"
#include "test_channel.h"
#include "test_scheduler.h"
#include <coroactors/actor.h>
#include <coroactors/packaged_awaitable.h>
#include <coroactors/with_task_group.h>
#include <coroactors/task.h>

using namespace coroactors;

TEST(CoroutineLocal, DefaultValue) {
    coroutine_local<int> value1;
    EXPECT_EQ(value1.get(), 0);
    coroutine_local<int> value2(42);
    EXPECT_EQ(value2.get(), 42);
}

TEST(CoroutineLocal, WithValue) {
    coroutine_local<int> value;
    test_channel<int> provider;

    auto outer = [&](int override) -> actor<void> {
        EXPECT_EQ(value.get(), 0);
        co_await no_actor_context();
        EXPECT_EQ(value.get(), 0);

        co_await value.with_value(override, [&]() -> actor<void> {
            // Note: we start running before the value is set, so it is still 0
            EXPECT_EQ(value.get(), 0);
            co_await actor_context::caller_context();
            EXPECT_EQ(value.get(), override);

            co_await [&]() -> actor<void> {
                EXPECT_EQ(value.get(), override);
                co_await actor_context::caller_context();
                EXPECT_EQ(value.get(), override);

                co_await provider.get();

                EXPECT_EQ(value.get(), override);
            }();

            EXPECT_EQ(value.get(), override);
        }());

        EXPECT_EQ(value.get(), 0);
    };

    auto result1 = packaged_awaitable(outer(51));
    ASSERT_TRUE(result1.running());
    auto a = provider.take();

    auto result2 = packaged_awaitable(outer(52));
    ASSERT_TRUE(result2.running());
    auto b = provider.take();

    a.resume(1);
    EXPECT_TRUE(result1.success());

    b.resume(2);
    EXPECT_TRUE(result2.success());
}

TEST(CoroutineLocal, LocalsNotInheritedInDetachedActors) {
    coroutine_local<int> value;
    test_channel<int> provider;

    auto detached = [&]() -> actor<void> {
        EXPECT_EQ(value.get(), 51); // initially we run with the bound value
        co_await no_actor_context();
        EXPECT_EQ(value.get(), 0); // detached actors don't inherit coroutine locals
        co_await provider.get();
        EXPECT_EQ(value.get(), 0); // value is still unbound
    };

    auto outer = [&]() -> actor<void> {
        co_await no_actor_context();

        co_await value.with_value(51, [&]() -> actor<void> {
            co_await actor_context::caller_context();

            EXPECT_EQ(value.get(), 51);
            detached().detach();
            EXPECT_EQ(value.get(), 51);
            provider.take().resume(1);
            EXPECT_EQ(value.get(), 51);
        }());

        EXPECT_EQ(value.get(), 0);
    };

    auto result = packaged_awaitable(outer());
    EXPECT_TRUE(result.success());
}

TEST(CoroutineLocal, LocalsNotInheritedInCustomTaskGroup) {
    coroutine_local<int> value;
    test_channel<int> provider;

    auto in_task_group = [&]() -> actor<void> {
        EXPECT_EQ(value.get(), 51); // initially we run with the bound value
        co_await no_actor_context();
        EXPECT_EQ(value.get(), 0); // value not inherited by the task group
        co_await provider.get();
        EXPECT_EQ(value.get(), 0); // value is still unbound
    };

    auto outer = [&]() -> actor<void> {
        co_await no_actor_context();

        co_await value.with_value(51, [&]() -> actor<void> {
            co_await actor_context::caller_context();

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

    auto result = packaged_awaitable(outer());
    EXPECT_TRUE(result.success());
}

TEST(CoroutineLocal, LocalsInheritedByWithTaskGroup) {
    coroutine_local<int> value;
    test_channel<int> provider;

    auto in_task_group = [&]() -> actor<void> {
        EXPECT_EQ(value.get(), 52); // initially we run with the currently bound value
        co_await no_actor_context();
        EXPECT_EQ(value.get(), 51); // now we must see the inherited value
        co_await provider.get();
        EXPECT_EQ(value.get(), 51); // value is still inherited
    };

    auto outer = [&]() -> actor<void> {
        co_await no_actor_context();

        co_await value.with_value(51, [&]() -> actor<void> {
            co_await actor_context::caller_context();

            co_await with_task_group<void>([&](task_group<void>& group) -> actor<void> {
                EXPECT_EQ(value.get(), 51);
                co_await actor_context::caller_context();
                EXPECT_EQ(value.get(), 51);

                co_await value.with_value(52, [&]() -> actor<void> {
                    EXPECT_EQ(value.get(), 51);
                    co_await actor_context::caller_context();
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

TEST(CoroutineLocal, LocalsNotBoundOutsideActors) {
    coroutine_local<int> value;
    test_channel<int> provider;

    auto outer = [&]() -> actor<void> {
        co_await no_actor_context();

        co_await value.with_value(51, [&]() -> actor<void> {
            co_await actor_context::caller_context();

            EXPECT_EQ(value.get(), 51);
            co_await provider.get();
            EXPECT_EQ(value.get(), 51);

            co_await [&]() -> task<void> {
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

TEST(CoroutineLocal, LocalsAcrossContextSwitches) {
    coroutine_local<int> value;
    test_channel<int> provider;
    test_scheduler scheduler;
    actor_context context1(scheduler);
    actor_context context2(scheduler);

    auto outer2 = [&](int expected) -> actor<void> {
        co_await context2();
        EXPECT_EQ(value.get(), expected);
    };

    int outer1blocks = 0;
    int outer1resumes = 0;
    auto outer1 = [&](int val) -> actor<void> {
        co_await no_actor_context();

        co_await value.with_value(val, [&]() -> actor<void> {
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
    ASSERT_EQ(scheduler.queue.size(), 2u); // context1 first, then context2
    EXPECT_EQ(scheduler.queue[0].deferred, true); // continuation of outer1
    EXPECT_EQ(value.get(), 0); // value not bound yet
    scheduler.run_next();
    EXPECT_EQ(value.get(), 0); // value must not be bound
    // context2 is still blocked, but when the first coroutine calls outer2 we
    // should already have the second coroutine ready in the queue, and it
    // should replace it on the same stack.
    EXPECT_EQ(outer1blocks, 2);
    EXPECT_EQ(outer1resumes, 0);
    ASSERT_EQ(scheduler.queue.size(), 1u); // only context2 is waiting
    EXPECT_EQ(scheduler.queue[0].deferred, false); // parallel activity
    // This will start the first outer2 call, and since it returns to a
    // non-actor it will schedule the rest of the context2 queue in parallel
    EXPECT_TRUE(c.running());
    scheduler.run_next();
    EXPECT_TRUE(c.success());
    EXPECT_EQ(value.get(), 0); // value must not be bound
    EXPECT_EQ(outer1resumes, 0); // context1 did not resume yet
    ASSERT_EQ(scheduler.queue.size(), 1u);
    EXPECT_EQ(scheduler.queue[0].deferred, false); // parallel activity
    // This will resume context2, return to context1 and schedule context2 in parallel
    EXPECT_TRUE(a.running());
    scheduler.run_next();
    EXPECT_TRUE(a.success());
    EXPECT_EQ(value.get(), 0); // value must not be bound
    EXPECT_EQ(outer1resumes, 1);
    ASSERT_EQ(scheduler.queue.size(), 1u); // only context2 is waiting
    EXPECT_EQ(scheduler.queue[0].deferred, false); // parallel activity
    EXPECT_TRUE(b.running());
    scheduler.run_next();
    EXPECT_TRUE(b.success());
    EXPECT_EQ(value.get(), 0); // value must not be bound
    EXPECT_EQ(outer1resumes, 2);
    EXPECT_EQ(scheduler.queue.size(), 0u); // everything finished
}
