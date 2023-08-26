#include <coroactors/task_group.h>
#include <coroactors/actor.h>
#include <coroactors/detach_awaitable.h>
#include <coroactors/with_task_group.h>
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
    continuation(std::optional<T>* result, std::coroutine_handle<> handle, stop_token token) noexcept
        : result(result)
        , handle(handle)
        , token(token)
    {}

    continuation(continuation&& rhs) noexcept
        : result(rhs.result)
        , handle(std::exchange(rhs.handle, {}))
        , token(std::move(rhs.token))
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
            token = std::move(rhs.token);
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

    const stop_token& get_stop_token() const {
        return token;
    }

private:
    std::optional<T>* result;
    std::coroutine_handle<> handle;
    stop_token token;
};

template<class T>
struct value_provider {
    std::mutex lock;
    std::deque<continuation<T>> queue;
    std::deque<T> results;

    struct awaiter {
        value_provider& provider;
        std::optional<T> result;
        stop_token token;

        awaiter(value_provider& provider)
            : provider(provider)
        {}

        bool await_ready(stop_token t = {}) noexcept {
            std::unique_lock l(provider.lock);
            if (!provider.results.empty()) {
                result = std::move(provider.results.front());
                provider.results.pop_front();
            } else {
                token = std::move(t);
            }
            return bool(result);
        }

        void await_suspend(std::coroutine_handle<> c) noexcept {
            std::unique_lock l(provider.lock);
            provider.queue.emplace_back(&result, c, std::move(token));
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

    ASSERT_EQ(provider.queue.size(), 3u);
    provider.resume(1);
    provider.resume(2);
    provider.resume(3);
    EXPECT_EQ(provider.queue.size(), 0u);
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

    EXPECT_EQ(provider.queue.size(), 0u);
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

    ASSERT_EQ(provider.queue.size(), 10u);
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

    ASSERT_EQ(provider.queue.size(), 3u);
    provider.resume_at(1, 2);
    provider.resume(1);
    provider.resume(3);
    EXPECT_EQ(provider.queue.size(), 0u);
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

    ASSERT_EQ(provider.queue.size(), 4u);
    provider.resume(1);
    provider.resume(2);
    provider.resume(3);
    EXPECT_FALSE(result);
    ASSERT_EQ(provider.queue.size(), 1u);
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

    ASSERT_EQ(provider.queue.size(), 4u);
    // Arrange for two tasks to be ready
    provider.resume(1);
    provider.resume(2);
    // Unblock coroutine, it will grab value 4 and value 1 and block again
    provider.resume_at(1, 4);
    EXPECT_FALSE(result);
    ASSERT_EQ(provider.queue.size(), 2u);
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
    EXPECT_EQ(result->size(), 0u);
    EXPECT_EQ(provider.queue.size(), 3u);
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
                result = std::move(value).take_value();
            }
        });

    ASSERT_EQ(provider.queue.size(), 3u);
    provider.resume(1);
    provider.take().resume();
    provider.resume(3);
    EXPECT_EQ(provider.queue.size(), 0u);
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

    ASSERT_EQ(provider.queue.size(), 3u);
    provider.resume(1);
    provider.take().resume();
    provider.resume(3);
    EXPECT_EQ(provider.queue.size(), 0u);
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

    ASSERT_EQ(provider.queue.size(), 3u);
    provider.resume(1);
    provider.take().destroy();
    provider.resume(3);
    EXPECT_EQ(provider.queue.size(), 0u);
    ASSERT_TRUE(result);
    std::vector<int> expected{ 1, -2, 3 };
    EXPECT_EQ(*result, expected);
}

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

TEST(TaskGroupTest, GroupRequestStop) {
    bool finished = false;

    detach_awaitable(
        check_request_stop(),
        [&]{
            finished = true;
        });

    ASSERT_TRUE(finished);
}

