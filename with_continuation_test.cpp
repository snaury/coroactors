#include "with_continuation.h"
#include "actor.h"
#include "detach_awaitable.h"
#include "detail/awaiters.h"
#include <exception>
#include <functional>
#include <gtest/gtest.h>
#include <type_traits>
#include <deque>

using namespace coroactors;

static thread_local int await_ready_count{ 0 };
static thread_local int await_suspend_count{ 0 };
static thread_local int await_resume_count{ 0 };
static thread_local std::function<void()>* await_suspend_hook{ nullptr };

void clear_await_count() {
    await_ready_count = 0;
    await_suspend_count = 0;
    await_resume_count = 0;
}

struct with_suspend_hook {
    std::function<void()> hook;

    with_suspend_hook(std::function<void()> hook)
        : hook(std::move(hook))
    {
        if (this->hook) {
            await_suspend_hook = &this->hook;
        }
    }

    ~with_suspend_hook() {
        if (this->hook) {
            await_suspend_hook = nullptr;
        }
    }

    void remove() {
        if (this->hook) {
            await_suspend_hook = nullptr;
            this->hook = {};
        }
    }
};

template<detail::awaiter TAwaiter>
struct autostart_inspect {
    TAwaiter awaiter;

    bool await_ready() {
        ++await_ready_count;
        return awaiter.await_ready();
    }

    template<class TArg>
    decltype(auto) await_suspend(TArg&& arg) {
        ++await_suspend_count;
        if (await_suspend_hook) {
            (*await_suspend_hook)();
        }
        return awaiter.await_suspend(std::forward<TArg>(arg));
    }

    auto await_resume() {
        ++await_resume_count;
        return awaiter.await_resume();
    }
};

struct autostart_promise {
    std::coroutine_handle<autostart_promise> get_return_object() noexcept {
        return std::coroutine_handle<autostart_promise>::from_promise(*this);
    }

    auto initial_suspend() noexcept { return std::suspend_never{}; }
    auto final_suspend() noexcept { return std::suspend_never{}; }

    void unhandled_exception() noexcept { std::terminate(); }
    void return_void() noexcept {}

    template<class TAwaitable>
    auto await_transform(TAwaitable&& awaitable) {
        using TAwaiter = std::remove_reference_t<decltype(detail::get_awaiter((TAwaitable&&)awaitable))>;
        return autostart_inspect<TAwaiter>{ detail::get_awaiter((TAwaitable&&)awaitable) };
    }
};

struct autostart {
    using promise_type = autostart_promise;

    autostart(std::coroutine_handle<autostart_promise>) {}
};

template<class TCallback>
autostart run_with_continuation(int* stage, TCallback callback) {
    *stage = 1;
    co_await with_continuation(callback);
    *stage = 2;
}

TEST(WithContinuationTest, CompleteSync) {
    clear_await_count();

    int stage = 0;

    run_with_continuation(&stage, [&](std::coroutine_handle<> c) {
        // We must be running in await_ready
        EXPECT_EQ(stage, 1);
        EXPECT_EQ(await_ready_count, 1);
        EXPECT_EQ(await_suspend_count, 0);
        // Resume directly in the callback
        c.resume();
        // Should not resume until we return from callback
        EXPECT_EQ(stage, 1);
        EXPECT_EQ(await_resume_count, 0);
    });

    // Should complete before returning
    EXPECT_EQ(stage, 2);

    // Coroutine should not have suspended
    EXPECT_EQ(await_ready_count, 1);
    EXPECT_EQ(await_suspend_count, 0);
    EXPECT_EQ(await_resume_count, 1);
}

TEST(WithContinuationTest, CompleteAsync) {
    clear_await_count();

    int stage = 0;
    std::coroutine_handle<> continuation;

    run_with_continuation(&stage, [&](std::coroutine_handle<> c) {
        // We must be running in await_ready
        EXPECT_EQ(stage, 1);
        EXPECT_EQ(await_ready_count, 1);
        EXPECT_EQ(await_suspend_count, 0);
        // Store continuation for later
        continuation = c;
    });

    // Must be suspended with continuation
    EXPECT_EQ(stage, 1);
    EXPECT_EQ(await_suspend_count, 1);
    EXPECT_EQ(await_resume_count, 0);
    ASSERT_TRUE(continuation);

    // Resume
    if (continuation) {
        continuation.resume();
    }

    // Should complete during resume
    EXPECT_EQ(stage, 2);
    EXPECT_EQ(await_ready_count, 1);
    EXPECT_EQ(await_suspend_count, 1);
    EXPECT_EQ(await_resume_count, 1);
}

