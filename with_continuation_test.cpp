#include <coroactors/with_continuation.h>
#include <coroactors/actor.h>
#include <coroactors/detach_awaitable.h>
#include <coroactors/detail/awaiters.h>
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

template<detail::awaitable Awaitable>
class autostart_inspect {
    using Awaiter = detail::awaiter_transform_type_t<Awaitable>;

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
    co_await with_continuation(callback);
    *stage = 2;
}

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

template<class Callback>
actor<void> actor_with_continuation(const actor_context& context, int* stage, Callback callback) {
    co_await context();
    *stage = 1;
    co_await with_continuation(callback);
    *stage = 2;
}

actor<int> actor_with_result(const actor_context& context, int result) {
    co_await context();
    co_return result;
}

TEST(WithContinuationTest, ActorCompleteSync) {
    int stage = 0;
    int result = 0;

    test_scheduler scheduler;
    actor_context context(scheduler);

    // Start the first actor function
    actor_with_continuation(context, &stage, [&](continuation<> c) {
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

    continuation<> suspended;

    // Start the first actor function
    actor_with_continuation(context, &stage, [&](continuation<> c) {
        EXPECT_EQ(stage, 1);
        suspended = c;
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
    ASSERT_TRUE(suspended);

    // However the second function should have started running and completed
    EXPECT_EQ(result, 42);
    EXPECT_EQ(scheduler.queue.size(), 0);

    // Resume the continuation, it should not monopolize our thread here
    suspended.resume();

    EXPECT_EQ(stage, 1);
    ASSERT_EQ(scheduler.queue.size(), 1);

    // Activate the context
    scheduler.run_next();

    // It should now complete
    EXPECT_EQ(stage, 2);
    EXPECT_EQ(scheduler.queue.size(), 0);
}

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

TEST(WithContinuationTest, DestroyUnwind) {
    int stage = 0;
    bool finished = false;
    int refs = 0;

    continuation<> suspended;

    detach_awaitable(
        actor_wrapper(
            actor_with_continuation(no_actor_context, &stage,
                [&, guard = count_refs_guard{ &refs }](continuation<> c) {
                    EXPECT_EQ(stage, 1);
                    EXPECT_EQ(refs, 3);
                    suspended = c;
                }),
            &refs),
        [&]{
            finished = true;
        });

    EXPECT_EQ(stage, 1);
    ASSERT_TRUE(suspended);
    EXPECT_FALSE(finished);
    ASSERT_EQ(refs, 2);

    // Destroy continuation
    suspended.reset();

    // Coroutine shouldn't finish, but stack should be unwound
    EXPECT_FALSE(finished);
    ASSERT_EQ(refs, 0);
}

TEST(WithContinuationTest, DestroyFromCallback) {
    int stage = 0;
    bool finished = false;
    int refs = 0;

    detach_awaitable(
        actor_wrapper(
            actor_with_continuation(no_actor_context, &stage,
                [&, guard = count_refs_guard{ &refs }](continuation<>&& c) {
                    EXPECT_EQ(stage, 1);
                    EXPECT_EQ(refs, 3);
                    c.reset();
                }),
            &refs),
        [&]{
            finished = true;
        });

    EXPECT_EQ(stage, 1);
    EXPECT_FALSE(finished);
    EXPECT_EQ(refs, 0);
}

// A very simple class for grabbing the bottom (outer) coroutine frame
struct simple_coroutine {
    std::coroutine_handle<> handle;

    struct promise_type {
        auto initial_suspend() noexcept { return std::suspend_never{}; }
        auto final_suspend() noexcept { return std::suspend_never{}; }

        simple_coroutine get_return_object() noexcept {
            return simple_coroutine{
                std::coroutine_handle<promise_type>::from_promise(*this),
            };
        }

        void unhandled_exception() noexcept { std::terminate(); }
        void return_void() noexcept {}
    };
};

template<class Awaitable>
simple_coroutine simple_run(Awaitable awaitable) {
    co_return co_await std::move(awaitable);
}

TEST(WithContinuationTest, DestroyBottomUp) {
    int stage = 0;
    int refs = 0;

    continuation<> suspended;

    auto coro = simple_run(
        actor_wrapper(
            actor_with_continuation(no_actor_context, &stage,
                [&, guard = count_refs_guard{ &refs }](continuation<> c) {
                    EXPECT_EQ(stage, 1);
                    EXPECT_EQ(refs, 3);
                    suspended = c;
                }),
            &refs));

    EXPECT_EQ(stage, 1);
    EXPECT_EQ(refs, 2);
    ASSERT_TRUE(suspended);

    coro.handle.destroy();

    EXPECT_EQ(stage, 1);
    EXPECT_EQ(refs, 0);
}

TEST(WithContinuationTest, StopToken) {
    int stage = 0;
    int refs = 0;
    bool finished = false;

    stop_source source;
    continuation<> suspended;

    detach_awaitable(
        with_stop_token(
            actor_wrapper(
                actor_with_continuation(no_actor_context, &stage,
                    [&](continuation<> c) {
                        suspended = c;
                    }),
                &refs),
            source.get_token()),
        [&]{
            finished = true;
        });

    EXPECT_EQ(stage, 1);
    EXPECT_EQ(refs, 1);
    ASSERT_TRUE(suspended);
    EXPECT_TRUE(suspended.get_stop_token().stop_possible());
    EXPECT_FALSE(suspended.get_stop_token().stop_requested());
    source.request_stop();
    EXPECT_TRUE(suspended.get_stop_token().stop_requested());
}
