#include <relacy/relacy.hpp>

struct test_relaxed_lock : rl::test_suite<test_relaxed_lock, 2> {
    rl::atomic<int> lock{ 0 };
    rl::atomic<int> counter{ 0 };

    void do_lock() {
        int current = 0;
        while (!lock.compare_exchange_weak(current, 1, rl::memory_order_relaxed)) {
            // spin waiting for the lock
            rl::yield(1, $);
            current = 0;
        }
    }

    void do_unlock() {
        lock.store(0, rl::memory_order_relaxed);
    }

    void thread(unsigned /*thread_index*/) {
        do_lock();
        // When the modification order of counter is considered this exchange
        // is not guaranteed to be before a store in another thread, however
        // by the time we unlock relacy has already stored the new value as
        // the "last value" and exchange loads that instead of some older
        // value. Real hardware behaves in a more relaxed way and store is
        // not always flushed before the exchange.
        int prev = counter.exchange(0, rl::memory_order_relaxed);
        counter.store(prev + 1, rl::memory_order_relaxed);
        do_unlock();
    }

    void after() {
        RL_ASSERT(lock($) == 0);
        RL_ASSERT(counter($) == params::thread_count);
    }
};

int main() {
    rl::test_params params;
    params.search_type = rl::sched_full;
    rl::simulate<test_relaxed_lock>(params);
}
