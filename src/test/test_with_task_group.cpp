#include <coroactors/with_task_group.h>
#include <gtest/gtest.h>

#include "test_common.h"
#include "test_channel.h"
#include "test_scheduler.h"
#include <coroactors/actor.h>
#include <coroactors/packaged_awaitable.h>

using namespace coroactors;

namespace {

actor<void> do_with_task_group_cancel(int& stage, test_channel<int>& provider) {
    co_await no_actor_context();

    try {
        co_await with_task_group<int>([&](task_group<int>& group) -> actor<void> {
            co_await actor_context::caller_context();

            group.add(provider.get());
            group.add(provider.get());

            int value;
            try {
                stage = 1;
                value = co_await group.next();
            } catch(...) {
                stage = 2;
                throw;
            }

            stage = 3;
            EXPECT_EQ(value, 42);
        });
    } catch(...) {
        stage = 4;
        throw;
    }

    stage = 5;
}

} // namespace

TEST(WithTaskGroupTest, ImplicitCancel) {
    int stage = 0;
    test_channel<int> provider;

    auto result = packaged_awaitable(
        do_with_task_group_cancel(stage, provider));

    EXPECT_EQ(stage, 1); // waiting on group.next()
    EXPECT_TRUE(result.running());
    ASSERT_EQ(provider.awaiters(), 2u);
    auto a = provider.take();
    auto b = provider.take();
    EXPECT_FALSE(a.get_stop_token().stop_requested());
    a.resume(42);
    EXPECT_EQ(stage, 3); // returned from group.next()
    EXPECT_TRUE(result.running());
    EXPECT_TRUE(b.get_stop_token().stop_requested());
    b.destroy();
    EXPECT_EQ(stage, 5); // returned from with_task_group
    EXPECT_TRUE(result.success());
}

TEST(WithTaskGroupTest, ImplicitCancelException) {
    int stage = 1;
    test_channel<int> provider;

    auto result = packaged_awaitable(
        do_with_task_group_cancel(stage, provider));

    EXPECT_EQ(stage, 1); // waiting on group.next()
    EXPECT_TRUE(result.running());
    ASSERT_EQ(provider.awaiters(), 2u);
    auto a = provider.take();
    auto b = provider.take();
    EXPECT_FALSE(a.get_stop_token().stop_requested());
    a.resume_with_exception(no_value_error());
    EXPECT_EQ(stage, 2); // group.next() thrown an exception
    EXPECT_TRUE(result.running());
    EXPECT_TRUE(b.get_stop_token().stop_requested());
    b.destroy();
    EXPECT_EQ(stage, 4); // with_task_group thrown an exception
    EXPECT_THROW(*result, no_value_error);
}

TEST(WithTaskGroupTest, ExplicitCancel) {
    int stage = 0;
    test_channel<int> provider;

    stop_source source;

    auto result = packaged_awaitable(
        with_stop_token(
            source.get_token(),
            do_with_task_group_cancel(stage, provider)));

    EXPECT_EQ(stage, 1); // waiting on group.next()
    EXPECT_TRUE(result.running());
    ASSERT_EQ(provider.awaiters(), 2u);
    auto a = provider.take();
    auto b = provider.take();
    EXPECT_FALSE(a.get_stop_token().stop_requested());
    EXPECT_FALSE(b.get_stop_token().stop_requested());
    source.request_stop();
    EXPECT_TRUE(a.get_stop_token().stop_requested());
    EXPECT_TRUE(b.get_stop_token().stop_requested());
    EXPECT_EQ(stage, 1); // still waiting on group.next();
    EXPECT_TRUE(result.running());
    b.resume(42);
    EXPECT_EQ(stage, 3); // returned from group.next()
    EXPECT_TRUE(result.running());
    a.resume_with_exception(no_value_error());
    EXPECT_EQ(stage, 5); // returned from with_task_group
    EXPECT_TRUE(result.success());
}

namespace {

struct move_only_int {
    int value;

    explicit move_only_int(int value)
        : value(value)
    {}

    move_only_int(const move_only_int&) = delete;
    move_only_int& operator=(const move_only_int&) = delete;

