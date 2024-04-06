#include <coroactors/detail/blocking_queue.h>
#include <benchmark/benchmark.h>
#include <thread>
#include <mutex>
#include <deque>
#if HAVE_ABSEIL
#include <absl/synchronization/mutex.h>
#endif

using namespace coroactors;

template<class T>
struct mutex_queue {
    std::mutex mutex_;
    std::condition_variable not_empty_;
    std::deque<T> items_;

    template<class... Args>
    void push(Args&&... args) {
        std::lock_guard g(mutex_);
        items_.emplace_back(std::forward<Args>(args)...);
        not_empty_.notify_one();
    }

    T pop() {
        std::unique_lock g(mutex_);
        while (items_.empty()) {
            not_empty_.wait(g);
        }
        T item = std::move(items_.front());
        items_.pop_front();
        return item;
    }
};

#if HAVE_ABSEIL
template<class T>
struct absl_mutex_queue {
    absl::Mutex mutex_;
    std::deque<T> items_;

    template<class... Args>
    void push(Args&&... args) {
        absl::MutexLock g(&mutex_);
        items_.emplace_back(std::forward<Args>(args)...);
    }

    T pop() {
        absl::MutexLock g(&mutex_, absl::Condition(this, &absl_mutex_queue::not_empty));
        T item = std::move(items_.front());
        items_.pop_front();
        return item;
    }

private:
    bool not_empty() const {
        return !items_.empty();
    }
};
#endif

template<class TQueue>
struct BM_Queue : public benchmark::Fixture {
    std::optional<TQueue> Queue;

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

    void RunMPSC(benchmark::State& state) {
        int last = 0;
        if (state.thread_index() == 0) {
            size_t producers = state.threads() - 1;
            bool pushLocal = producers == 0;
            if (pushLocal) {
                producers = 1;
            }
            for (auto _ : state) {
                if (pushLocal) {
                    Queue->push(++last);
                }
                for (size_t i = 0; i < producers; ++i) {
                    Queue->pop();
                }
            }
            state.SetItemsProcessed(state.iterations() * producers);
        } else {
            for (auto _ : state) {
                Queue->push(++last);
            }
        }
    }

    void RunMPMC(benchmark::State& state) {
        if (state.thread_index() % 2 == 0) {
            for (auto _ : state) {
                Queue->pop();
            }
            state.SetItemsProcessed(state.iterations());
        } else {
            int last = 0;
            for (auto _ : state) {
                Queue->push(++last);
            }
        }
    }
};

using BM_BlockingQueue = BM_Queue<detail::blocking_queue<int>>;

BENCHMARK_DEFINE_F(BM_BlockingQueue, MPSC)(benchmark::State& state) {
    RunMPSC(state);
}

BENCHMARK_REGISTER_F(BM_BlockingQueue, MPSC)
    ->ThreadRange(1, 32)
    ->UseRealTime();

BENCHMARK_DEFINE_F(BM_BlockingQueue, MPMC)(benchmark::State& state) {
    RunMPMC(state);
}

BENCHMARK_REGISTER_F(BM_BlockingQueue, MPMC)
    ->ThreadRange(2, 32)
    ->UseRealTime();

using BM_MutexQueue = BM_Queue<mutex_queue<int>>;

BENCHMARK_DEFINE_F(BM_MutexQueue, MPSC)(benchmark::State& state) {
    RunMPSC(state);
}

BENCHMARK_REGISTER_F(BM_MutexQueue, MPSC)
    ->ThreadRange(1, 32)
    ->UseRealTime();

BENCHMARK_DEFINE_F(BM_MutexQueue, MPMC)(benchmark::State& state) {
    RunMPMC(state);
}

BENCHMARK_REGISTER_F(BM_MutexQueue, MPMC)
    ->ThreadRange(2, 32)
    ->UseRealTime();

#if HAVE_ABSEIL
using BM_AbseilQueue = BM_Queue<absl_mutex_queue<int>>;

BENCHMARK_DEFINE_F(BM_AbseilQueue, MPSC)(benchmark::State& state) {
    RunMPSC(state);
}

BENCHMARK_REGISTER_F(BM_AbseilQueue, MPSC)
    ->ThreadRange(1, 32)
    ->UseRealTime();

BENCHMARK_DEFINE_F(BM_AbseilQueue, MPMC)(benchmark::State& state) {
    RunMPMC(state);
}

BENCHMARK_REGISTER_F(BM_AbseilQueue, MPMC)
    ->ThreadRange(2, 32)
    ->UseRealTime();
#endif

int main(int argc, char** argv) {
    benchmark::Initialize(&argc, argv);
    benchmark::RunSpecifiedBenchmarks();
    benchmark::Shutdown();
    return 0;
}
