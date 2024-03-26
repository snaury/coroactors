#include <coroactors/coro.h>
#include <gtest/gtest.h>

#include <coroactors/packaged_awaitable.h>

using namespace coroactors;

TEST(TestCoro, CoroVoid) {
    auto r = packaged_awaitable([]() -> coro<void> {
        co_return;
    }());

    EXPECT_TRUE(r.success());
}

TEST(TestCoro, CoroInt) {
    auto r = packaged_awaitable([]() -> coro<int> {
        co_return 42;
    }());

    EXPECT_EQ(*r, 42);
}

TEST(TestCoro, CoroThrow) {
    struct special_error {};

    auto r = packaged_awaitable([]() -> coro<void> {
        throw special_error{};
        co_return;
    }());

    EXPECT_THROW(*r, special_error);
}

TEST(TestCoro, CoroAwaitTwice) {
    auto r = packaged_awaitable([]() -> coro<void> {
        auto t = []() -> coro<int> {
            co_return 42;
        }();

        int value = co_await t;
        EXPECT_EQ(value, 42);

        int value2 = co_await std::move(t);
        EXPECT_EQ(value2, 42);
    }());
}