    move_only_int(move_only_int&& rhs)
        : value(std::exchange(rhs.value, 0))
    {}
};

actor<void> do_with_task_group_result_type(int& stage, test_channel<int>& provider) {
    co_await no_actor_context();

    auto result = co_await with_task_group<int>(
        [&](task_group<int>& group) -> actor<move_only_int> {
            co_await actor_context::caller_context();

            group.add(provider.get());
            group.add(provider.get());

            stage = 1;
            int a = co_await group.next();

            stage = 2;
            int b = co_await group.next();

            stage = 3;
            co_return move_only_int(a + b);
        });

    stage = 4;

    // with_task_group should autodetect the return type of a callback coroutine
    static_assert(std::is_same_v<decltype(result), move_only_int>);

    EXPECT_EQ(result.value, 100);
}

} // namespace

TEST(WithTaskGroupTest, ResultType) {
    int stage = 0;
    test_channel<int> provider;

    auto result = packaged_awaitable(
        do_with_task_group_result_type(stage, provider));

    EXPECT_EQ(stage, 1);
    EXPECT_TRUE(result.running());
    ASSERT_EQ(provider.awaiters(), 2u);
    auto a = provider.take();
    auto b = provider.take();
    a.resume(42);
    EXPECT_EQ(stage, 2);
    EXPECT_TRUE(result.running());
    b.resume(58);
    EXPECT_EQ(stage, 4);
    EXPECT_TRUE(result.success());
}

namespace {

actor<void> do_with_stop_token_context(int& stage, test_scheduler& scheduler, test_channel<int>& provider) {
    actor_context context(scheduler);

    stage = 1;
    co_await context();

    // Double check we are running in our context
    EXPECT_EQ(context, co_await actor_context::current_context);

    stage = 2;
    int result = co_await with_stop_token(
        stop_token(),
        with_task_group<int>([&](task_group<int>& group) -> actor<int> {
            stage = 3;
            co_await actor_context::caller_context();

            // We expect with_stop_token to not interfere with our context
            EXPECT_EQ(context, co_await actor_context::current_context);

            group.add(provider.get());
            group.add(provider.get());

            stage = 4;
            int a = co_await group.next();

            stage = 5;
            int b = co_await group.next();

            EXPECT_FALSE((co_await actor_context::current_stop_token).stop_requested());

            stage = 6;
            co_return a + b;
        }));

    stage = 7;
    EXPECT_EQ(result, 100);
    EXPECT_TRUE((co_await actor_context::current_stop_token).stop_requested());
}

} // namespace

TEST(WithTaskGroupTest, WithStopTokenContext) {
    int stage = 0;
    test_scheduler scheduler;
    test_channel<int> provider;
    stop_source source;

    auto result = packaged_awaitable(
        with_stop_token(
            source.get_token(),
            do_with_stop_token_context(stage, scheduler, provider)));

    EXPECT_EQ(stage, 1); // waiting for context
    ASSERT_EQ(scheduler.queue.size(), 1u);
    scheduler.run_next();
    EXPECT_EQ(scheduler.queue.size(), 0u);

    EXPECT_EQ(stage, 4); // waiting for the first value
    ASSERT_EQ(provider.awaiters(), 2u);
    auto a = provider.take();
    auto b = provider.take();
    EXPECT_FALSE(a.get_stop_token().stop_requested());
    EXPECT_FALSE(b.get_stop_token().stop_requested());

    // Cancel our source, we expect task group to be isolated
    source.request_stop();
    EXPECT_FALSE(a.get_stop_token().stop_requested());
    EXPECT_FALSE(b.get_stop_token().stop_requested());

    a.resume(42);
    EXPECT_EQ(stage, 4); // waiting for the first value (context resume)
    ASSERT_EQ(scheduler.queue.size(), 1u);
    scheduler.run_next();
    EXPECT_EQ(stage, 5); // waiting for the second value

    b.resume(58);
    EXPECT_EQ(stage, 5); // waiting for the second value (context resume)
    ASSERT_EQ(scheduler.queue.size(), 1u);
    scheduler.run_next();
    EXPECT_EQ(stage, 7); // finished
    EXPECT_TRUE(result.success());

    EXPECT_EQ(scheduler.queue.size(), 0u);
}