TEST(WithContinuationTest, CompleteRace) {
    clear_await_count();

    int stage = 0;
    std::coroutine_handle<> continuation;

    with_suspend_hook hook([&]{
        EXPECT_EQ(await_suspend_count, 1);
        EXPECT_TRUE(continuation);

        if (continuation) {
            std::exchange(continuation, {}).resume();
            // Should not resume until suspend finishes
            EXPECT_EQ(await_resume_count, 0);
        }
    });

    run_with_continuation(&stage, [&](std::coroutine_handle<> c) {
        // We must be running in await_ready
        EXPECT_EQ(stage, 1);
        EXPECT_EQ(await_ready_count, 1);
        EXPECT_EQ(await_suspend_count, 0);
        // Store continuation for later
        continuation = c;
    });

    // Must resume and complete before returning
    EXPECT_EQ(stage, 2);
    EXPECT_EQ(await_ready_count, 1);
    EXPECT_EQ(await_suspend_count, 1);
    EXPECT_EQ(await_resume_count, 1);
}

struct test_scheduler : public actor_scheduler {
    std::deque<std::coroutine_handle<>> queue;

    void schedule(std::coroutine_handle<> h) override {
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

template<class TCallback>
actor<void> actor_with_continuation(const actor_context& context, int* stage, TCallback callback) {
    co_await context;
    *stage = 1;
    co_await with_continuation(callback);
    *stage = 2;
}

actor<int> actor_with_result(const actor_context& context, int result) {
    co_await context;
    co_return result;
}

TEST(WithContinuationTest, ActorCompleteSync) {
    int stage = 0;
    int result = 0;

    test_scheduler scheduler;
    actor_context context(scheduler);

    // Start the first actor function
    actor_with_continuation(context, &stage, [&](std::coroutine_handle<> c) {
        EXPECT_EQ(stage, 1);
        c.resume();
        EXPECT_EQ(stage, 1);
    }).detach();

    // It should initially block on context activation
    EXPECT_EQ(stage, 0);
    EXPECT_EQ(scheduler.queue.size(), 1);

    // Start another actor function
    detach_awaitable(
        actor_with_result(context, 42),
        [&](int value) {
            result = value;
        });

    // It will be enqueued after the first one
    EXPECT_EQ(result, 0);
    ASSERT_EQ(scheduler.queue.size(), 1);

    // Activate the context
    scheduler.run_next();

    // The first function should complete and transfer to the second one
    EXPECT_EQ(stage, 2);
    EXPECT_EQ(result, 42);
    EXPECT_EQ(scheduler.queue.size(), 0);
}

TEST(WithContinuationTest, ActorCompleteAsync) {
    int stage = 0;
    int result = 0;

    test_scheduler scheduler;
    actor_context context(scheduler);

    std::coroutine_handle<> continuation;

    // Start the first actor function
    actor_with_continuation(context, &stage, [&](std::coroutine_handle<> c) {
        EXPECT_EQ(stage, 1);
        continuation = c;
    }).detach();

    // It should initially block on context activation
    EXPECT_EQ(stage, 0);
    EXPECT_EQ(scheduler.queue.size(), 1);

    // Start another actor function
    detach_awaitable(
        actor_with_result(context, 42),
        [&](int value) {
            result = value;
        });

    // It will be enqueued after the first one
    EXPECT_EQ(result, 0);
    ASSERT_EQ(scheduler.queue.size(), 1);

    // Activate the context
    scheduler.run_next();

    // The first function should be suspended now
    EXPECT_EQ(stage, 1);
    ASSERT_TRUE(continuation);

    // However the second function should have started running and completed
    EXPECT_EQ(result, 42);
    EXPECT_EQ(scheduler.queue.size(), 0);

    // Resume the continuation, actor should resume and complete
    continuation.resume();

    EXPECT_EQ(stage, 2);
    EXPECT_EQ(scheduler.queue.size(), 0);
}

struct set_destroyed_guard {
    bool* destroyed;

    ~set_destroyed_guard() {
        *destroyed = true;
    }
};

actor<void> actor_wrapper(actor<void> nested, bool* destroyed) {
    co_await no_actor_context;
    set_destroyed_guard guard{ destroyed };
    co_await std::move(nested);
}

TEST(WithContinuationTest, DestroyUnwind) {
    int stage = 0;
    bool finished = false;
    bool destroyed = false;

    std::coroutine_handle<> continuation;

    detach_awaitable(
        actor_wrapper(
            actor_with_continuation(no_actor_context, &stage, [&](std::coroutine_handle<> c){
                EXPECT_EQ(stage, 1);
                continuation = c;
            }),
            &destroyed),
        [&]{
            finished = true;
        });

    EXPECT_EQ(stage, 1);
    ASSERT_TRUE(continuation);
    EXPECT_FALSE(finished);
    ASSERT_FALSE(destroyed);

    // Destroy continuation, only safe when done non-concurrently
    continuation.destroy();

    // Coroutine shouldn't finish, but stack should be unwound
    EXPECT_FALSE(finished);
    ASSERT_TRUE(destroyed);
}
