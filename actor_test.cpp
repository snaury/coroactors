#include <coroactors/actor.h>
#include <coroactors/detach_awaitable.h>
#include <coroactors/with_continuation.h>
#include <gtest/gtest.h>
#include <deque>
#include <map>

using namespace coroactors;

struct test_scheduler : public actor_scheduler {
    using clock_type = actor_scheduler::clock_type;
    using time_point = actor_scheduler::time_point;
    using schedule_callback_t = actor_scheduler::schedule_callback_type;

    struct continuation_t {
        std::coroutine_handle<> h;
        actor_context c;
        bool deferred;

        void resume() {
            c.manager().resume(h);
        }
    };

    struct timer_t {
        schedule_callback_t callback;
        std::optional<stop_callback<std::function<void()>>> stop;
        bool cancelled = false;

        explicit timer_t(schedule_callback_t&& callback)
            : callback(std::move(callback))
        {}
    };

    bool preempt() const override {
        return false;
    }

    void post(std::coroutine_handle<> h, actor_context&& c) override {
        queue.push_back(continuation_t{ h, std::move(c), false });
    }

    void defer(std::coroutine_handle<> h, actor_context&& c) override {
        queue.push_back(continuation_t{ h, std::move(c), true });
    }

    void schedule(schedule_callback_t c, time_point d, stop_token t) override {
        if (!timers_enabled) {
            c(false);
            return;
        }
        auto it = timers.emplace(
            std::piecewise_construct,
            std::forward_as_tuple(d),
            std::forward_as_tuple(std::move(c)));
        if (t.stop_possible()) {
            locked = true;
            it->second.stop.emplace(std::move(t), [this, it]{
                it->second.cancelled = true;
                if (locked) {
                    return; // triggered during emplace
                }
                // The call is synchronized with destructor (i.e. removal)
                it->second.callback(false);
                // We destroy ourselves here, copy iterator first
                auto copy = it;
                timers.erase(copy);
            });
            locked = false;
            if (it->second.cancelled) {
                it->second.stop.reset();
                it->second.callback(false);
                timers.erase(it);
            }
        }
    }

    void run_next() {
        assert(!queue.empty());
        auto cont = queue.front();
        queue.pop_front();
        cont.resume();
    }

    void wake_next() {
        assert(!timers.empty());
        auto it = timers.begin();
        it->second.stop.reset();
        assert(!it->second.cancelled);
        it->second.callback(true);
        timers.erase(it);
    }

    std::deque<continuation_t> queue;
    std::multimap<actor_scheduler::time_point, timer_t> timers;
    bool timers_enabled = false;
    bool locked = false;
};

template<class T>
struct run_result {
    struct data_t {
        std::optional<T> value;
        bool finished = false;
    };

    struct callback_t {
        std::shared_ptr<data_t> data;

        explicit callback_t(const std::shared_ptr<data_t>& data)
            : data(data)
        {}

        callback_t(callback_t&&) = default;
        callback_t(const callback_t&) = delete;
        callback_t& operator=(const callback_t&) = delete;

        ~callback_t() {
            if (data) {
                data->finished = true;
            }
        }

        void operator()(T&& value) {
            data->value.emplace(std::move(value));
        }
    };

    callback_t callback() {
        return callback_t(data);
    }

    bool finished() const { return data->finished; }

    explicit operator bool() const { return bool(data->value); }

    T* operator->() const { return &*data->value; }
    T& operator*() const { return *data->value; }

    std::shared_ptr<data_t> data = std::make_shared<data_t>();
};

template<class Awaitable>
run_result<std::decay_t<detail::await_result_t<Awaitable>>>
run(Awaitable&& awaitable) {
    run_result<std::decay_t<detail::await_result_t<Awaitable>>> result;
    detach_awaitable(std::forward<Awaitable>(awaitable), result.callback());
    return result;
}

actor<int> actor_return_const(int value) {
    co_return value;
}

