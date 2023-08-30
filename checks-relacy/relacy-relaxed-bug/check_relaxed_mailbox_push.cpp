#include <relacy/relacy.hpp>

struct test_relaxed_mailbox_push : rl::test_suite<test_relaxed_mailbox_push, 2> {
    struct node_t {
        rl::atomic<node_t*> next{ reinterpret_cast<node_t*>(1) };
    };

    rl::var<node_t*> head{ new node_t };
    rl::atomic<node_t*> tail{ head($) };

    void thread(unsigned thread_index) {
        // each thread tries to push a new node
        node_t* next = new node_t;
        node_t* prev = tail.exchange(next, rl::memory_order_relaxed);
        // this load/store is not synchronized and observes an uninitialized variable.
        // node_t* current = prev->next.load(rl::memory_order_relaxed);
        // prev->next.store(next, rl::memory_order_relaxed);
        // however this exchange for some reason does not?
        node_t* current = prev->next.exchange(next, rl::memory_order_relaxed);
        RL_ASSERT(current == reinterpret_cast<node_t*>(1));
    }

    void after() {
        node_t* node = head($);
        while (node) {
            node_t* next = node->next($);
            if (next == reinterpret_cast<node_t*>(1)) {
                next = nullptr;
            }
            delete node;
            node = next;
        }
    }
};

int main() {
    rl::test_params params;
    params.search_type = rl::sched_full;
    rl::simulate<test_relaxed_mailbox_push>(params);
}
