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

coro<void> fooN(int n) {
    if (n > 1) {
        co_await fooN(n - 1);
    } else {
        co_await foo0();
    }
}

coro<void> bar(int count, int n, bool dyn) {
    if (dyn) {
        for (int i = 0; i < count; ++i) {
            //co_await foo5();
            co_await fooN(n);
        }
    } else {
        switch (n) {
            case 5:
                for (int i = 0; i < count; ++i) {
                    co_await foo5();
                }
                break;
            case 4:
                for (int i = 0; i < count; ++i) {
                    co_await foo4();
                }
                break;
            case 3:
                for (int i = 0; i < count; ++i) {
                    co_await foo3();
                }
                break;
            case 2:
                for (int i = 0; i < count; ++i) {
                    co_await foo2();
                }
                break;
            case 1:
                for (int i = 0; i < count; ++i) {
                    co_await foo1();
                }
                break;
        }
    }
}

coro<void> async_main() {
    int count = 100'000'000;
    for (int n = 1; n <= 5; ++n) {
        for (int dyn = 0; dyn <= 1; ++dyn) {
            auto start = std::chrono::steady_clock::now();
            co_await bar(count, n, dyn != 0);
            auto end = std::chrono::steady_clock::now();
            auto elapsedNs = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
            auto callNs = double(elapsedNs) / double(count);
            auto suffix = dyn != 0 ? " dynamic" : "";
            printf("Single call is %fns for n=%d%s\n", callNs, n, suffix);
        }
    }
}

int main() {
    auto m = async_main();
    m.await_suspend(std::noop_coroutine()).resume();
    return 0;
}