TEST(ActorTest, ImmediateReturn) {
    auto result = run(actor_return_const(42));
    ASSERT_TRUE(result.finished());
    ASSERT_TRUE(result);
    ASSERT_EQ(*result, 42);
}

actor<void> actor_await_const_without_context(int value) {
    int result = co_await actor_return_const(value);
    (void)result;
    ADD_FAILURE() << "unexpected success";
}

actor<void> actor_await_caller_context_without_context() {
    const actor_context& context = co_await actor_context::caller_context;
    (void)context;
    ADD_FAILURE() << "unexpected success";
}

actor<void> actor_await_current_context_without_context() {
    const actor_context& context = co_await actor_context::current_context;
    (void)context;
    ADD_FAILURE() << "unexpected success";
}

actor<void> actor_await_sleep_without_context() {
    co_await no_actor_context.sleep_for(std::chrono::milliseconds(100));
    ADD_FAILURE() << "unexpected success";
}

TEST(ActorTest, CannotAwaitWithoutContext) {
    auto a = run(actor_await_const_without_context(42).result());
    EXPECT_TRUE(a && a->has_exception());
    auto b = run(actor_await_caller_context_without_context().result());
    EXPECT_TRUE(b && b->has_exception());
    auto c = run(actor_await_current_context_without_context().result());
    EXPECT_TRUE(c && c->has_exception());
    auto d = run(actor_await_sleep_without_context().result());
    EXPECT_TRUE(d && d->has_exception());
}

actor<void> actor_empty_context() {
    co_await no_actor_context();
    EXPECT_EQ(co_await actor_context::current_context, no_actor_context);
}

actor<void> actor_empty_caller_context() {
    co_await actor_context::caller_context();
    EXPECT_EQ(co_await actor_context::current_context, no_actor_context);
}

TEST(ActorTest, StartWithEmptyContext) {
    auto a = run(actor_empty_context().result());
    EXPECT_TRUE(a && a->has_value());
    auto b = run(actor_empty_caller_context().result());
    EXPECT_TRUE(b && b->has_value());
}

actor<void> actor_with_specific_context(const actor_context& context) {
    co_await context();
    actor_context current = co_await actor_context::current_context;
    EXPECT_EQ(current, context);
    EXPECT_NE(current, no_actor_context);
    actor_context caller = co_await actor_context::caller_context;
    EXPECT_EQ(caller, no_actor_context);
}

TEST(ActorTest, StartWithSpecificContext) {
    test_scheduler scheduler;
    actor_context context(scheduler);
    // Not nested, so runs in the same thread
    auto r = run(actor_with_specific_context(context).result());
    EXPECT_TRUE(r && r->has_value());
}

TEST(ActorTest, DetachWithSpecificContext) {
    test_scheduler scheduler;
    actor_context context(scheduler);
    // Not nested, so runs in the same thread
    actor_with_specific_context(context).detach();
    ASSERT_EQ(scheduler.queue.size(), 0u);
}

actor<void> actor_without_context_awaits_specific_context(const actor_context& context) {
    co_await no_actor_context();
    EXPECT_EQ(co_await actor_context::current_context, no_actor_context);
    co_await actor_with_specific_context(context);
    EXPECT_EQ(co_await actor_context::current_context, no_actor_context);
}

TEST(ActorTest, AwaitWithSpecificContext) {
    test_scheduler scheduler;
    actor_context context(scheduler);
    // The same thread of execution, no difference to run/detach
    auto r = run(actor_without_context_awaits_specific_context(context).result());
    EXPECT_TRUE(r && r->has_value());
}

actor<void> actor_with_context_awaits_empty_context(int& stage, const actor_context& context) {
    stage = 1;
    co_await context();
    stage = 2;
    EXPECT_EQ(co_await actor_context::current_context, context);
    stage = 3;
    co_await actor_empty_context();
    stage = 4;
}

