#include <coroactors/async.h>
#include <coroactors/task_local.h>
#include <coroactors/packaged_awaitable.h>
#include <chrono>

using namespace coroactors;

struct Fooer {
    virtual async<void> foo() = 0;
};

inline task_local<Fooer*> g_fooer;

async<void> foo0() {
    if (auto* fooer = g_fooer.get()) {
        co_await fooer->foo();
    }
}

async<void> foo1() {
    co_await foo0();
}

async<void> foo2() {
    co_await foo1();
}

async<void> foo3() {
    co_await foo2();
}

async<void> foo4() {
    co_await foo3();
}

async<void> foo5() {
    co_await foo4();
}

async<void> bar(int count) {
    for (int i = 0; i < count; ++i) {
        co_await foo5();
    }
}

async<void> async_main() {
    int count = 100'000'000;
    auto start = std::chrono::steady_clock::now();
    co_await bar(count);
    auto end = std::chrono::steady_clock::now();
    auto elapsedNs = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    auto callNs = double(elapsedNs) / double(count);
    printf("Single call is %fns\n", callNs);
}

int main() {
    auto r = packaged_awaitable(async_main());
    return 0;
}
