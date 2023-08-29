#include <relacy/relacy.hpp>

// Models the mailbox object
struct model_mailbox {
    static constexpr uintptr_t MarkerUnlocked = 1;

    struct node {
        rl::atomic<node*> next{ nullptr };
        rl::var<int> item;

        explicit node(int item = 0)
            : item(item)
        {}
    };

    rl::var<node*> head_;
    rl::atomic<node*> tail_;

    model_mailbox() {
        head_($) = new node();
        tail_($) = head_($);
    }

    ~model_mailbox() {
        node* head = head_($);
        RL_ASSERT(head);
        while (head) {
            node* next = head->next.load(rl::memory_order_acquire);
            if (next == reinterpret_cast<node*>(MarkerUnlocked)) {
                next = nullptr;
            }
            delete head;
            head = next;
        }
    }

    bool try_unlock() {
        node* head = head_($);
        RL_ASSERT(head);
        node* next = head->next.load(rl::memory_order_relaxed);
        if (next == nullptr) {
            return head->next.compare_exchange_strong(
                next, reinterpret_cast<node*>(MarkerUnlocked),
                rl::memory_order_release);
        }
        RL_ASSERT(next != reinterpret_cast<node*>(MarkerUnlocked));
        return false;
    }

    bool push(int item) {
        node* next = new node(item);
        // Note: it doesn't fail with mo relaxed here, but I'm not sure why
        // exactly. It could be some complicated sequenced-before relations
        // are enough and exchange on prev->next with acquire/release is all
        // that's needed, but I'm skeptical if maybe it is a relacy bug and
        // it's not weak enough. What I'm worried about is potential reordering
        // of initialization of next (to nullptr) after the exchange on tail_,
        // since they are both relaxed, and then exchange on prev->next taking
        // an uninitialized value instead of nullptr. But I can't make it
        // happen in the test for some reason.
        node* prev = tail_.exchange(next, rl::memory_order_acq_rel);
        node* marker = prev->next.exchange(next, rl::memory_order_acq_rel);
        RL_ASSERT(
            marker == nullptr ||
            marker == reinterpret_cast<node*>(MarkerUnlocked));
        return marker == reinterpret_cast<node*>(MarkerUnlocked);
    }

    int pop() {
        node* head = head_($);
        node* next = head->next.load(rl::memory_order_acquire);
        if (next == nullptr) {
            if (head->next.compare_exchange_strong(
                    next, reinterpret_cast<node*>(MarkerUnlocked),
                    rl::memory_order_acq_rel))
                return 0; // unlocked
            RL_ASSERT(next != nullptr);
        }
        RL_ASSERT(next != reinterpret_cast<node*>(MarkerUnlocked));
        head_($) = next;
        delete head;
        return next->item($);
    }
};

template<
    bool start_locked,
    bool with_try_unlock>
struct test_mailbox
    : public rl::test_suite<test_mailbox<start_locked, with_try_unlock>, 3>
{
    using base = rl::test_suite<test_mailbox<start_locked, with_try_unlock>, 3>;

    model_mailbox mailbox;

    static constexpr int items_per_thread = 3;
    static constexpr int items_total = base::params::thread_count * items_per_thread;

    // note: these are observations and not rl::vars
    bool observed[items_total] = { false };

    void before() {
        if (!start_locked) {
            // need to unlock
            RL_ASSERT(mailbox.try_unlock());
        }
    }

    void run_pop_loop() {
        while (int item = mailbox.pop()) {
            RL_ASSERT(item > 0);
            RL_ASSERT(item <= items_total);
            RL_ASSERT(!observed[item - 1]);
            observed[item - 1] = true;
            if (with_try_unlock && mailbox.try_unlock()) {
                break;
            }
        }
    }

    void thread(unsigned thread_index) {
        if (thread_index == 0 && start_locked) {
            run_pop_loop();
        }
        // each thread pushes n items
        for (int i = 0; i < items_per_thread; ++i) {
            if (mailbox.push(1 + thread_index * items_per_thread + i)) {
                // we now own the mailbox and pop
                run_pop_loop();
                // mailbox is unlocked, continue
            }
        }
    }

    void after() {
        for (int i = 0; i < items_total; ++i) {
            RL_ASSERT(observed[i]);
        }
    }
};

int main() {
    rl::test_params params;
    params.iteration_count = 1000000;
    rl::simulate<test_mailbox<false, false>>(params);
    rl::simulate<test_mailbox<false, true>>(params);
}
