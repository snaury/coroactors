#include <coroactors/task_group.h>
#include <gtest/gtest.h>

#include "test_common.h"
#include "test_channel.h"
#include "test_scheduler.h"
#include <coroactors/async.h>
#include <coroactors/packaged_awaitable.h>

#include <algorithm>
#include <deque>
#include <functional>
#include <latch>
#include <mutex>
#include <optional>
#include <thread>
#include <type_traits>
#include <variant>
#include <vector>

using namespace coroactors;

namespace {

namespace Action {
    struct AddTask_t{} AddTask;
    struct AwaitTask_t{} AwaitTask;
    struct AwaitTaskWrapped_t{} AwaitTaskWrapped;
    struct AwaitValue_t{} AwaitValue;
    struct Return_t{} Return;

    struct AddSuspend{
        std::coroutine_handle<>& handle;

        bool await_ready() noexcept { return false; }
        void await_suspend(std::coroutine_handle<> h) noexcept { handle = h; }
        int await_resume() noexcept { std::terminate(); }
    };

    using Any = std::variant<
        AddTask_t,
        AwaitTask_t,
        AwaitTaskWrapped_t,
        AwaitValue_t,
        Return_t,
        AddSuspend>;

    template<class T, size_t I = 0>
    static constexpr size_t index_of() {
        static_assert(I < std::variant_size_v<Any>, "Cannot find the specified type");
        if constexpr (std::is_same_v<std::variant_alternative_t<I, Any>, T>) {
            return I;
        } else {
            return (index_of<T, I + 1>());
        }
    }
};

async<std::vector<int>> run_scenario(test_channel<int>& provider, std::function<Action::Any()> next) {
    std::vector<int> results;
    task_group<int> group;
    for (;;) {
        auto action = next();
        switch (action.index()) {
            case Action::index_of<Action::AddTask_t>(): {
                group.add(provider.get());
                break;
            }
            case Action::index_of<Action::AwaitTask_t>(): {
                results.push_back(co_await group.next());
                break;
            }
            case Action::index_of<Action::AwaitTaskWrapped_t>(): {
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
            case Action::index_of<Action::AwaitValue_t>(): {
                results.push_back(co_await provider.get());
                break;
            }
            case Action::index_of<Action::Return_t>(): {
                co_return results;
            }
            case Action::index_of<Action::AddSuspend>(): {
                group.add(std::get<Action::AddSuspend>(action));
                break;
            }
            default: {
                ADD_FAILURE() << "Unsupported action";
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
        run_scenario(provider, [&]() -> Action::Any {
            auto step = ++last_step;
            if (step <= 3) {
                return Action::AddTask;
            }
            if (step <= 6) {
                return Action::AwaitTask;
            }
            return Action::Return;
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
        run_scenario(provider, [&]() -> Action::Any {
            auto step = ++last_step;
            if (step <= 3) {
                return Action::AddTask;
            }
            if (step <= 6) {
                return Action::AwaitTask;
            }
            return Action::Return;
        }));

    EXPECT_EQ(provider.awaiters(), 0u);
    ASSERT_TRUE(result.success());
    std::vector<int> expected{ 1, 2, 3 };
    EXPECT_EQ(*result, expected);
}

TEST(TaskGroupTest, SimpleMultiThreaded) {
    size_t last_step = 0;
    test_channel<int> provider;
    const int thread_count = 10;

    auto result = packaged_awaitable(
        run_scenario(provider, [&]() -> Action::Any {
            auto step = ++last_step;
            if (step <= 1 * thread_count) {
                return Action::AddTask;
            }
            if (step <= 2 * thread_count) {
                return Action::AwaitTask;
            }
            return Action::Return;
        }));

    ASSERT_EQ(provider.awaiters(), thread_count);
    std::latch barrier(thread_count);
    std::vector<std::thread> threads;
    for (int i = 1; i <= thread_count; ++i) {
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
    for (int i = 1; i <= thread_count; ++i) {
        expected.push_back(i);
    }
    EXPECT_EQ(*result, expected);
}

TEST(TaskGroupTest, CompleteOutOfOrder) {
    size_t last_step = 0;
    test_channel<int> provider;

    auto result = packaged_awaitable(
        run_scenario(provider, [&]() -> Action::Any {
            auto step = ++last_step;
            if (step <= 3) {
                return Action::AddTask;
            }
            if (step <= 6) {
                return Action::AwaitTask;
            }
            return Action::Return;
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
        run_scenario(provider, [&]() -> Action::Any {
            auto step = ++last_step;
            if (step <= 3) {
                return Action::AddTask;
            }
            if (step == 4) {
                return Action::AwaitValue;
            }
            if (step <= 7) {
                return Action::AwaitTask;
            }
            return Action::Return;
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
        run_scenario(provider, [&]() -> Action::Any {
            auto step = ++last_step;
            if (step <= 3) {
                return Action::AddTask;
            }
            if (step == 4) {
                return Action::AwaitValue;
            }
            if (step == 5) {
                return Action::AwaitTask;
            }
            if (step == 6) {
                return Action::AwaitValue;
            }
            if (step <= 8) {
                return Action::AwaitTask;
            }
            return Action::Return;
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
        run_scenario(provider, [&]() -> Action::Any {
            auto step = ++last_step;
            if (step <= 3) {
                return Action::AddTask;
            }
            return Action::Return;
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
        run_scenario(provider, [&]() -> Action::Any {
            auto step = ++last_step;
            if (step <= 3) {
                return Action::AddTask;
            }
            if (step <= 6) {
                return Action::AwaitTask;
            }
            return Action::Return;
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
        run_scenario(provider, [&]() -> Action::Any {
            auto step = ++last_step;
            if (step <= 3) {
                return Action::AddTask;
            }
            if (step <= 6) {
                return Action::AwaitTaskWrapped;
            }
            return Action::Return;
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

namespace {

async<void> check_stop_possible() {
    stop_token token = current_stop_token();
    EXPECT_TRUE(token.stop_possible());
    EXPECT_FALSE(token.stop_requested());
    co_return;
}

async<void> check_stop_requested() {
    stop_token token = current_stop_token();
    EXPECT_TRUE(token.stop_requested());
    co_return;
}

async<void> check_request_stop() {
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
