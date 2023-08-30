#include <coroutine>
#include <exception>

struct my_awaiter {
    my_awaiter() {
        printf("awaiter %p constructed\n", this);
    }
    ~my_awaiter() {
        printf("awaiter %p destroyed\n", this);
    }
    bool await_ready() {
        printf("awaiter %p await_ready\n", this);
        return false;
    }
    std::coroutine_handle<> await_suspend(std::coroutine_handle<> c) {
        printf("awaiter %p await_suspend\n", this);
        return c;
    }
    int await_resume() {
        printf("awaiter %p await_resume\n", this);
        return 42;
    }
};

struct my_awaitable {
    my_awaitable() {
        printf("awaitable %p constructed\n", this);
    }
    ~my_awaitable() {
        printf("awaitable %p destroyed\n", this);
    }
    my_awaiter operator co_await() {
        printf("awaitable %p operator co_await\n", this);
        return my_awaiter{};
    }
};

struct wrapper {
    my_awaiter awaiter;

    wrapper(my_awaitable&& awaitable)
        : awaiter(awaitable.operator co_await())
    {
        printf("wrapper %p constructed\n", this);
    }

    ~wrapper() {
        printf("wrapper %p destroyed\n", this);
    }

    bool await_ready() {
        printf("wrapper %p await_ready\n", this);
        return awaiter.await_ready();
    }

    std::coroutine_handle<> await_suspend(std::coroutine_handle<> c) {
        printf("wrapper %p await_suspend\n", this);
        return awaiter.await_suspend(c);
    }

    int await_resume() {
        printf("wrapper %p await_resume\n", this);
        return awaiter.await_resume();
    }
};

struct task {
    struct promise_type {
        promise_type() {
            printf("promise %p constructed\n", this);
        }
        ~promise_type() {
            printf("promise %p destroyed\n", this);
        }
        task get_return_object() noexcept { return task{}; }
        auto initial_suspend() noexcept {
            printf("promise %p initial suspend\n", this);
            return std::suspend_never{};
        }
        auto final_suspend() noexcept {
            printf("promise %p final suspend\n", this);
            return std::suspend_never{};
        }
        void unhandled_exception() noexcept { std::terminate(); }
        void return_value(int) {
            printf("promise %p return_value\n", this);
        }
        wrapper await_transform(my_awaitable&& awaitable) {
            return wrapper(std::move(awaitable));
        }
    };
};

my_awaitable make_my_awaitable() {
    return my_awaitable{};
}

task check_lifetimes() {
    co_return co_await make_my_awaitable();
}

int main() {
    check_lifetimes();
    return 0;
}
