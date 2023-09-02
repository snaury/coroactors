#include <coroactors/with_continuation.h>
#include <gtest/gtest.h>

#include "test_scheduler.h"
#include <coroactors/actor.h>
#include <coroactors/detail/awaiters.h>
#include <coroactors/packaged_awaitable.h>

#include <exception>
#include <functional>
#include <type_traits>

using namespace coroactors;

namespace {

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

template<detail::awaitable Awaitable>
class autostart_inspect {
    using Awaiter = detail::awaiter_type_t<Awaitable>;

public:
    autostart_inspect(Awaitable&& awaitable)
        : awaiter(detail::get_awaiter(std::forward<Awaitable>(awaitable)))
    {}

    bool await_ready() {
        ++await_ready_count;
        return awaiter.await_ready();
    }

    template<class Promise>
    __attribute__((__noinline__))
    decltype(auto) await_suspend(std::coroutine_handle<Promise> c) {
        ++await_suspend_count;
        if (await_suspend_hook) {
            (*await_suspend_hook)();
        }
        return awaiter.await_suspend(c);
    }

    decltype(auto) await_resume() {
        ++await_resume_count;
        return awaiter.await_resume();
    }

private:
    Awaiter awaiter;
};

struct autostart_promise {
    std::coroutine_handle<autostart_promise> get_return_object() noexcept {
        return std::coroutine_handle<autostart_promise>::from_promise(*this);
    }

    auto initial_suspend() noexcept { return std::suspend_never{}; }
    auto final_suspend() noexcept { return std::suspend_never{}; }

    void unhandled_exception() noexcept { std::terminate(); }
    void return_void() noexcept {}

    template<detail::awaitable Awaitable>
    auto await_transform(Awaitable&& awaitable) {
        return autostart_inspect<Awaitable>(std::forward<Awaitable>(awaitable));
    }
};

struct autostart {
    using promise_type = autostart_promise;

    autostart(std::coroutine_handle<autostart_promise>) {}
};

template<class Callback>
autostart run_with_continuation(int* stage, Callback callback) {
    *stage = 1;
    co_await with_continuation(std::move(callback));
    *stage = 2;
}

} // namespace

