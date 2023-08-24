#include <coroactors/detail/atomic_semaphore.h>
#include <coroactors/detail/mailbox.h>
#include <benchmark/benchmark.h>
#include <thread>

using namespace coroactors;

static void TestBasics() {
    int item;
    detail::mailbox<int> mailbox;
    assert(mailbox.peek() == nullptr);
    bool push1 = mailbox.emplace(1);
    assert(push1 == false);
    bool push2 = mailbox.emplace(2);
    assert(push2 == false);
    item = mailbox.pop_default();
    assert(item == 1);
    item = mailbox.pop_default();
    assert(item == 2);
    item = mailbox.pop_default();
    assert(item == 0);
    bool push3 = mailbox.emplace(3);
    assert(push3 == true);
    const int* current = mailbox.peek();
    assert(current && *current == 3);
    bool unlocked = mailbox.try_unlock();
    assert(!unlocked);
    item = mailbox.pop_default();
    assert(item == 3);
    unlocked = mailbox.try_unlock();
    assert(unlocked);
}

static void BM_Push(benchmark::State& state) {
    detail::mailbox<int> mailbox;
    benchmark::DoNotOptimize(mailbox);
    int last = 0;
    for (auto _ : state) {
        mailbox.emplace(++last);
    }
    state.SetItemsProcessed(state.iterations());
}

BENCHMARK(BM_Push);

static void BM_PushPop_NoThreads(benchmark::State& state) {
    detail::mailbox<int> mailbox;
    benchmark::DoNotOptimize(mailbox);
    int last = 0;
    for (auto _ : state) {
        mailbox.emplace(++last);
        int value = mailbox.pop_default();
        assert(value == last);
        benchmark::DoNotOptimize(value);
    }
    state.SetItemsProcessed(state.iterations());
}

BENCHMARK(BM_PushPop_NoThreads);

struct BM_PushPop : public benchmark::Fixture {
    struct TState {
        detail::mailbox<int> Mailbox;
        detail::semaphore_atomic_t Semaphore{ 0 }; // initially locked
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
            int value = State->Mailbox.pop_default();
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
        if (State->Mailbox.emplace(value)) {
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
