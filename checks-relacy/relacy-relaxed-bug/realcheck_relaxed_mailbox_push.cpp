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

struct test_mailbox_push_relaxed {
    static constexpr size_t thread_count = 8;
    static constexpr int iterations_per_thread = 1000000/thread_count;

    struct node_t {
        padded<std::atomic<node_t*>> next{ reinterpret_cast<node_t*>(1) };
    };

    padded<std::atomic<int>> ready{ 0 };

    padded<std::atomic<node_t*>> tail;

    // note: index 0 is our initial head
    alignas(node_t) char storage[sizeof(node_t) * (1 + thread_count * iterations_per_thread)];

    void enter_barrier() {
        ++ready;
        while (ready.load() != thread_count) {
            // spin
        }
    }

    char* get_ptr(size_t index) {
        return storage + sizeof(node_t) * index;
    }

    node_t* get_node(size_t index) {
        return reinterpret_cast<node_t*>(get_ptr(index));
    }

    char* get_ptr(size_t thread_index, int i) {
        return get_ptr(1 + thread_index * iterations_per_thread + i);
    }

    node_t* allocate(size_t thread_index, int i) {
        return new (get_ptr(thread_index, i)) node_t;
    }

    void thread_func(size_t thread_index) {
        enter_barrier();
        for (int i = 0; i < iterations_per_thread; ++i) {
            node_t* next = allocate(thread_index, i);
            // perform a push operation with a relaxed tail exchange
            // note: tsan correctly identifies the problem when this is not acq_rel
            node_t* prev = tail.exchange(next, std::memory_order_relaxed);
            // note: even though we must have a correct node pointer (there's
            // a total order of atomic operations on tail itself), the prev
            // node's constructor is not supposed to happen-before this load,
            // and could theoretically observe any stale value. But for some
            // reason real hardware (apple m1) doesn't appear to. :-/
            node_t* current = prev->next.load(std::memory_order_relaxed);
            prev->next.store(next, std::memory_order_relaxed);
            // node_t* current = prev->next.exchange(next, std::memory_order_relaxed);
            assert(current == reinterpret_cast<node_t*>(1));
        }
    }

    void visit() {
        node_t* node = get_node(0);
        while (node) {
            // everything is already synchronized and single threaded
            node_t* next = node->next.exchange(reinterpret_cast<node_t*>(2), std::memory_order_relaxed);
            // visited nodes will have next == 2
            assert(next != reinterpret_cast<node_t*>(2));
            // the last node will have next == 1
            if (next == reinterpret_cast<node_t*>(1)) {
                next = nullptr;
            }
            node = next;
        }
    }

    void validate() {
        for (size_t index = 0; index < 1 + thread_count * iterations_per_thread; ++index) {
            node_t* node = get_node(index);
            node_t* next = node->next.load(std::memory_order_relaxed);
            assert(next == reinterpret_cast<node_t*>(2)
                && "Not all nodes are in the linked list");
        }
    }

    void run() {
        ready.store(0);
        // makes sure there are no visited markers from the last run
        memset(storage, 0, sizeof(storage));
        tail.store(new (get_ptr(0)) node_t);

        std::vector<std::thread> threads;
        threads.reserve(thread_count);
        for (size_t i = 0; i < thread_count; ++i) {
            threads.emplace_back([this, i]{ thread_func(i); });
        }
        for (auto& thread : threads) {
            thread.join();
        }
        visit();
        validate();
    }
};

int main() {
    auto* t = new test_mailbox_push_relaxed;
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