TEST(ActorTest, AwaitEmptyFromSpecificContext) {
    test_scheduler scheduler;
    actor_context context(scheduler);
    int stage = 0;
    auto r = run(actor_with_context_awaits_empty_context(stage, context).result());
    EXPECT_EQ(stage, 3); // we should defer on the return path
    ASSERT_EQ(scheduler.queue.size(), 1u);
    EXPECT_EQ(scheduler.queue[0].deferred, true);
    scheduler.run_next();
    EXPECT_EQ(stage, 4);
    EXPECT_TRUE(r && r->has_value());
}

actor<void> actor_without_context_runs_specific_context(const actor_context& context,
        std::optional<run_result<detail::result<void>>>& r,
        std::function<void()> before_return)
{
    co_await no_actor_context();
    r = run(actor_with_specific_context(context).result());
    before_return();
}

TEST(ActorTest, StartNestedWithSpecificContext) {
    test_scheduler scheduler;
    actor_context context(scheduler);
    std::optional<run_result<detail::result<void>>> r1;
    auto r = run(actor_without_context_runs_specific_context(context, r1,
        [&]{
            EXPECT_TRUE(r1);
            EXPECT_FALSE(*r1);
        }).result());
    EXPECT_TRUE(r && r->has_value());
    EXPECT_TRUE(r1 && !*r1);
    ASSERT_EQ(scheduler.queue.size(), 1u);
    scheduler.run_next();
    ASSERT_EQ(scheduler.queue.size(), 0u);
    EXPECT_TRUE(*r1 && (*r1)->has_value());
}

actor<void> actor_check_sleep(const actor_context& context, bool expected,
        std::function<void()> before_sleep = {})
{
    co_await context();
    if (before_sleep) {
        before_sleep();
    }
    bool success = co_await context.sleep_for(std::chrono::milliseconds(100));
    EXPECT_EQ(success, expected);
}

TEST(ActorTest, Sleep) {
    test_scheduler scheduler;
    actor_context context(scheduler);
    {
        SCOPED_TRACE("no actor context");
        auto r = run(actor_check_sleep(no_actor_context, false).result());
        EXPECT_TRUE(r && r->has_value());
    }
    {
        SCOPED_TRACE("timers disabled");
        auto r = run(actor_check_sleep(context, false).result());
        ASSERT_EQ(scheduler.queue.size(), 0u);
        EXPECT_TRUE(r && r->has_value());
    }
    scheduler.timers_enabled = true;
    {
        SCOPED_TRACE("timer triggers");
        auto r = run(actor_check_sleep(context, true).result());
        ASSERT_EQ(scheduler.queue.size(), 0u);
        ASSERT_EQ(scheduler.timers.size(), 1u);
        scheduler.wake_next();
        ASSERT_EQ(scheduler.timers.size(), 0u);
        // Returning from a timer will defer
        ASSERT_EQ(scheduler.queue.size(), 1u);
        scheduler.run_next();
        ASSERT_EQ(scheduler.queue.size(), 0u);
        EXPECT_TRUE(r && r->has_value());
    }
    {
        SCOPED_TRACE("cancelled before sleep");
        stop_source source;
        auto before_sleep = [&]{
            source.request_stop();
        };
        auto r = run(with_stop_token(source.get_token(), actor_check_sleep(context, false, before_sleep).result()));
        ASSERT_EQ(scheduler.queue.size(), 0u);
        ASSERT_EQ(scheduler.timers.size(), 0u);
        EXPECT_TRUE(r && r->has_value());
    }
    {
        SCOPED_TRACE("cancelled during sleep");
        stop_source source;
        auto r = run(with_stop_token(source.get_token(), actor_check_sleep(context, false).result()));
        ASSERT_EQ(scheduler.queue.size(), 0u);
        ASSERT_EQ(scheduler.timers.size(), 1u);
        source.request_stop();
        ASSERT_EQ(scheduler.timers.size(), 0u);
        ASSERT_EQ(scheduler.queue.size(), 1u);
        scheduler.run_next();
        ASSERT_EQ(scheduler.queue.size(), 0u);
        EXPECT_TRUE(r && r->has_value());
    }
}
