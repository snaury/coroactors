#include <coroactors/actor.h>
#include <coroactors/with_continuation.h>
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
            // Note: we use caller_context here to make sure push does not have
            // to serialize on this actor context when resuming continuations.
            // The callback still runs in our context though, so push_back to
            // awaiters is thread-safe. Without it we would first enqueue a
            // return from co_await on this context, and then immediately
            // switch to caller context, causing scheduler thrashing.
            co_return co_await actor_context::caller_context(
                with_continuation<int>([this](continuation<int> c) {
                    awaiters.push_back(std::move(c));
                }));
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
