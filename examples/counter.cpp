#include <coroactors/actor.h>

using namespace coroactors;

class Counter {
public:
    Counter(actor_scheduler& scheduler)
        : context(scheduler)
    {}

    actor<int> get() const {
        co_await context();
        co_return value_;
    }

    actor<void> set(int value) {
        co_await context();
        value_ = value;
    }

    actor<int> increment() {
        co_await context();
        co_return ++value_;
    }

private:
    actor_context context;
    int value_ = 0;
};
