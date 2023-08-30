#include <atomic>
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
    static constexpr int iteration_count = 1000000;

    struct node_t {
        std::atomic<int> value;

        explicit node_t(int value)
            : value(value)
        {}
    };

    struct thread_stat {
        int increments = 0;
    };

    padded<node_t> node{ 0 };
    padded<std::atomic<node_t*>> head{ &node };

    padded<std::atomic<int>> ready{ 0 };
    padded<thread_stat> thread_stats[thread_count];

    void enter_barrier() {
        ++ready;
        while (ready.load() != thread_count) {
            // spin
        }
    }

    void thread_func(size_t thread_index) {
        enter_barrier();
        for (;;) {
            // note: there is some total order on any atomic variable itself
            // so only one thread can have non-null node at a time
            node_t* node = head.exchange(nullptr, std::memory_order_relaxed);
            if (!node) {
                // keep spinning
                continue;
            }

            int count = node->value.exchange(0, std::memory_order_relaxed);
            if (count < iteration_count) {
                // the question is: could this relaxed store be reordered
                // with the exchange on head below, and could another thread
                // observe some stale value during its value exchange?
                // the real answer (on apple m1) appears to be: yes
                node->value.store(count + 1, std::memory_order_relaxed);
            } else {
                node->value.store(count, std::memory_order_relaxed);
            }

            // note: there is some total order on any atomic variable itself
            head.exchange(node, std::memory_order_relaxed);

            // update stats or stop
            if (count < iteration_count) {
                thread_stats[thread_index].increments++;
            } else {
                break;
            }
        }
    }

    void run() {
        std::vector<std::thread> threads;
        threads.reserve(thread_count);
        for (size_t i = 0; i < thread_count; ++i) {
            threads.emplace_back([this, i]{ thread_func(i); });
        }
        for (auto& thread : threads) {
            thread.join();
        }
        int total = 0;
        for (size_t i = 0; i < thread_count; ++i) {
            printf("thread %d did %d increments\n", int(i), thread_stats[i].increments);
            total += thread_stats[i].increments;
        }
        printf("there have been %d total increments\n", total);
        assert(total == iteration_count);
    }
};

int main() {
    test_relaxed* t = new test_relaxed;
    t->run();
    delete t;
    return 0;
}
