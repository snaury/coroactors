#include "timer_service.h"
#include <coroactors/actor.h>
#include <coroactors/detach_awaitable.h>

using namespace coroactors;

void run_sync(std::vector<actor<void>> actors) {
    std::atomic_signed_lock_free waiting(actors.size());

    for (auto& a : actors) {
        detach_awaitable(std::move(a), [&]{
            if (0 == --waiting) {
                waiting.notify_one();
            }
        });
    }
    actors.clear();

    for (;;) {
        size_t value = waiting.load();
        if (value == 0) {
            break;
        }
        waiting.wait(value);
    }
}

void run_sync(actor<void> a) {
    std::vector<actor<void>> actors;
    actors.push_back(std::move(a));
    run_sync(std::move(actors));
}

actor<void> runme(timer_service& timer, int count) {
    co_await no_actor_context();
    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < count; ++i) {
        printf("Sleeping...\n");
        co_await timer.after(std::chrono::milliseconds(100));
    }
    auto end = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    printf("Loop time: %dms\n", elapsed.count());
}

int main(int argc, char** argv) {
    timer_service timer;

    run_sync(runme(timer, 10));

    return 0;
}
