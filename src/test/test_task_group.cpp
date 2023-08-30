#include <coroactors/task_group.h>
#include <gtest/gtest.h>

#include "test_common.h"
#include "test_channel.h"
#include "test_scheduler.h"
#include <coroactors/actor.h>
#include <coroactors/packaged_awaitable.h>

#include <deque>
#include <vector>
#include <functional>
#include <optional>
#include <mutex>
#include <thread>
#include <latch>

using namespace coroactors;

namespace {

enum class EAction {
    AddTask,
    AwaitTask,
    AwaitTaskWrapped,
    AwaitValue,
    Return,
};

actor<std::vector<int>> run_scenario(test_channel<int>& provider, std::function<EAction()> next) {
    co_await no_actor_context();
    std::vector<int> results;
    task_group<int> group;
    for (;;) {
        switch (next()) {
            case EAction::AddTask: {
                group.add(provider.get());
                break;
            }
            case EAction::AwaitTask: {
                results.push_back(co_await group.next());
                break;
            }
            case EAction::AwaitTaskWrapped: {
                auto result = co_await group.next_result();
                if (result.has_value()) {
                    results.push_back(std::move(result).take_value());
                } else if (result.has_exception()) {
                    results.push_back(-1);
                } else {
                    results.push_back(-2);
                }
                break;
            }
            case EAction::AwaitValue: {
                results.push_back(co_await provider.get());
                break;
            }
            case EAction::Return: {
                co_return results;
            }
        }
    }
}

} // namespace

TEST(TaskGroupTest, SimpleAsync) {
    size_t last_step = 0;
    test_channel<int> provider;

    auto result = packaged_awaitable(
        run_scenario(provider, [&]{
            auto step = ++last_step;
            if (step <= 3) {
                return EAction::AddTask;
            }
            if (step <= 6) {
                return EAction::AwaitTask;
            }
            return EAction::Return;
        }));

    ASSERT_EQ(provider.awaiters(), 3u);
    provider.resume(1);
    provider.resume(2);
    provider.resume(3);
    EXPECT_EQ(provider.awaiters(), 0u);
    ASSERT_TRUE(result.success());
    std::vector<int> expected{ 1, 2, 3 };
    EXPECT_EQ(*result, expected);
}

TEST(TaskGroupTest, SimpleSync) {
    size_t last_step = 0;
    test_channel<int> provider;

    provider.provide(1);
    provider.provide(2);
    provider.provide(3);

    auto result = packaged_awaitable(
        run_scenario(provider, [&]{
            auto step = ++last_step;
            if (step <= 3) {
                return EAction::AddTask;
            }
            if (step <= 6) {
                return EAction::AwaitTask;
            }
            return EAction::Return;
        }));

    EXPECT_EQ(provider.awaiters(), 0u);
    ASSERT_TRUE(result.success());
    std::vector<int> expected{ 1, 2, 3 };
    EXPECT_EQ(*result, expected);
}

TEST(TaskGroupTest, SimpleMultiThreaded) {
    size_t last_step = 0;
    test_channel<int> provider;

    auto result = packaged_awaitable(
        run_scenario(provider, [&]{
            auto step = ++last_step;
            if (step <= 10) {
                return EAction::AddTask;
            }
            if (step <= 20) {
                return EAction::AwaitTask;
            }
            return EAction::Return;
        }));

    ASSERT_EQ(provider.awaiters(), 10u);
    std::latch barrier(10);
    std::vector<std::thread> threads;
    for (int i = 1; i <= 10; ++i) {
        threads.emplace_back([i, c = provider.take(), &barrier]() mutable {
            barrier.arrive_and_wait();
            c.resume(i);
        });
    }
    for (auto& t : threads) {
        t.join();
    }
    ASSERT_TRUE(result.success());
    std::sort(result->begin(), result->end());
    std::vector<int> expected;
    for (int i = 1; i <= 10; ++i) {
        expected.push_back(i);
    }
    EXPECT_EQ(*result, expected);
}

TEST(TaskGroupTest, CompleteOutOfOrder) {
    size_t last_step = 0;
    test_channel<int> provider;

    auto result = packaged_awaitable(
        run_scenario(provider, [&]{
            auto step = ++last_step;
            if (step <= 3) {
                return EAction::AddTask;
            }
            if (step <= 6) {
                return EAction::AwaitTask;
            }
            return EAction::Return;
        }));

    ASSERT_EQ(provider.awaiters(), 3u);
    provider.resume_at(1, 2);
    provider.resume(1);
    provider.resume(3);
    EXPECT_EQ(provider.awaiters(), 0u);
    ASSERT_TRUE(result.success());
    std::vector<int> expected{ 2, 1, 3 };
    EXPECT_EQ(*result, expected);
}

TEST(TaskGroupTest, CompleteBeforeAwaited) {
    size_t last_step = 0;
    test_channel<int> provider;

    auto result = packaged_awaitable(
        run_scenario(provider, [&]{
            auto step = ++last_step;
            if (step <= 3) {
                return EAction::AddTask;
            }
            if (step == 4) {
                return EAction::AwaitValue;
            }
            if (step <= 7) {
                return EAction::AwaitTask;
            }
            return EAction::Return;
        }));

    ASSERT_EQ(provider.awaiters(), 4u);
    provider.resume(1);
    provider.resume(2);
    provider.resume(3);
    EXPECT_TRUE(result.running());
    ASSERT_EQ(provider.awaiters(), 1u);
    provider.resume(4);
    ASSERT_TRUE(result.success());
    std::vector<int> expected{ 4, 1, 2, 3 };
    EXPECT_EQ(*result, expected);
}

