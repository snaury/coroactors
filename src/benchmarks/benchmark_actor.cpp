#include <coroactors/actor.h>
#include <coroactors/detach_awaitable.h>
#include <benchmark/benchmark.h>
#include <deque>
#include <mutex>

#if HAVE_ABSEIL
#include <absl/synchronization/mutex.h>
#endif

using namespace coroactors;

class TSimpleScheduler : public actor_scheduler {
public:
    void post(execute_callback_type c) override {
        queue.push_back(std::move(c));
    }

    bool preempt() override {
        return false;
    }

    void run() {
        while (!queue.empty()) {
            ++processed;
            auto cont = std::move(queue.front());
            queue.pop_front();
            cont();
        }
    }

public:
    size_t processed = 0;

private:
    std::deque<execute_callback_type> queue;
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

class ICounterServiceMutex {
public:
    virtual int get_const() = 0;

    virtual int increment() = 0;
};

class TCounterServiceStdMutex : public ICounterServiceMutex {
public:
    int get_const() override {
        return 42;
    }

    int increment() override {
        std::unique_lock l(lock);
        return ++value_;
    }

private:
    std::mutex lock;
    int value_ = 0;
};

#if HAVE_ABSEIL
class TCounterServiceAbslMutex : public ICounterServiceMutex {
public:
    int get_const() override {
        return 42;
    }

    int increment() override {
        absl::MutexLock l(&Lock);
        return ++value_;
    }

private:
    absl::Mutex Lock;
    int value_ = 0;
};
#endif

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
    TTestServiceMutex(ICounterServiceMutex& counter)
        : counter(counter)
    {}

    __attribute__((__noinline__))
    void run_const(benchmark::State& state) {
        for (auto _ : state) {
            int value = counter.get_const();
            benchmark::DoNotOptimize(value);
        }
        state.SetItemsProcessed(state.iterations());
    }

    __attribute__((__noinline__))
    void run_direct(benchmark::State& state) {
        for (auto _ : state) {
            int value = counter.increment();
            benchmark::DoNotOptimize(value);
        }
        state.SetItemsProcessed(state.iterations());
    }

private:
    ICounterServiceMutex& counter;
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
    TCounterServiceStdMutex counter;
    TTestServiceMutex test(counter);
    test.run_const(state);
}

BENCHMARK(BM_Normal_Call_Const);

static void BM_StdMutex_Call_Service(benchmark::State& state) {
    TCounterServiceStdMutex counter;
    TTestServiceMutex test(counter);
    test.run_direct(state);
}

BENCHMARK(BM_StdMutex_Call_Service);

#if HAVE_ABSEIL
static void BM_AbslMutex_Call_Service(benchmark::State& state) {
    TCounterServiceAbslMutex counter;
    TTestServiceMutex test(counter);
    test.run_direct(state);
}

BENCHMARK(BM_AbslMutex_Call_Service);
#endif

int main(int argc, char** argv) {
    benchmark::Initialize(&argc, argv);
    benchmark::RunSpecifiedBenchmarks();
    benchmark::Shutdown();
    return 0;
}
