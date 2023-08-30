#include <relacy/relacy.hpp>

struct test_relaxed : rl::test_suite<test_relaxed, 2> {
    struct node_t {
        rl::atomic<int> value{ 1 };
    };

    rl::atomic<node_t*> head{ nullptr };

    void thread(unsigned thread_index) {
        switch (thread_index) {
            case 0: {
                // allocate a new node and publish it
                node_t* node = new node_t;
                node_t* prev = head.exchange(node, rl::memory_order_relaxed);
                RL_ASSERT(prev == nullptr);
                break;
            }
            case 1: {
                // remove the node and check it
                node_t* node = head.exchange(nullptr, rl::memory_order_relaxed);
                if (node) {
                    // this is supposed to fail sometimes, but it doesn't?
                    int prev = node->value.exchange(2, rl::memory_order_relaxed);
                    RL_ASSERT(prev == 1);
                    delete node;
                }
                break;
            }
        }
    }

    void after() {
        if (node_t* node = head($)) {
            delete node;
        }
    }
};

int main() {
    rl::test_params params;
    params.search_type = rl::sched_full;
    rl::simulate<test_relaxed>(params);
}
