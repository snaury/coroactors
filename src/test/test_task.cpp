#include <coroactors/task.h>
#include <gtest/gtest.h>

#include <coroactors/packaged_awaitable.h>

using namespace coroactors;

TEST(TestTask, TaskVoid) {
    auto r = packaged_awaitable([]() -> task<void> {
        co_return;
    }());

    EXPECT_TRUE(r.success());
}

TEST(TestTask, TaskInt) {
    auto r = packaged_awaitable([]() -> task<int> {
        co_return 42;
    }());

    EXPECT_EQ(*r, 42);
}

TEST(TestTask, TaskThrow) {
    struct special_error {};

    auto r = packaged_awaitable([]() -> task<void> {
        throw special_error{};
        co_return;
    }());

    EXPECT_THROW(*r, special_error);
}

TEST(TestTask, TaskAwaitTwice) {
    auto r = packaged_awaitable([]() -> task<void> {
        auto t = []() -> task<int> {
            co_return 42;
        }();

        int value = co_await t;
        EXPECT_EQ(value, 42);

        co_await t;

        ADD_FAILURE() << "Task was awaited twice";
    }());

    EXPECT_THROW(*r, std::logic_error);
}
