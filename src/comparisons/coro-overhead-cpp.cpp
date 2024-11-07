#include <coroactors/coro.h>
#include <chrono>

using namespace coroactors;

struct Fooer {
    virtual coro<void> foo() = 0;
};

Fooer* g_fooer = nullptr;

coro<void> foo0() {
    if (auto* fooer = g_fooer) {
        co_await fooer->foo();
    }
}

coro<void> foo1() {
    co_await foo0();
}

coro<void> foo2() {
    co_await foo1();
}

coro<void> foo3() {
    co_await foo2();
}

coro<void> foo4() {
    co_await foo3();
}

coro<void> foo5() {
    co_await foo4();
}

coro<void> bar(int count) {
    for (int i = 0; i < count; ++i) {
        co_await foo5();
    }
}

coro<void> async_main() {
    int count = 100'000'000;
    auto start = std::chrono::steady_clock::now();
    co_await bar(count);
    auto end = std::chrono::steady_clock::now();
    auto elapsedNs = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    auto callNs = double(elapsedNs) / double(count);
    printf("Single call is %fns\n", callNs);
}

int main() {
    auto m = async_main();
    m.await_suspend(std::noop_coroutine()).resume();
    return 0;
}
