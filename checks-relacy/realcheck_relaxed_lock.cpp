#include <atomic>
#include <chrono>
#include <thread>
#include <cstdio>

template<class base>
class padded : public base {
public:
    using base::base;

private:
    char padding[256 - sizeof(base)];
};

struct test_relaxed {
    static constexpr size_t thread_count = 8;
    static constexpr int iterations_per_thread = 1000000/thread_count;

    padded<std::atomic<int>> ready;
    padded<std::atomic<int>> lock;
    padded<std::atomic<int>> counter;

    void enter_barrier() {
        ++ready;
        while (ready.load() != thread_count) {
            // spin
        }
    }

    void do_lock() {
        int current = 0;
        while (!lock.compare_exchange_weak(current, 1, std::memory_order_relaxed)) {
            asm volatile ("yield");
            current = 0;
        }
    }

    void do_unlock() {
        lock.store(0, std::memory_order_relaxed);
    }

    void do_once() {
        do_lock();
        // When the modification order of counter is considered this exchange
        // is not guaranteed to be before a store in another thread, and by
        // the we unlock it might not happen yet on real hardware (apple m1).
        // So this test exchanges some stale value to 0 and then the final
        // counter does not match.
        int prev = counter.exchange(0, std::memory_order_relaxed);
        counter.store(prev + 1, std::memory_order_relaxed);
        do_unlock();
    }

    void thread_func(size_t thread_index) {
        enter_barrier();
        for (int i = 0; i < iterations_per_thread; ++i) {
            do_once();
        }
    }

    void run() {
        ready.store(0);
        lock.store(0);
        counter.store(0);

        std::vector<std::thread> threads;
        threads.reserve(thread_count);
        for (size_t i = 0; i < thread_count; ++i) {
            threads.emplace_back([this, i]{ thread_func(i); });
        }
        for (auto& thread : threads) {
            thread.join();
        }

        int count = counter.load(std::memory_order_relaxed);
        if (count != iterations_per_thread * thread_count) {
            printf(" got counter=%d (expected %d)...",
                count, iterations_per_thread * thread_count);
            fflush(stdout);
            assert(false && "Iteration count did not match");
        }
    }
};

int main() {
    auto* t = new test_relaxed;
    for (int i = 0; ; ++i) {
        printf("Running iteration %d...", i);
        fflush(stdout);
        auto start = std::chrono::steady_clock::now();
        t->run();
        auto end = std::chrono::steady_clock::now();
        auto elapsed = end - start;
        printf(" done in %dms\n",
            (int)std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());
    }
    delete t;
    return 0;
}
