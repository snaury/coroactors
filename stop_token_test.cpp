#include <coroactors/detail/stop_token_polyfill.h>
#include <any>
#include <functional>
#include <optional>
#include <thread>
#include <mutex>
#include <gtest/gtest.h>

using namespace coroactors::detail;

TEST(StopTokenTest, EmptyStopToken) {
    stop_token a, b;
    EXPECT_FALSE(a.stop_possible());
    EXPECT_FALSE(a.stop_requested());
    EXPECT_TRUE(a == b);
    swap(a, b);
    EXPECT_TRUE(a == b);
    a.swap(b);
    EXPECT_TRUE(a == b);
    a = b;
    EXPECT_TRUE(a == b);
}

TEST(StopTokenTest, EmptyStopSource) {
    stop_source a(nostopstate);
    stop_source b(nostopstate);
    EXPECT_TRUE(a == b);
    EXPECT_FALSE(a.stop_possible());
    EXPECT_FALSE(a.stop_requested());
    EXPECT_FALSE(a.request_stop());
    swap(a, b);
    EXPECT_TRUE(a == b);
    a.swap(b);
    EXPECT_TRUE(a == b);
    a = b;
    EXPECT_TRUE(a == b);

    // Already tested by EmptyStopToken
    stop_token at = a.get_token();
    EXPECT_TRUE(at == stop_token());
    stop_token bt = b.get_token();
    EXPECT_TRUE(bt == stop_token());
}

TEST(StopTokenTest, StopSourceAndTokenOps) {
    stop_token tempty;
    stop_source sempty(nostopstate);
    stop_source a, b;
    EXPECT_FALSE(a == b);
    EXPECT_FALSE(a == sempty);
    EXPECT_FALSE(b == sempty);
    stop_token at = a.get_token();
    stop_token bt = b.get_token();
    EXPECT_FALSE(at == bt);

    // copy and move constructors
    stop_source acopy(a);
    stop_source bcopy(b);
    EXPECT_TRUE(acopy == a);
    EXPECT_TRUE(bcopy == b);
    EXPECT_TRUE(acopy != bcopy);
    stop_source amove(std::move(a));
    EXPECT_TRUE(amove == acopy);
    EXPECT_TRUE(a == sempty);
    // same for stop_token
    stop_token atcopy(at);
    stop_token btcopy(bt);
    EXPECT_TRUE(atcopy == at);
    EXPECT_TRUE(btcopy == bt);
    EXPECT_TRUE(atcopy != btcopy);
    stop_token atmove(std::move(at));
    EXPECT_TRUE(atmove == atcopy);
    EXPECT_TRUE(at == tempty);

    // copy assign to empty
    a = acopy;
    EXPECT_TRUE(a == acopy);
    EXPECT_TRUE(a == amove);
    EXPECT_TRUE(acopy == amove);
    // same for stop_token
    at = atcopy;
    EXPECT_TRUE(at == atcopy);
    EXPECT_TRUE(at == atmove);
    EXPECT_TRUE(atcopy == atmove);

    // move assign to empty
    a = sempty;
    a = std::move(amove);
    EXPECT_TRUE(a == acopy);
    EXPECT_TRUE(amove == sempty);
    // same for stop_token
    at = tempty;
    at = std::move(atmove);
    EXPECT_TRUE(at == atcopy);
    EXPECT_TRUE(atmove == tempty);
    atmove = at;

    // copy assign to not empty
    stop_source bcopyreassign(a);
    bcopyreassign = b;
    EXPECT_TRUE(bcopyreassign == b);
    // same for stop_token
    stop_token btcopyreassign(at);
    btcopyreassign = bt;
    EXPECT_TRUE(btcopyreassign == bt);

    // move assign to not empty
    stop_source bmovereassign(a);
    bmovereassign = std::move(b);
    EXPECT_TRUE(bmovereassign == bcopyreassign);
    EXPECT_TRUE(b == sempty);
    // same for stop_token
    stop_token btmovereassign(at);
    btmovereassign = std::move(bt);
    EXPECT_TRUE(btmovereassign == btcopyreassign);
    EXPECT_TRUE(bt == tempty);

    // stop_source swaps
    a = acopy;
    b = bcopy;
    EXPECT_FALSE(a == b);
    swap(a, b);
    EXPECT_FALSE(a == b);
    EXPECT_TRUE(a == bcopy);
    EXPECT_TRUE(b == acopy);
    a.swap(b);
    EXPECT_FALSE(a == b);
    EXPECT_TRUE(a == acopy);
    EXPECT_TRUE(b == bcopy);

    // stop_token swaps
    at = atcopy;
    bt = btcopy;
    EXPECT_FALSE(at == bt);
    swap(at, bt);
    EXPECT_FALSE(at == bt);
    EXPECT_TRUE(at == btcopy);
    EXPECT_TRUE(bt == atcopy);
    at.swap(bt);
    EXPECT_FALSE(at == bt);
    EXPECT_TRUE(at == atcopy);
    EXPECT_TRUE(bt == btcopy);
}

