#include "actor.h"
#include "actor_detach.h"
#include "detail/blocking_queue.h"
#include <deque>
#include <iostream>
#include <string>
#include <vector>

using namespace coroactors;

using TClock = std::chrono::steady_clock;
using TTime = std::chrono::time_point<TClock>;

class TPingable {
public:
    actor<int> ping() {
        co_await context;
        int result = ++counter;
        co_return result;
    }

private:
    actor_context context = actor_context::create();
    int counter = 0;
};

class TPinger {
public:
    struct TRunResult {
        std::chrono::microseconds elapsed;
        std::chrono::microseconds max_latency;
    };

    TPinger(TPingable& pingable)
        : pingable(pingable)
    {}

    actor<TRunResult> run(int count) {
        TTime start = TClock::now();

        co_await context;

        TTime end = TClock::now();
        auto max_latency = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

        for (int i = 0; i < count; ++i) {
            TTime call_start = end;
            int value = co_await pingable.ping();
            (void)value;
            TTime call_end = TClock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(call_end - call_start);
            max_latency = std::max(max_latency, elapsed);
            end = call_end;
        }

        auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

        co_return TRunResult{
            elapsed,
            max_latency,
        };
    }

private:
    actor_context context = actor_context::create();
    TPingable& pingable;
};

class TScheduler : public actor_scheduler {
public:
    TScheduler(size_t threads) {
        for (size_t i = 0; i < threads; ++i) {
            Threads.emplace_back([this]{
                RunWorker();
            });
        }
    }

    ~TScheduler() {
        // Send a stop signal for every thread
        for (size_t i = 0; i < Threads.size(); ++i) {
            Queue.Push(nullptr);
        }
        for (auto& thread : Threads) {
            thread.join();
        }
        if (auto c = Queue.TryPop()) {
            assert(false && "Unexpected scheduler shutdown with non-empty queue");
        }
    }

    void schedule(std::coroutine_handle<> c) override {
        assert(c && "Cannot schedule a null continuation");
        Queue.Push(c);
    }

    bool preempt() const override {
        if (thread_deadline) {
            return TClock::now() >= *thread_deadline;
        }
        return false;
    }

private:
    void RunWorker() {
        actor_scheduler::set(this);
        TTime deadline{};
        thread_deadline = &deadline;
        while (auto c = Queue.Pop()) {
            deadline = TClock::now() + std::chrono::microseconds(10);
            c.resume();
        }
        thread_deadline = nullptr;
    }

private:
    detail::TBlockingQueue<std::coroutine_handle<>> Queue;
    std::vector<std::thread> Threads;

    static inline thread_local const TTime* thread_deadline{ nullptr };
};

template<class T>
    requires (!std::same_as<T, void>)
std::vector<T> run_sync(std::vector<actor<T>> actors) {
    std::atomic_signed_lock_free waiting(actors.size());
    std::vector<T> results(actors.size());

    for (size_t i = 0; i < actors.size(); ++i) {
        detach_awaitable(std::move(actors[i]), [&waiting, &results, i](T&& result){
            results[i] = std::move(result);
            if (0 == --waiting) {
                waiting.notify_one();
            }
        });
    }
    actors.clear();

    for (;;) {
        size_t value = waiting.load();
        if (value == 0) {
            break;
        }
        waiting.wait(value);
    }

    return results;
}

void run_sync(std::vector<actor<void>> actors) {
    std::atomic_signed_lock_free waiting(actors.size());

    for (auto& a : actors) {
        detach_awaitable(std::move(a), [&]{
            if (0 == --waiting) {
                waiting.notify_one();
            }
        });
    }
    actors.clear();

    for (;;) {
        size_t value = waiting.load();
        if (value == 0) {
            break;
        }
        waiting.wait(value);
    }
}

template<class T>
    requires (!std::same_as<T, void>)
T run_sync(actor<T> a) {
    std::vector<actor<T>> actors;
    actors.push_back(std::move(a));
    auto results = run_sync(std::move(actors));
    return results[0];
}

void run_sync(actor<void> a) {
    std::vector<actor<void>> actors;
    actors.push_back(std::move(a));
    run_sync(std::move(actors));
}

int main(int argc, char** argv) {
    int numThreads = 1;
    int numPingers = 1;
    int numPingables = 1;
    long long count = 10'000'000;

    for (int i = 1; i < argc; ++i) {
        std::string arg(argv[i]);
        if ((arg == "-t" || arg == "--threads") && i + 1 < argc) {
            numThreads = std::stoi(argv[++i]);
            continue;
        }
        if ((arg == "-p") && i + 1 < argc) {
            numPingers = std::stoi(argv[++i]);
            numPingables = numPingers;
            continue;
        }
        if ((arg == "--pingers") && i + 1 < argc) {
            numPingers = std::stoi(argv[++i]);
            continue;
        }
        if ((arg == "--pingables") && i + 1 < argc) {
            numPingables = std::stoi(argv[++i]);
            continue;
        }
        if ((arg == "-c" || arg == "--count") && i + 1 < argc) {
            count = std::stoi(argv[++i]);
            continue;
        }
        std::cerr << "ERROR: unexpected argument: " << argv[i] << std::endl;
        return 1;
    }

    TScheduler scheduler(numThreads);
    actor_scheduler::set(&scheduler);

    std::deque<TPingable> pingables;
    std::deque<TPinger> pingers;

    for (int i = 0; i < numPingables; ++i) {
        pingables.emplace_back();
    }
    for (int i = 0; i < numPingers; ++i) {
        pingers.emplace_back(pingables[i % pingables.size()]);
    }

    std::cout << "Warming up..." << std::endl;
    run_sync(pingers[0].run(count / numPingers / 100));

    std::cout << "Starting..." << std::endl;
    std::vector<actor<TPinger::TRunResult>> runs;
    auto start = TClock::now();
    for (auto& pinger : pingers) {
        runs.push_back(pinger.run(count / numPingers));
    }
    auto results = run_sync(std::move(runs));
    auto end = TClock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

    std::chrono::microseconds maxLatency = {};
    for (auto& result : results) {
        maxLatency = std::max(maxLatency, result.max_latency);
    }

    long long rps = count * 1000000LL / elapsed.count();
    std::cout << "Finished in " << (elapsed.count() / 1000) << "ms (" << rps << "/s)"
        ", max latency = " << (maxLatency.count()) << "us" << std::endl;

    return 0;
}