namespace {

template<class WhenReadyCall>
actor<void> do_when_ready_with_token(int& stage, actor_scheduler& scheduler, test_channel<int>& provider,
        WhenReadyCall when_ready, bool expected)
{
    stage = 1;
    co_await actor_context(scheduler)();

    stage = 2;
    int result = co_await with_task_group<int>(
        [&](task_group<int>& group) -> actor<int> {
            stage = 3;
            co_await actor_context::caller_context();

            group.add(provider.get());
            group.add(provider.get());

            stage = 4;
            bool r = co_await when_ready(group);
            // bool r = co_await with_stop_token(token, group.when_ready());
            EXPECT_EQ(r, expected);

            stage = 5;
            int a = co_await group.next();

            stage = 6;
            int b = co_await group.next();

            // Note: gcc fails with 'insufficient contextual information' when
            // a co_await expression with a method call is in paranthesis.
            auto token = co_await actor_context::current_stop_token;
            EXPECT_FALSE(token.stop_requested());

            stage = 7;
            co_return a + b;
        });

    stage = 8;
    EXPECT_EQ(result, 100);
}

} // namespace

TEST(WithTaskGroupTest, WaitReadySuccess) {
    int stage = 0;
    test_scheduler scheduler;
    test_channel<int> provider;
    stop_source source;

    auto when_ready = [&](auto& group) {
        return with_stop_token(source.get_token(), group.when_ready());
    };

    auto result = packaged_awaitable(
        do_when_ready_with_token(stage, scheduler, provider, when_ready, true));

    EXPECT_EQ(stage, 1); // waiting for context
    ASSERT_EQ(scheduler.queue.size(), 1u);
    scheduler.run_next();
    EXPECT_EQ(scheduler.queue.size(), 0u);

    EXPECT_EQ(stage, 4); // waiting in wait_next
    ASSERT_EQ(provider.awaiters(), 2u);
    auto a = provider.take();
    auto b = provider.take();

    a.resume(42);
    EXPECT_EQ(stage, 4); // waiting on wait_next (context resume)
    ASSERT_EQ(scheduler.queue.size(), 1u);
    scheduler.run_next();
    EXPECT_EQ(stage, 6); // waiting for the second value

    b.resume(58);
    EXPECT_EQ(stage, 6); // waiting for the second value (context resume)
    ASSERT_EQ(scheduler.queue.size(), 1u);
    scheduler.run_next();
    EXPECT_EQ(stage, 8); // finished
    EXPECT_TRUE(result.success());

    EXPECT_EQ(scheduler.queue.size(), 0u);
}

TEST(WithTaskGroupTest, WaitReadyCancelled) {
    int stage = 0;
    test_scheduler scheduler;
    test_channel<int> provider;
    stop_source source;

    auto when_ready = [&](auto& group) {
        return with_stop_token(source.get_token(), group.when_ready());
    };

    auto result = packaged_awaitable(
        do_when_ready_with_token(stage, scheduler, provider, when_ready, false));

    EXPECT_EQ(stage, 1); // waiting for context
    ASSERT_EQ(scheduler.queue.size(), 1u);
    scheduler.run_next();
    EXPECT_EQ(scheduler.queue.size(), 0u);

    EXPECT_EQ(stage, 4); // waiting in wait_next
    ASSERT_EQ(provider.awaiters(), 2u);
    auto a = provider.take();
    auto b = provider.take();

    source.request_stop();
    EXPECT_EQ(stage, 4); // waiting on wait_next (context resume)
    ASSERT_EQ(scheduler.queue.size(), 1u);
    scheduler.run_next();
    EXPECT_EQ(stage, 5); // waiting for the first value

    a.resume(42);
    EXPECT_EQ(stage, 5); // waiting for the first value (context resume)
    ASSERT_EQ(scheduler.queue.size(), 1u);
    scheduler.run_next();
    EXPECT_EQ(stage, 6); // waiting for the second value

    b.resume(58);
    EXPECT_EQ(stage, 6); // waiting for the second value (context resume)
    ASSERT_EQ(scheduler.queue.size(), 1u);
    scheduler.run_next();
    EXPECT_EQ(stage, 8); // finished
    EXPECT_TRUE(result.success());

    EXPECT_EQ(scheduler.queue.size(), 0u);
}

