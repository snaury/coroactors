#include "with_continuation.h"
#include "detail/get_awaiter.h"
#include <exception>
#include <functional>
#include <gtest/gtest.h>
#include <type_traits>

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

template<class TAwaiter>
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
        using TAwaiter = std::remove_reference_t<decltype(coroactors::detail::get_awaiter((TAwaitable&&)awaitable))>;
        return autostart_inspect<TAwaiter>{ coroactors::detail::get_awaiter((TAwaitable&&)awaitable) };
    }
};

struct autostart {
    using promise_type = autostart_promise;

    autostart(std::coroutine_handle<autostart_promise>) {}
};

autostart run_with_continuation(int* stage, std::function<void(std::coroutine_handle<>)> callback) {
    *stage = 1;
    co_await coroactors::with_continuation(callback);
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
    EXPECT_TRUE(continuation);

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