TEST(StopTokenTest, StopSourceDestroyedBeforeStop) {
    std::optional<stop_source> a;
    a.emplace();
    EXPECT_TRUE(a->stop_possible());
    EXPECT_FALSE(a->stop_requested());
    stop_token b = a->get_token();
    EXPECT_TRUE(b.stop_possible());
    EXPECT_FALSE(b.stop_requested());
    a.reset();
    EXPECT_FALSE(b.stop_possible());
    EXPECT_FALSE(b.stop_requested());
    stop_token c;
    EXPECT_FALSE(b == c);
    EXPECT_TRUE(b != c);
}

TEST(StopTokenTest, StopSourceDestroyedAfterStop) {
    std::optional<stop_source> a;
    a.emplace();
    stop_token b = a->get_token();
    EXPECT_TRUE(a->request_stop());
    EXPECT_FALSE(a->request_stop());
    EXPECT_TRUE(a->stop_possible());
    EXPECT_TRUE(a->stop_requested());
    EXPECT_TRUE(b.stop_possible());
    EXPECT_TRUE(b.stop_requested());
    a.reset();
    EXPECT_TRUE(b.stop_possible());
    EXPECT_TRUE(b.stop_requested());
}

TEST(StopTokenTest, StopCallbackNotCalledOnEmptyToken) {
    stop_token a;
    stop_callback c(a, [&]{
        ADD_FAILURE() << "Unexpected callback call";
    });
}

TEST(StopTokenTest, StopCallbackNotCalledWithoutSource) {
    std::optional<stop_source> a;
    a.emplace();
    stop_token b = a->get_token();
    a.reset();
    stop_callback c(b, [&]{
        ADD_FAILURE() << "Unexpected callback without source";
    });
}

TEST(StopTokenTest, StopCallbackNotCalledWithoutStop) {
    stop_source a;
    stop_token b = a.get_token();
    stop_callback c(b, [&]{
        ADD_FAILURE() << "Unexpected callback without stop";
    });
}

TEST(StopTokenTest, StopCallbackMovesTokenWithSource) {
    stop_source a;
    stop_token b = a.get_token();
    ASSERT_FALSE(b == stop_token());
    stop_callback c(std::move(b), [&]{
        ADD_FAILURE() << "Unexpected callback without stop";
    });
    ASSERT_TRUE(b == stop_token());
}

TEST(StopTokenTest, StopCallbackMovesTokenWithoutSource) {
    std::optional<stop_source> a;
    a.emplace();
    stop_token b = a->get_token();
    a.reset();
    ASSERT_FALSE(b == stop_token());
    stop_callback c(std::move(b), [&]{
        ADD_FAILURE() << "Unexpected callback without stop";
    });
    ASSERT_TRUE(b == stop_token());
}

TEST(StopTokenTest, StopCallbackCalledAfterStop) {
    stop_source a;
    stop_token b = a.get_token();
    std::string called;
    stop_callback c(b, [&]{
        called.push_back('c');
    });
    EXPECT_EQ(called, "");
    EXPECT_TRUE(a.request_stop());
    EXPECT_EQ(called, "c");
    EXPECT_FALSE(a.request_stop());
    EXPECT_EQ(called, "c");
}