TEST(WithTaskGroupTest, WaitReadyCancelledBeforeAwait) {
    int stage = 0;
    test_scheduler scheduler;
    test_channel<int> provider;
    stop_source source;

    auto when_ready = [&](auto& group) {
        source.request_stop();
        return with_stop_token(source.get_token(), group.when_ready());
    };

    auto result = packaged_awaitable(
        do_when_ready_with_token(stage, scheduler, provider, when_ready, false));

    EXPECT_EQ(stage, 1); // waiting for context
    ASSERT_EQ(scheduler.queue.size(), 1u);
    scheduler.run_next();
    EXPECT_EQ(scheduler.queue.size(), 0u);

    EXPECT_EQ(stage, 5); // waiting for the first value
    ASSERT_EQ(provider.awaiters(), 2u);
    auto a = provider.take();
    auto b = provider.take();

    a.resume(42);
    EXPECT_EQ(stage, 5); // waiting for the first value (context resume)
    ASSERT_EQ(scheduler.queue.size(), 1u);
    scheduler.run_next();
    EXPECT_EQ(stage, 6); // waiting for the second value

    b.resume(58);
    EXPECT_EQ(stage, 6); // waiting for the second value (context resume)
    ASSERT_EQ(scheduler.queue.size(), 1u);
    scheduler.run_next();
    EXPECT_EQ(stage, 8); // finished
    EXPECT_TRUE(result.success());

    EXPECT_EQ(scheduler.queue.size(), 0u);
}

namespace {

template<class Awaiter, class Hook>
struct await_suspend_hook {
    Awaiter awaiter;
    Hook hook;

    bool await_ready() {
        return awaiter.await_ready();
    }

    decltype(auto) await_suspend(std::coroutine_handle<> h) {
        hook();
        return awaiter.await_suspend(h);
    }

    decltype(auto) await_resume() {
        return awaiter.await_resume();
    }
};

template<class Awaiter, class Hook>
await_suspend_hook(Awaiter, Hook) -> await_suspend_hook<Awaiter, Hook>;

} // namespace

TEST(WithTaskGroupTest, WaitReadyCancelledBeforeSuspend) {
    int stage = 0;
    test_scheduler scheduler;
    test_channel<int> provider;
    stop_source source;

    auto when_ready = [&](auto& group) {
        return await_suspend_hook{
            with_stop_token(source.get_token(), group.when_ready()),
            [&]{
                source.request_stop();
            },
        };
    };

    auto result = packaged_awaitable(
        do_when_ready_with_token(stage, scheduler, provider, when_ready, false));

    EXPECT_EQ(stage, 1); // waiting for context
    ASSERT_EQ(scheduler.queue.size(), 1u);
    scheduler.run_next();
    EXPECT_EQ(scheduler.queue.size(), 0u);

    ASSERT_EQ(provider.awaiters(), 2u);
    auto a = provider.take();
    auto b = provider.take();

    // Note: no actor context switch since we returned immediately
    EXPECT_EQ(stage, 5); // waiting for the first value
    ASSERT_EQ(scheduler.queue.size(), 0u);

    a.resume(42);
    EXPECT_EQ(stage, 5); // waiting for the first value (context resume)
    ASSERT_EQ(scheduler.queue.size(), 1u);
    scheduler.run_next();
    EXPECT_EQ(stage, 6); // waiting for the second value
    ASSERT_EQ(scheduler.queue.size(), 0u);

    b.resume(58);
    EXPECT_EQ(stage, 6); // waiting for the second value (context resume)
    ASSERT_EQ(scheduler.queue.size(), 1u);
    scheduler.run_next();
    EXPECT_EQ(stage, 8); // finished
    EXPECT_TRUE(result.success());

    EXPECT_EQ(scheduler.queue.size(), 0u);
}