actor<void> do_with_task_group_cancel(int& stage, value_provider<int>& provider) {
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

TEST(WithTaskGroupTest, ImplicitCancel) {
    int stage = 0;
    value_provider<int> provider;
    bool finished = false;
    bool success = false;

    detach_awaitable(
        do_with_task_group_cancel(stage, provider).result(),
        [&](auto&& result) {
            finished = true;
            success = result.has_value();
        });

    EXPECT_EQ(stage, 1); // waiting on group.next()
    EXPECT_FALSE(finished);
    ASSERT_EQ(provider.queue.size(), 2u);
    auto a = provider.take();
    auto b = provider.take();
    EXPECT_FALSE(a.get_stop_token().stop_requested());
    a.resume(42);
    EXPECT_EQ(stage, 3); // returned from group.next()
    EXPECT_FALSE(finished);
    EXPECT_TRUE(b.get_stop_token().stop_requested());
    b.destroy();
    EXPECT_EQ(stage, 5); // returned from with_task_group
    EXPECT_TRUE(finished);
    EXPECT_TRUE(success);
}

TEST(WithTaskGroupTest, ImplicitCancelException) {
    int stage = 1;
    value_provider<int> provider;
    bool finished = false;
    bool success = false;

    detach_awaitable(
        do_with_task_group_cancel(stage, provider).result(),
        [&](auto&& result) {
            finished = true;
            success = result.has_value();
        });

    EXPECT_EQ(stage, 1); // waiting on group.next()
    EXPECT_FALSE(finished);
    ASSERT_EQ(provider.queue.size(), 2u);
    auto a = provider.take();
    auto b = provider.take();
    EXPECT_FALSE(a.get_stop_token().stop_requested());
    a.resume();
    EXPECT_EQ(stage, 2); // group.next() thrown an exception
    EXPECT_FALSE(finished);
    EXPECT_TRUE(b.get_stop_token().stop_requested());
    b.destroy();
    EXPECT_EQ(stage, 4); // with_task_group thrown an exception
    EXPECT_TRUE(finished);
    EXPECT_FALSE(success);
}

TEST(WithTaskGroupTest, ExplicitCancel) {
    int stage = 0;
    value_provider<int> provider;
    bool finished = false;
    bool success = false;

    stop_source source;

    detach_awaitable(
        with_stop_token(
            source.get_token(),
            do_with_task_group_cancel(stage, provider).result()),
        [&](auto&& result) {
            finished = true;
            success = result.has_value();
        });

    EXPECT_EQ(stage, 1); // waiting on group.next()
    EXPECT_FALSE(finished);
    ASSERT_EQ(provider.queue.size(), 2u);
    auto a = provider.take();
    auto b = provider.take();
    EXPECT_FALSE(a.get_stop_token().stop_requested());
    EXPECT_FALSE(b.get_stop_token().stop_requested());
    source.request_stop();
    EXPECT_TRUE(a.get_stop_token().stop_requested());
    EXPECT_TRUE(b.get_stop_token().stop_requested());
    EXPECT_EQ(stage, 1); // still waiting on group.next();
    EXPECT_FALSE(finished);
    b.resume(42);
    EXPECT_EQ(stage, 3); // returned from group.next()
    EXPECT_FALSE(finished);
    a.resume();
    EXPECT_EQ(stage, 5); // returned from with_task_group
    EXPECT_TRUE(finished);
    EXPECT_TRUE(success);
}

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

actor<void> do_with_task_group_result_type(int& stage, value_provider<int>& provider) {
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

TEST(WithTaskGroupTest, ResultType) {
    int stage = 0;
    value_provider<int> provider;
    bool finished = false;
    bool success = false;

    detach_awaitable(
        do_with_task_group_result_type(stage, provider).result(),
        [&](auto&& result) {
            finished = true;
            success = result.has_value();
        });

    EXPECT_EQ(stage, 1);
    EXPECT_FALSE(finished);
    ASSERT_EQ(provider.queue.size(), 2u);
    auto a = provider.take();
    auto b = provider.take();
    a.resume(42);
    EXPECT_EQ(stage, 2);
    EXPECT_FALSE(finished);
    b.resume(58);
    EXPECT_EQ(stage, 4);
    EXPECT_TRUE(finished);
    EXPECT_TRUE(success);
}

struct test_scheduler : public actor_scheduler {
    std::deque<std::coroutine_handle<>> queue;

    void post(std::coroutine_handle<> h) override {
        queue.push_back(h);
    }

    bool preempt() const override {
        return false;
    }

    void run_next() {
        auto h = std::move(queue.front());
        queue.pop_front();
        h.resume();
    }
};

actor<void> do_with_stop_token_context(int& stage, test_scheduler& scheduler, value_provider<int>& provider) {
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

TEST(WithTaskGroupTest, WithStopTokenContext) {
    int stage = 0;
    test_scheduler scheduler;
    value_provider<int> provider;
    stop_source source;

    bool finished = false;
    bool success = false;

    detach_awaitable(
        with_stop_token(
            source.get_token(),
            do_with_stop_token_context(stage, scheduler, provider).result()),
        [&](auto&& result) {
            finished = true;
            success = result.has_value();
        });

    EXPECT_EQ(stage, 1); // waiting for context activation
    ASSERT_EQ(scheduler.queue.size(), 1u);

    scheduler.run_next();

    EXPECT_EQ(stage, 4); // waiting for the first value
    ASSERT_EQ(provider.queue.size(), 2u);
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
    EXPECT_TRUE(finished);
    EXPECT_TRUE(success);

    EXPECT_EQ(scheduler.queue.size(), 0u);
}