TEST(WithContinuationTest, CompleteSync) {
    clear_await_count();

    int stage = 0;

    run_with_continuation(&stage, [&](continuation<> c) {
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
    continuation<> suspended;

    run_with_continuation(&stage, [&](continuation<> c) {
        // We must be running in await_ready
        EXPECT_EQ(stage, 1);
        EXPECT_EQ(await_ready_count, 1);
        EXPECT_EQ(await_suspend_count, 0);
        // Store continuation for later
        suspended = c;
    });

    // Must be suspended with continuation
    EXPECT_EQ(stage, 1);
    EXPECT_EQ(await_suspend_count, 1);
    EXPECT_EQ(await_resume_count, 0);
    ASSERT_TRUE(suspended);

    // Resume
    suspended.resume();

    // Should complete during resume
    EXPECT_EQ(stage, 2);
    EXPECT_EQ(await_ready_count, 1);
    EXPECT_EQ(await_suspend_count, 1);
    EXPECT_EQ(await_resume_count, 1);
}

TEST(WithContinuationTest, CompleteRaceWithState) {
    clear_await_count();

    int stage = 0;
    continuation<> suspended;

    with_suspend_hook hook([&]{
        EXPECT_EQ(await_suspend_count, 1);
        EXPECT_TRUE(suspended);

        if (suspended) {
            // Resume while keeping the state alive
            // Awaiter won't be able to set continuation and will resume
            suspended.resume();
            // Should not resume until suspend finishes
            EXPECT_EQ(await_resume_count, 0);
        }
    });

    run_with_continuation(&stage, [&](continuation<> c) {
        // We must be running in await_ready
        EXPECT_EQ(stage, 1);
        EXPECT_EQ(await_ready_count, 1);
        EXPECT_EQ(await_suspend_count, 0);
        // Store continuation for later
        suspended = c;
    });

    // Must resume and complete before returning
    EXPECT_EQ(stage, 2);
    EXPECT_EQ(await_ready_count, 1);
    EXPECT_EQ(await_suspend_count, 1);
    EXPECT_EQ(await_resume_count, 1);
}

TEST(WithContinuationTest, CompleteRaceWithoutState) {
    clear_await_count();

    int stage = 0;
    continuation<> suspended;

    with_suspend_hook hook([&]{
        EXPECT_EQ(await_suspend_count, 1);
        EXPECT_TRUE(suspended);

        if (suspended) {
            // State will be destroyed with continuation
            std::exchange(suspended, {}).resume();
            // Should not resume until suspend finishes
            EXPECT_EQ(await_resume_count, 0);
        }
    });

    run_with_continuation(&stage, [&](continuation<> c) {
        // We must be running in await_ready
        EXPECT_EQ(stage, 1);
        EXPECT_EQ(await_ready_count, 1);
        EXPECT_EQ(await_suspend_count, 0);
        // Store continuation for later
        suspended = c;
    });

    // Must resume and complete before returning
    EXPECT_EQ(stage, 2);
    EXPECT_EQ(await_ready_count, 1);
    EXPECT_EQ(await_suspend_count, 1);
    EXPECT_EQ(await_resume_count, 1);
}

namespace {

template<class Callback>
autostart run_with_continuation_int(int* stage, Callback callback) {
    *stage = 1;
    try {
        int value = co_await with_continuation<int>(callback);
        EXPECT_EQ(value, 42);
        *stage = 2;
    } catch (const std::exception&) {
        *stage = 3;
    }
}

} // namespace

TEST(WithContinuationTest, CompleteWithValue) {
    clear_await_count();

    int stage = 0;

    run_with_continuation_int(&stage, [&](continuation<int> c) {
        EXPECT_EQ(stage, 1);
        c.resume(42);
        EXPECT_EQ(stage, 1);
    });

    // Should complete before returning
    EXPECT_EQ(stage, 2);

    // Coroutine should not have suspended
    EXPECT_EQ(await_ready_count, 1);
    EXPECT_EQ(await_suspend_count, 0);
    EXPECT_EQ(await_resume_count, 1);
}

TEST(WithContinuationTest, CompleteWithException) {
    clear_await_count();

    int stage = 0;

    run_with_continuation_int(&stage, [&](continuation<int> c) {
        EXPECT_EQ(stage, 1);
        c.resume_with_exception(std::make_exception_ptr(std::exception()));
        EXPECT_EQ(stage, 1);
    });

    // Should complete with exception
    EXPECT_EQ(stage, 3);

    // Coroutine should not have suspended
    EXPECT_EQ(await_ready_count, 1);
    EXPECT_EQ(await_suspend_count, 0);
    EXPECT_EQ(await_resume_count, 1);
}

namespace {

template<class Callback>
actor<void> actor_with_continuation(const actor_context& context, int& stage, Callback callback) {
    stage = 1;
    co_await context();
    stage = 2;
    co_await with_continuation(std::move(callback));
    stage = 3;
}

actor<int> actor_with_result(const actor_context& context, int& stage, int result) {
    co_await context();
    stage = 1;
    co_return result;
}

} // namespace

TEST(WithContinuationTest, ActorCompleteSync) {
    int stage = 0;
    int rstage = 0;

    test_scheduler scheduler;
    actor_context context(scheduler);

    // Start the first actor function
    actor_with_continuation(context, stage, [&](continuation<> c) {
        EXPECT_EQ(stage, 2);
        c.resume();
        EXPECT_EQ(stage, 2);
    }).detach();

    // It should initially block at context wait
    EXPECT_EQ(stage, 1);
    EXPECT_EQ(scheduler.queue.size(), 1u);

    // Start another actor function
    auto result = packaged_awaitable(
        actor_with_result(context, rstage, 42));

    // It will be enqueued after the first one (in the mailbox)
    EXPECT_EQ(rstage, 0);
    EXPECT_TRUE(result.running());
    ASSERT_EQ(scheduler.queue.size(), 1u);

    // Activate the context
    scheduler.run_next();

    // The first function should complete and transfer to the second one
    EXPECT_EQ(stage, 3);
    EXPECT_EQ(rstage, 1);
    EXPECT_EQ(*result, 42);
    EXPECT_EQ(scheduler.queue.size(), 0u);
}

TEST(WithContinuationTest, ActorCompleteAsync) {
    int stage = 0;
    int rstage = 0;

    test_scheduler scheduler;
    actor_context context(scheduler);

    continuation<> suspended;

    // Start the first actor function
    actor_with_continuation(context, stage, [&](continuation<> c) {
        EXPECT_EQ(stage, 2);
        suspended = c;
    }).detach();

    // It should initially block at context wait
    EXPECT_EQ(stage, 1);
    EXPECT_EQ(scheduler.queue.size(), 1u);

    // Start another actor function
    auto result = packaged_awaitable(
        actor_with_result(context, rstage, 42));

    // It will be enqueued after the first one (in the mailbox)
    EXPECT_EQ(rstage, 0);
    EXPECT_TRUE(result.running());
    ASSERT_EQ(scheduler.queue.size(), 1u);

    // Activate the context
    scheduler.run_next();

    // The first function should be suspended now
    EXPECT_EQ(stage, 2);
    ASSERT_TRUE(suspended);

    // However the second function should have started running and completed
    EXPECT_EQ(rstage, 1);
    EXPECT_EQ(*result, 42);
    EXPECT_EQ(scheduler.queue.size(), 0u);

    // Resume the continuation, it should not monopolize our thread here
    suspended.resume();

    EXPECT_EQ(stage, 2);
    ASSERT_EQ(scheduler.queue.size(), 1u);

    // Activate the context
    scheduler.run_next();

    // It should now complete
    EXPECT_EQ(stage, 3);
    EXPECT_EQ(scheduler.queue.size(), 0u);
}

namespace {

class count_refs_guard {
public:
    explicit count_refs_guard(int* refs)
        : refs(refs)
    {
        ++*refs;
    }

    count_refs_guard(const count_refs_guard& rhs)
        : refs(rhs.refs)
    {
        ++*refs;
    }

    ~count_refs_guard() {
        --*refs;
    }

private:
    int* refs;
};

actor<void> actor_wrapper(actor<void> nested, int* refs) {
    co_await no_actor_context();
    count_refs_guard guard{ refs };
    co_await std::move(nested);
}

} // namespace

TEST(WithContinuationTest, DestroyAfterSuspend) {
    int stage = 0;
    int refs = 0;

    continuation<> suspended;

    auto result = packaged_awaitable(
        actor_wrapper(
            actor_with_continuation(no_actor_context, stage,
                [&, guard = count_refs_guard{ &refs }](continuation<> c) {
                    EXPECT_EQ(stage, 2);
                    // Expected refs:
                    // - in actor_wrapper (local variable)
                    // - in actor_with_continuation (lambda argument)
                    // - in with_continuation_awaiter (callback member)
                    // - the original lambda temporary is not destroyed yet!
                    EXPECT_EQ(refs, 4);
                    suspended = c;
                }),
            &refs));

    EXPECT_EQ(stage, 2);
    ASSERT_TRUE(suspended);
    EXPECT_TRUE(result.running());
    // One less expected ref (lambda temporary destroyed)
    ASSERT_EQ(refs, 3);

    // Destroy continuation
    suspended.reset();

    // Coroutine should finish with an exception
    EXPECT_FALSE(result.running());
    EXPECT_TRUE(result);
    EXPECT_THROW(*result, with_continuation_error);
    ASSERT_EQ(refs, 0);
}

TEST(WithContinuationTest, DestroyFromCallback) {
    int stage = 0;
    int refs = 0;

    auto result = packaged_awaitable(
        actor_wrapper(
            actor_with_continuation(no_actor_context, stage,
                [&, guard = count_refs_guard{ &refs }](continuation<> c) {
                    EXPECT_EQ(stage, 2);
                    EXPECT_EQ(refs, 4);
                    c.reset();
                }),
            &refs));

    // Coroutine should finish with an exception
    EXPECT_EQ(stage, 2);
    EXPECT_FALSE(result.running());
    EXPECT_TRUE(result);
    EXPECT_THROW(*result, with_continuation_error);
    EXPECT_EQ(refs, 0);
}

TEST(WithContinuationTest, DestroyBottomUp) {
    int stage = 0;
    int refs = 0;

    continuation<> suspended;

    auto result = packaged_awaitable(
        actor_wrapper(
            actor_with_continuation(no_actor_context, stage,
                [&, guard = count_refs_guard{ &refs }](continuation<> c) {
                    EXPECT_EQ(stage, 2);
                    EXPECT_EQ(refs, 4);
                    suspended = c;
                }),
            &refs));

    EXPECT_EQ(stage, 2);
    EXPECT_EQ(refs, 3);
    ASSERT_TRUE(suspended);

    result.destroy();

    EXPECT_EQ(stage, 2);
    EXPECT_EQ(refs, 0);

    // Resuming a destroyed continuation is an error
    EXPECT_THROW(suspended.resume(), with_continuation_error);
}

TEST(WithContinuationTest, StopToken) {
    int stage = 0;
    int refs = 0;

    stop_source source;
    continuation<> suspended;

    auto result = packaged_awaitable(
        with_stop_token(
            source.get_token(),
            actor_wrapper(
                actor_with_continuation(no_actor_context, stage,
                    [&](continuation<> c) {
                        suspended = c;
                    }),
                &refs)));

    EXPECT_EQ(stage, 2);
    // Expect only one ref in actor_wrapper
    EXPECT_EQ(refs, 1);
    ASSERT_TRUE(suspended);
    EXPECT_TRUE(suspended.get_stop_token().stop_possible());
    EXPECT_FALSE(suspended.get_stop_token().stop_requested());
    source.request_stop();
    EXPECT_TRUE(suspended.get_stop_token().stop_requested());
    EXPECT_TRUE(result.running());
    suspended.resume();
    EXPECT_TRUE(result.success());
    EXPECT_EQ(refs, 0);
}