TEST(TaskGroupTest, LocalReadyQueue) {
    size_t last_step = 0;
    test_channel<int> provider;

    auto result = packaged_awaitable(
        run_scenario(provider, [&]{
            auto step = ++last_step;
            if (step <= 3) {
                return EAction::AddTask;
            }
            if (step == 4) {
                return EAction::AwaitValue;
            }
            if (step == 5) {
                return EAction::AwaitTask;
            }
            if (step == 6) {
                return EAction::AwaitValue;
            }
            if (step <= 8) {
                return EAction::AwaitTask;
            }
            return EAction::Return;
        }));

    ASSERT_EQ(provider.awaiters(), 4u);
    // Arrange for two tasks to be ready
    provider.resume(1);
    provider.resume(2);
    // Unblock coroutine, it will grab value 4 and value 1 and block again
    provider.resume_at(1, 4);
    EXPECT_TRUE(result.running());
    ASSERT_EQ(provider.awaiters(), 2u);
    // Unblock the last task, now we have both ready queue and atomic queue
    provider.resume(3);
    // Unblock coroutine again, it will grab value 5 and finally values 2 and 3
    provider.resume(5);

    ASSERT_TRUE(result.success());
    std::vector<int> expected{ 4, 1, 5, 2, 3 };
    EXPECT_EQ(*result, expected);
}

TEST(TaskGroupTest, DetachAll) {
    size_t last_step = 0;
    test_channel<int> provider;

    auto result = packaged_awaitable(
        run_scenario(provider, [&]{
            auto step = ++last_step;
            if (step <= 3) {
                return EAction::AddTask;
            }
            return EAction::Return;
        }));

    ASSERT_TRUE(result.success());
    EXPECT_EQ(result->size(), 0u);
    EXPECT_EQ(provider.awaiters(), 3u);
    // Note: test_channel destructor will fail all tasks
}

TEST(TaskGroupTest, ResumeException) {
    size_t last_step = 0;
    test_channel<int> provider;

    auto result = packaged_awaitable(
        run_scenario(provider, [&]{
            auto step = ++last_step;
            if (step <= 3) {
                return EAction::AddTask;
            }
            if (step <= 6) {
                return EAction::AwaitTask;
            }
            return EAction::Return;
        }));

    ASSERT_EQ(provider.awaiters(), 3u);
    provider.resume(1);
    provider.resume_with_exception(no_value_error());
    provider.resume(3);
    EXPECT_EQ(provider.awaiters(), 0u);
    ASSERT_TRUE(result.has_exception());
}

TEST(TaskGroupTest, ResumeExceptionIgnored) {
    size_t last_step = 0;
    test_channel<int> provider;

    auto result = packaged_awaitable(
        run_scenario(provider, [&]{
            auto step = ++last_step;
            if (step <= 3) {
                return EAction::AddTask;
            }
            if (step <= 6) {
                return EAction::AwaitTaskWrapped;
            }
            return EAction::Return;
        }));

    ASSERT_EQ(provider.awaiters(), 3u);
    provider.resume(1);
    provider.resume_with_exception(no_value_error());
    provider.resume(3);
    EXPECT_EQ(provider.awaiters(), 0u);
    ASSERT_TRUE(result.success());
    std::vector<int> expected{ 1, -1, 3 };
    EXPECT_EQ(*result, expected);
}

TEST(TaskGroupTest, DestroyedContinuationResumesTaskGroup) {
    size_t last_step = 0;
    test_channel<int> provider;

    auto result = packaged_awaitable(
        run_scenario(provider, [&]{
            auto step = ++last_step;
            if (step <= 3) {
                return EAction::AddTask;
            }
            if (step <= 6) {
                return EAction::AwaitTaskWrapped;
            }
            return EAction::Return;
        }));

    ASSERT_EQ(provider.awaiters(), 3u);
    provider.resume(1);
    provider.take().destroy();
    provider.resume(3);
    EXPECT_EQ(provider.awaiters(), 0u);
    ASSERT_TRUE(result.success());
    std::vector<int> expected{ 1, -2, 3 };
    EXPECT_EQ(*result, expected);
}

namespace {

actor<void> check_stop_possible() {
    co_await no_actor_context();
    stop_token token = co_await actor_context::current_stop_token;
    EXPECT_TRUE(token.stop_possible());
    EXPECT_FALSE(token.stop_requested());
}

actor<void> check_stop_requested() {
    co_await no_actor_context();
    stop_token token = co_await actor_context::current_stop_token;
    EXPECT_TRUE(token.stop_requested());
}

actor<void> check_request_stop() {
    co_await no_actor_context();

    task_group<void> group;

    group.add(check_stop_possible());
    EXPECT_TRUE(group.ready());
    co_await group.next();

    group.request_stop();

    group.add(check_stop_requested());
    EXPECT_TRUE(group.ready());
    co_await group.next();
}

} // namespace

TEST(TaskGroupTest, GroupRequestStop) {
    auto result = packaged_awaitable(
        check_request_stop());
    ASSERT_TRUE(result.success());
}
