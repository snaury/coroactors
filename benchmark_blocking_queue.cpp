#include <coroactors/detail/blocking_queue.h>
#include <benchmark/benchmark.h>
#include <thread>

using namespace coroactors;

struct BM_BlockingQueue : public benchmark::Fixture {
    std::optional<detail::TBlockingQueue<int>> Queue;

    void SetUp(const benchmark::State& state) {
        if (state.thread_index() == 0) {
            Queue.emplace();
        }
    }

    void TearDown(const benchmark::State& state) {
        if (state.thread_index() == 0) {
            Queue.reset();
        }
    }
};

BENCHMARK_DEFINE_F(BM_BlockingQueue, MPSC)(benchmark::State& state) {
    int last = 0;
    if (state.thread_index() == 0) {
        size_t producers = state.threads() - 1;
        bool pushLocal = producers == 0;
        if (pushLocal) {
            producers = 1;
        }
        for (auto _ : state) {
            if (pushLocal) {
                Queue->Push(++last);
            }
            for (size_t i = 0; i < producers; ++i) {
                Queue->Pop();
            }
        }
        state.SetItemsProcessed(state.iterations() * producers);
    } else {
        for (auto _ : state) {
            Queue->Push(++last);
        }
    }
}

BENCHMARK_REGISTER_F(BM_BlockingQueue, MPSC)
    ->ThreadRange(1, 32)
    ->UseRealTime();

BENCHMARK_DEFINE_F(BM_BlockingQueue, MPMC)(benchmark::State& state) {
    if (state.thread_index() % 2 == 0) {
        for (auto _ : state) {
            Queue->Pop();
        }
        state.SetItemsProcessed(state.iterations());
    } else {
        int last = 0;
        for (auto _ : state) {
            Queue->Push(++last);
        }
    }
}

BENCHMARK_REGISTER_F(BM_BlockingQueue, MPMC)
    ->ThreadRange(2, 32)
    ->UseRealTime();

int main(int argc, char** argv) {
    benchmark::Initialize(&argc, argv);
    benchmark::RunSpecifiedBenchmarks();
    benchmark::Shutdown();
    return 0;
}   
