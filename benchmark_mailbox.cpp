#include "detail/mailbox.h"
#include <benchmark/benchmark.h>
#include <thread>

using namespace coroactors;

static void TestBasics() {
    int item;
    detail::TMailbox<int> mailbox;
    assert(mailbox.Peek() == nullptr);
    bool push1 = mailbox.Push(1);
    assert(push1 == false);
    bool push2 = mailbox.Push(2);
    assert(push2 == false);
    item = mailbox.Pop();
    assert(item == 1);
    item = mailbox.Pop();
    assert(item == 2);
    item = mailbox.Pop();
    assert(item == 0);
    bool push3 = mailbox.Push(3);
    assert(push3 == true);
    const int* current = mailbox.Peek();
    assert(current && *current == 3);
    bool unlocked = mailbox.TryUnlock();
    assert(!unlocked);
    item = mailbox.Pop();
    assert(item == 3);
    unlocked = mailbox.TryUnlock();
    assert(unlocked);
}

static void BM_Push(benchmark::State& state) {
    detail::TMailbox<int> mailbox;
    benchmark::DoNotOptimize(mailbox);
    int last = 0;
    for (auto _ : state) {
        mailbox.Push(++last);
    }
    state.SetItemsProcessed(state.iterations());
}

BENCHMARK(BM_Push);

static void BM_PushPop_NoThreads(benchmark::State& state) {
    detail::TMailbox<int> mailbox;
    benchmark::DoNotOptimize(mailbox);
    int last = 0;
    for (auto _ : state) {
        mailbox.Push(++last);
        int value = mailbox.Pop();
        assert(value == last);
        benchmark::DoNotOptimize(value);
    }
    state.SetItemsProcessed(state.iterations());
}

BENCHMARK(BM_PushPop_NoThreads);

struct BM_PushPop : public benchmark::Fixture {
    // Same as clang __cxx_contention_t for faster wait/notify
#ifdef __linux__
    using TSemaphoreValue = int32_t;
#else
    using TSemaphoreValue = int64_t;
#endif
    struct TState {
        detail::TMailbox<int> Mailbox;
        std::atomic<TSemaphoreValue> Semaphore{ 0 }; // initially locked
        std::optional<std::thread> Consumer;
        std::atomic<size_t> WakeUps{ 0 };
    };
    std::optional<TState> State;

    void SetUp(const benchmark::State& state) {
        if (state.thread_index() == 0) {
            State.emplace();
            State->Consumer.emplace([this, threads = state.threads()]{
                RunConsumer(threads);
            });
        }
    }

    void TearDown(benchmark::State& state) {
        Push(-1);
        if (state.thread_index() == 0) {
            State->Consumer->join();
            state.counters["WakeUps"] = State->WakeUps.load();
        }
    }

    void WaitMailbox() {
        while (State->Semaphore.load() == 0) {
            // wait for signal
            State->Semaphore.wait(0);
        }
        --State->Semaphore;
    }

    void RunConsumer(size_t threads) {
        while (threads > 0) {
            int value = State->Mailbox.Pop();
            if (value == 0) {
                // mailbox is empty and unlocked
                WaitMailbox();
                continue;
            }
            if (value == -1) {
                --threads;
                continue;
            }
        }
    }

    void Push(int value) {
        if (State->Mailbox.Push(value)) {
            WakeConsumer();
        }
    }

    void WakeConsumer() {
        ++State->WakeUps;
        ++State->Semaphore;
        State->Semaphore.notify_one();
    }
};

BENCHMARK_DEFINE_F(BM_PushPop, Producers)(benchmark::State& state) {
    int last = 0;
    for (auto _ : state) {
        Push(++last);
    }
    state.SetItemsProcessed(state.iterations());
}

BENCHMARK_REGISTER_F(BM_PushPop, Producers)->ThreadRange(1, 32)->UseRealTime();

int main(int argc, char** argv) {
    TestBasics();
    benchmark::Initialize(&argc, argv);
    benchmark::RunSpecifiedBenchmarks();
    benchmark::Shutdown();
    return 0;
}
