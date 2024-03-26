#include <coroactors/async.h>

using namespace coroactors;

class Counter {
public:
    Counter(actor_scheduler& scheduler)
        : context(scheduler)
    {}

    async<int> get() const {
        co_await context();
        co_return value_;
    }

    async<void> set(int value) {
        co_await context();
        value_ = value;
    }

    async<int> increment() {
        co_await context();
        co_return ++value_;
    }

private:
    actor_context context;
    int value_ = 0;
};
