#include <coroactors/task_group.h>
#include <coroactors/actor.h>
#include <coroactors/detach_awaitable.h>
#include <deque>
#include <vector>
#include <functional>
#include <optional>
#include <mutex>
#include <thread>
#include <latch>
#include <gtest/gtest.h>

using namespace coroactors;

template<class T>
class continuation {
public:
    continuation(std::optional<T>* result, std::coroutine_handle<> handle) noexcept
        : result(result)
        , handle(handle)
    {}

    continuation(continuation&& rhs) noexcept
        : result(rhs.result)
        , handle(std::exchange(rhs.handle, {}))
    {}

    ~continuation() noexcept {
        if (handle) {
            // resume without setting a value
            handle.resume();
        }
    }

    continuation& operator=(continuation&& rhs) noexcept {
        if (this != &rhs) {
            auto prev_handle = handle;
            result = rhs.result;
            handle = std::exchange(rhs.handle, {});
            if (prev_handle) {
                // resume without setting a value
                prev_handle.resume();
            }
        }
        return *this;
    }

    void resume(T value) {
        assert(handle);
        result->emplace(std::move(value));
        std::exchange(handle, {}).resume();
    }

    void resume() {
        assert(handle);
        std::exchange(handle, {}).resume();
    }

    void destroy() {
        assert(handle);
        std::exchange(handle, {}).destroy();
    }

private:
    std::optional<T>* result;
    std::coroutine_handle<> handle;
};

template<class T>
struct value_provider {
    std::mutex lock;
    std::deque<continuation<T>> queue;
    std::deque<T> results;

    struct awaiter {
        value_provider& provider;
        std::optional<T> result;

        awaiter(value_provider& provider)
            : provider(provider)
        {}

        bool await_ready() noexcept {
            std::unique_lock l(provider.lock);
            if (!provider.results.empty()) {
                result = std::move(provider.results.front());
                provider.results.pop_front();
            }
            return bool(result);
        }

        void await_suspend(std::coroutine_handle<> c) noexcept {
            std::unique_lock l(provider.lock);
            provider.queue.emplace_back(&result, c);
        }

        T await_resume() {
            if (!result) {
                throw std::runtime_error("resumed without a value");
            }
            return std::move(*result);
        }
    };

    awaiter get() {
        return awaiter{ *this };
    }

    void provide(T value) {
        results.push_back(std::move(value));
    }

    continuation<T> take() {
        assert(!queue.empty());
        auto c = std::move(queue.front());
        queue.pop_front();
        return c;
    }

    continuation<T> take_at(size_t index) {
        assert(index <= queue.size());
        auto it = queue.begin() + index;
        auto c = std::move(*it);
        queue.erase(it);
        return c;
    }

    void resume(T value) {
        take().resume(std::move(value));
    }

    void resume_at(size_t index, T value) {
        take_at(index).resume(std::move(value));
    }
};

enum class EAction {
    AddTask,
    AwaitTask,
    AwaitTaskWrapped,
    AwaitValue,
    Return,
};

actor<std::vector<int>> run_scenario(value_provider<int>& provider, std::function<EAction()> next) {
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
                    results.push_back(std::move(result).take());
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

TEST(TaskGroupTest, SimpleAsync) {
    size_t last_step = 0;
    value_provider<int> provider;
    std::optional<std::vector<int>> result;

    detach_awaitable(
        run_scenario(provider, [&]{
            auto step = ++last_step;
            if (step <= 3) {
                return EAction::AddTask;
            }
            if (step <= 6) {
                return EAction::AwaitTask;
            }
            return EAction::Return;
        }),
        [&](auto&& value) {
            result = std::move(value);
        });

    ASSERT_EQ(provider.queue.size(), 3);
    provider.resume(1);
    provider.resume(2);
    provider.resume(3);
    EXPECT_EQ(provider.queue.size(), 0);
    ASSERT_TRUE(result);
    std::vector<int> expected{ 1, 2, 3 };
    EXPECT_EQ(*result, expected);
}

TEST(TaskGroupTest, SimpleSync) {
    size_t last_step = 0;
    value_provider<int> provider;
    std::optional<std::vector<int>> result;

    provider.provide(1);
    provider.provide(2);
    provider.provide(3);

    detach_awaitable(
        run_scenario(provider, [&]{
            auto step = ++last_step;
            if (step <= 3) {
                return EAction::AddTask;
            }
            if (step <= 6) {
                return EAction::AwaitTask;
            }
            return EAction::Return;
        }),
        [&](auto&& value) {
            result = std::move(value);
        });

    EXPECT_EQ(provider.queue.size(), 0);
    ASSERT_TRUE(result);
    std::vector<int> expected{ 1, 2, 3 };
    EXPECT_EQ(*result, expected);
}

TEST(TaskGroupTest, SimpleMultiThreaded) {
    size_t last_step = 0;
    value_provider<int> provider;
    std::optional<std::vector<int>> result;

    detach_awaitable(
        run_scenario(provider, [&]{
            auto step = ++last_step;
            if (step <= 10) {
                return EAction::AddTask;
            }
            if (step <= 20) {
                return EAction::AwaitTask;
            }
            return EAction::Return;
        }),
        [&](auto&& value) {
            result = std::move(value);
        });

    ASSERT_EQ(provider.queue.size(), 10);
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
    ASSERT_TRUE(result);
    std::sort(result->begin(), result->end());
    std::vector<int> expected;
    for (int i = 1; i <= 10; ++i) {
        expected.push_back(i);
    }
    EXPECT_EQ(*result, expected);
}

TEST(TaskGroupTest, CompleteOutOfOrder) {
    size_t last_step = 0;
    value_provider<int> provider;
    std::optional<std::vector<int>> result;

    detach_awaitable(
        run_scenario(provider, [&]{
            auto step = ++last_step;
            if (step <= 3) {
                return EAction::AddTask;
            }
            if (step <= 6) {
                return EAction::AwaitTask;
            }
            return EAction::Return;
        }),
        [&](auto&& value) {
            result = std::move(value);
        });

    ASSERT_EQ(provider.queue.size(), 3);
    provider.resume_at(1, 2);
    provider.resume(1);
    provider.resume(3);
    EXPECT_EQ(provider.queue.size(), 0);
    ASSERT_TRUE(result);
    std::vector<int> expected{ 2, 1, 3 };
    EXPECT_EQ(*result, expected);
}

TEST(TaskGroupTest, CompleteBeforeAwaited) {
    size_t last_step = 0;
    value_provider<int> provider;
    std::optional<std::vector<int>> result;

    detach_awaitable(
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
        }),
        [&](auto&& value) {
            result = std::move(value);
        });

    ASSERT_EQ(provider.queue.size(), 4);
    provider.resume(1);
    provider.resume(2);
    provider.resume(3);
    EXPECT_FALSE(result);
    ASSERT_EQ(provider.queue.size(), 1);
    provider.resume(4);
    ASSERT_TRUE(result);
    std::vector<int> expected{ 4, 1, 2, 3 };
    EXPECT_EQ(*result, expected);
}

