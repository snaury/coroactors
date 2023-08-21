#include <actor.h>
#include <with_continuation.h>
#include <deque>

using namespace coroactors;

class Queue {
public:
    Queue(actor_scheduler& scheduler)
        : context(scheduler)
    {}

    actor<void> push(int value) {
        co_await context;
        if (awaiters.empty()) {
            values.push_back(value);
        } else {
            auto c = std::move(awaiters.front());
            awaiters.pop_front();
            c.resume(value);
        }
    }

    actor<int> pop() {
        co_await context;
        if (values.empty()) {
            co_return co_await with_continuation<int>([this](continuation<int> c) {
                awaiters.push_back(std::move(c));
            });
        } else {
            int value = values.front();
            values.pop_front();
            co_return value;
        }
    }

private:
    actor_context context;
    std::deque<int> values;
    std::deque<continuation<int>> awaiters;
};