TEST(StopTokenTest, StopCallbackMultiple) {
    stop_source a;
    stop_token b = a.get_token();
    std::string called;
    stop_callback c(b, [&]{
        called.push_back('c');
    });
    stop_callback d(b, [&]{
        called.push_back('d');
    });
    a.request_stop();
    EXPECT_EQ(called, "dc");
    stop_callback e(b, [&]{
        called.push_back('e');
    });
    EXPECT_EQ(called, "dce");
}
TEST(StopTokenTest, StopCallbackAddFromCallback) {
    stop_source a;
    stop_token b = a.get_token();
    std::string called;
    stop_callback c(b, [&]{
        EXPECT_EQ(called, "");
        called.push_back('c');
        stop_callback d(b, [&]{
            called.push_back('d');
        });
        EXPECT_EQ(called, "cd");
    });
    a.request_stop();
    EXPECT_EQ(called, "cd");
}

TEST(StopTokenTest, StopCallbackDestroySelf) {
    stop_source a;
    stop_token b = a.get_token();
    std::optional<stop_callback<std::function<void()>>> c;
    c.emplace(b, [&]{
        c.reset();
    });
    ASSERT_TRUE(c);
    a.request_stop();
    ASSERT_FALSE(c);
}

void DoStopCallbackDestroyOthers(int kind) {
    stop_source a;
    stop_token b = a.get_token();
    std::optional<stop_callback<std::function<void()>>> c;
    std::optional<stop_callback<std::function<void()>>> d;
    std::optional<stop_callback<std::function<void()>>> e;
    std::optional<stop_callback<std::function<void()>>> f;
    std::string called;
    c.emplace(b, [&]{
        called.push_back('c');
    });
    d.emplace(b, [&]{
        called.push_back('d');
    });
    e.emplace(b, [&]{
        called.push_back('e');
    });
    f.emplace(b, [&]{
        called.push_back('f');
        switch (kind) {
        case 0:
            e.reset();
            d.reset();
            c.reset();
            break;
        case 1:
            c.reset();
            d.reset();
            e.reset();
            break;
        case 2:
            d.reset();
            e.reset();
            c.reset();
            break;
        }
    });
    a.request_stop();
    // e should be called first and remove others
    EXPECT_EQ(called, "f");
    EXPECT_FALSE(c);
    EXPECT_FALSE(d);
    EXPECT_FALSE(e);
    EXPECT_TRUE(f);
}

TEST(StopTokenTest, StopCallbackDestroyOthersForwards) {
    DoStopCallbackDestroyOthers(0);
}

TEST(StopTokenTest, StopCallbackDestroyOthersBackwards) {
    DoStopCallbackDestroyOthers(1);
}

TEST(StopTokenTest, StopCallbackDestroyOthersMixed) {
    DoStopCallbackDestroyOthers(2);
}

TEST(StopTokenTest, StopCallbackDestroySelfFromAnotherThread) {
    stop_source a;
    stop_token b = a.get_token();
    std::optional<stop_callback<std::function<void()>>> c;
    std::optional<std::thread> t;
    std::string called;
    sync_semaphore semaphore;
    std::atomic<bool> destroyed{ false };
    c.emplace(b, [&]{
        t.emplace([&]{
            semaphore.release();
            c.reset();
            destroyed.store(true);
        });
        semaphore.acquire();
        // Give thread some chance to finish
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        // It must wait until we return
        EXPECT_EQ(destroyed.load(), false);
    });
    a.request_stop();
    ASSERT_TRUE(t);
    t->join();
    EXPECT_FALSE(c);
    EXPECT_EQ(destroyed.load(), true);
}

TEST(StopTokenTest, StopCallbackDestroyOtherFromAnotherThread) {
    stop_source a;
    stop_token b = a.get_token();
    std::optional<stop_callback<std::function<void()>>> c;
    std::optional<stop_callback<std::function<void()>>> d;
    std::optional<std::thread> t;
    std::string called;
    c.emplace(b, [&]{
        called.push_back('c');
    });
    d.emplace(b, [&]{
        called.push_back('d');
        t.emplace([&]{
            c.reset();
        });
        t->join();
    });
    a.request_stop();
    EXPECT_EQ(called, "d");
}