TEST(TaskGroupTest, LocalReadyQueue) {
    size_t last_step = 0;
    value_provider<int> provider;
    std::optional<std::vector<int>> result;

    detach_awaitable(
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
        }),
        [&](auto&& value) {
            result = std::move(value);
        });

    ASSERT_EQ(provider.queue.size(), 4);
    // Arrange for two tasks to be ready
    provider.resume(1);
    provider.resume(2);
    // Unblock coroutine, it will grab value 4 and value 1 and block again
    provider.resume_at(1, 4);
    EXPECT_FALSE(result);
    ASSERT_EQ(provider.queue.size(), 2);
    // Unblock the last task, now we have both ready queue and atomic queue
    provider.resume(3);
    // Unblock coroutine again, it will grab value 5 and finally values 2 and 3
    provider.resume(5);

    ASSERT_TRUE(result);
    std::vector<int> expected{ 4, 1, 5, 2, 3 };
    EXPECT_EQ(*result, expected);
}

TEST(TaskGroupTest, DetachAll) {
    size_t last_step = 0;
    value_provider<int> provider;
    std::optional<std::vector<int>> result;

    detach_awaitable(
        run_scenario(provider, [&]{
            auto step = ++last_step;
            if (step <= 3) {
                return EAction::AddTask;
            }
            return EAction::Return;
        }),
        [&](auto&& value) {
            result = std::move(value);
        });

    ASSERT_TRUE(result);
    EXPECT_EQ(result->size(), 0);
    EXPECT_EQ(provider.queue.size(), 3);
    // Note: value_provider destructor will fail all tasks
}

TEST(TaskGroupTest, ResumeException) {
    size_t last_step = 0;
    value_provider<int> provider;
    std::optional<std::vector<int>> result;
    bool had_exception = false;

    detach_awaitable(
        run_scenario(provider, [&]{
            auto step = ++last_step;
            if (step <= 3) {
                return EAction::AddTask;
            }
            if (step <= 6) {
                return EAction::AwaitTask;
            }
            return EAction::Return;
        }).result(),
        [&](auto&& value) {
            if (value.has_exception()) {
                had_exception = true;
            } else {
                result = std::move(value).take();
            }
        });

    ASSERT_EQ(provider.queue.size(), 3);
    provider.resume(1);
    provider.take().resume();
    provider.resume(3);
    EXPECT_EQ(provider.queue.size(), 0);
    ASSERT_TRUE(had_exception);
}

TEST(TaskGroupTest, ResumeExceptionIgnored) {
    size_t last_step = 0;
    value_provider<int> provider;
    std::optional<std::vector<int>> result;

    detach_awaitable(
        run_scenario(provider, [&]{
            auto step = ++last_step;
            if (step <= 3) {
                return EAction::AddTask;
            }
            if (step <= 6) {
                return EAction::AwaitTaskWrapped;
            }
            return EAction::Return;
        }),
        [&](auto&& value) {
            result = std::move(value);
        });

    ASSERT_EQ(provider.queue.size(), 3);
    provider.resume(1);
    provider.take().resume();
    provider.resume(3);
    EXPECT_EQ(provider.queue.size(), 0);
    ASSERT_TRUE(result);
    std::vector<int> expected{ 1, -1, 3 };
    EXPECT_EQ(*result, expected);
}

TEST(TaskGroupTest, DestroyedContinuationResumesTaskGroup) {
    size_t last_step = 0;
    value_provider<int> provider;
    std::optional<std::vector<int>> result;

    detach_awaitable(
        run_scenario(provider, [&]{
            auto step = ++last_step;
            if (step <= 3) {
                return EAction::AddTask;
            }
            if (step <= 6) {
                return EAction::AwaitTaskWrapped;
            }
            return EAction::Return;
        }),
        [&](auto&& value) {
            result = std::move(value);
        });

    ASSERT_EQ(provider.queue.size(), 3);
    provider.resume(1);
    provider.take().destroy();
    provider.resume(3);
    EXPECT_EQ(provider.queue.size(), 0);
    ASSERT_TRUE(result);
    std::vector<int> expected{ 1, -2, 3 };
    EXPECT_EQ(*result, expected);
}
