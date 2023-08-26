#include <coroactors/actor.h>
#include <coroactors/detach_awaitable.h>
#include <benchmark/benchmark.h>
#include <absl/synchronization/mutex.h>
#include <deque>

using namespace coroactors;

class TSimpleScheduler : public actor_scheduler {
public:
    void post(std::coroutine_handle<> c) override {
        queue.push_back(c);
    }

    bool preempt() const override {
        return false;
    }

    void run() {
        while (!queue.empty()) {
            ++processed;
            auto c = queue.front();
            queue.pop_front();
            c.resume();
        }
    }

public:
    size_t processed = 0;

private:
    std::deque<std::coroutine_handle<>> queue;
};

class TCounterServiceActor {
public:
    TCounterServiceActor(actor_scheduler& scheduler)
        : context(scheduler)
    {}

    __attribute__((__noinline__))
    actor<int> increment() {
        co_await context();
        co_return ++value_;
    }

private:
    actor_context context;
    int value_ = 0;
};

class TCounterServiceMutex {
public:
    __attribute__((__noinline__))
    int increment() {
        absl::MutexLock l(&Lock);
        return ++value_;
    }

private:
    absl::Mutex Lock;
    int value_ = 0;
};

class TTestServiceActor {
public:
    TTestServiceActor(actor_scheduler& scheduler, TCounterServiceActor& counter)
        : context(scheduler)
        , counter(counter)
    {}

    __attribute__((__noinline__))
    actor<int> get_const_immediate() const {
        co_return 42;
    }

    __attribute__((__noinline__))
    actor<int> get_const_context() const {
        co_await context();
        co_return 42;
    }

    __attribute__((__noinline__))
    actor<int> get_indirect() const {
        co_await context();
        co_return co_await counter.increment();
    }

    actor<void> run_const_immediate(benchmark::State& state) {
        co_await context();
        for (auto _ : state) {
            int value = co_await get_const_immediate();
            benchmark::DoNotOptimize(value);
        }
        state.SetItemsProcessed(state.iterations());
    }

    actor<void> run_const_context(benchmark::State& state) {
        co_await context();
        for (auto _ : state) {
            int value = co_await get_const_context();
            benchmark::DoNotOptimize(value);
        }
        state.SetItemsProcessed(state.iterations());
    }

    actor<void> run_direct(benchmark::State& state) {
        co_await context();
        for (auto _ : state) {
            int value = co_await counter.increment();
            benchmark::DoNotOptimize(value);
        }
        state.SetItemsProcessed(state.iterations());
    }

    actor<void> run_indirect(benchmark::State& state) {
        co_await context();
        for (auto _ : state) {
            int value = co_await get_indirect();
            benchmark::DoNotOptimize(value);
        }
        state.SetItemsProcessed(state.iterations());
    }

private:
    actor_context context;
    TCounterServiceActor& counter;
};

class TTestServiceMutex {
public:
    TTestServiceMutex(TCounterServiceMutex& counter)
        : counter(counter)
    {}

    __attribute__((__noinline__))
    int get_const() {
        return 42;
    }

    void run_const(benchmark::State& state) {
        for (auto _ : state) {
            int value = get_const();
            benchmark::DoNotOptimize(value);
        }
        state.SetItemsProcessed(state.iterations());
    }

    void run_direct(benchmark::State& state) {
        for (auto _ : state) {
            int value = counter.increment();
            benchmark::DoNotOptimize(value);
        }
        state.SetItemsProcessed(state.iterations());
    }

private:
    TCounterServiceMutex& counter;
};

static void BM_Actor_Call_Const_Immediate(benchmark::State& state) {
    TSimpleScheduler scheduler;
    TCounterServiceActor counter(scheduler);
    TTestServiceActor test(scheduler, counter);
    detach_awaitable(test.run_const_immediate(state));
    scheduler.run();
    state.counters["scheduled"] = scheduler.processed;
}

BENCHMARK(BM_Actor_Call_Const_Immediate);

static void BM_Actor_Call_Const_Context(benchmark::State& state) {
    TSimpleScheduler scheduler;
    TCounterServiceActor counter(scheduler);
    TTestServiceActor test(scheduler, counter);
    detach_awaitable(test.run_const_context(state));
    scheduler.run();
    state.counters["scheduled"] = scheduler.processed;
}

BENCHMARK(BM_Actor_Call_Const_Context);

static void BM_Actor_Call_Direct(benchmark::State& state) {
    TSimpleScheduler scheduler;
    TCounterServiceActor counter(scheduler);
    TTestServiceActor test(scheduler, counter);
    detach_awaitable(test.run_direct(state));
    scheduler.run();
    state.counters["scheduled"] = scheduler.processed;
}

BENCHMARK(BM_Actor_Call_Direct);

static void BM_Actor_Call_Indirect(benchmark::State& state) {
    TSimpleScheduler scheduler;
    TCounterServiceActor counter(scheduler);
    TTestServiceActor test(scheduler, counter);
    detach_awaitable(test.run_indirect(state));
    scheduler.run();
    state.counters["scheduled"] = scheduler.processed;
}

BENCHMARK(BM_Actor_Call_Indirect);

static void BM_Normal_Call_Const(benchmark::State& state) {
    TCounterServiceMutex counter;
    TTestServiceMutex test(counter);
    test.run_const(state);
}

BENCHMARK(BM_Normal_Call_Const);

static void BM_Normal_Call_Service(benchmark::State& state) {
    TCounterServiceMutex counter;
    TTestServiceMutex test(counter);
    test.run_direct(state);
}

BENCHMARK(BM_Normal_Call_Service);

int main(int argc, char** argv) {
    benchmark::Initialize(&argc, argv);
    benchmark::RunSpecifiedBenchmarks();
    benchmark::Shutdown();
    return 0;
}
