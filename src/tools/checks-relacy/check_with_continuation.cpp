#include <relacy/relacy.hpp>

// Models the continuation object
struct model_cont {
    enum e_state : int {
        initial = 0,
        resumed = 1,
        destroyed = 2,
    };

    rl::var<int> state{ initial };
    rl::var<int> value{ 0 };
    rl::var<int> token{ 0 };
    rl::var<int> observed_value{ 0 };
    rl::var<int> observed_token{ 0 };

    void set_value(int value) {
        this->value($) = value;
    }

    void set_token(int token) {
        this->token($) = token;
    }

    void resume() {
        RL_ASSERT(state($) == initial);
        state($) = resumed;
        observed_value($) = value($);
        observed_token($) = token($);
    }

    void destroy() {
        RL_ASSERT(state($) == initial);
        state($) = destroyed;
        // We touch values as if running a destructor
        observed_value($) = value($);
        observed_token($) = token($);
    }
};

// Models the state object
struct model_state {
    enum e_state : int {
        initial = 0,
        continuation = 1,
        finished = 2,
        destroyed = 3,
    };

    rl::atomic<int> strong_refs{ 2 };
    rl::atomic<int> state{ initial };
    model_cont* const cont;

    model_state(model_cont* cont)
        : cont(cont)
    {}

    void release_strong_ref() {
        if (strong_refs.fetch_sub(1, rl::memory_order_acq_rel) == 1) {
            destroy_strong();
        }
    }

    void destroy_strong() {
        int current = state.load(rl::memory_order_relaxed);
        while (current != finished) {
            RL_ASSERT(current != destroyed);
            if (state.compare_exchange_weak(current, destroyed,
                    rl::memory_order_acq_rel, rl::memory_order_relaxed))
            {
                if (current == continuation) {
                    cont->destroy();
                }
                break;
            }
        }
    }

    bool ready() const {
        int current = state.load(rl::memory_order_acquire);
        RL_ASSERT(current != continuation);
        return current == finished;
    }

    e_state set_continuation() {
        int current = initial;
        if (state.compare_exchange_strong(current, continuation,
                rl::memory_order_acq_rel))
        {
            return continuation;
        }
        RL_ASSERT(current == finished || current == destroyed);
        return e_state(current);
    }

    void finish() {
        int prev = state.exchange(finished, rl::memory_order_acq_rel);
        RL_ASSERT(prev != finished);
        RL_ASSERT(prev != destroyed);
        if (prev == continuation) {
            cont->resume();
        }
    }
};

struct continuation_test_base {
    model_cont cont;
    model_state state{ &cont };

    void run_produce(int value) {
        cont.set_value(value);
        state.finish();
        state.release_strong_ref();
    }

    void run_produce_failure(int value) {
        cont.set_value(value);
        state.release_strong_ref();
    }

    void run_release_ref() {
        state.release_strong_ref();
    }

    void run_awaiter(int token) {
        // this verifies we correctly transfer between threads
        cont.set_token(token);

        // before we suspend
        if (state.ready()) {
            cont.resume();
            return;
        }

        // suspending
        switch (state.set_continuation()) {
            case model_state::continuation:
                // will resume in another thread
                break;
            case model_state::finished:
                // finished with a value
                cont.resume();
                break;
            case model_state::destroyed:
                // destroyed without a value
                cont.destroy();
                break;
            default:
                RL_ASSERT(false && "Unexpected state");
        }
    }
};

struct continuation_test_with_value
    : rl::test_suite<continuation_test_with_value, 3>
    , continuation_test_base
{
    void thread(unsigned thread_index) {
        switch (thread_index) {
            case 0:
                run_awaiter(51);
                break;
            case 1:
                run_produce(42);
                break;
            case 2:
                run_release_ref();
                break;
        }
    }

    void after() {
        RL_ASSERT(cont.state($) == model_cont::resumed);
        RL_ASSERT(cont.observed_value($) == 42);
        RL_ASSERT(cont.observed_token($) == 51);

        RL_ASSERT(state.strong_refs($) == 0);
        RL_ASSERT(state.state($) == model_state::finished);
    }
};

struct continuation_test_without_value
    : rl::test_suite<continuation_test_without_value, 3>
    , continuation_test_base
{
    void thread(unsigned thread_index) {
        switch (thread_index) {
            case 0:
                run_awaiter(51);
                break;
            case 1:
                run_produce_failure(42);
                break;
            case 2:
                run_release_ref();
                break;
        }
    }

    void after() {
        RL_ASSERT(cont.state($) == model_cont::destroyed);
        RL_ASSERT(cont.observed_value($) == 42);
        RL_ASSERT(cont.observed_token($) == 51);

        RL_ASSERT(state.strong_refs($) == 0);
        RL_ASSERT(state.state($) == model_state::destroyed);
    }
};

int main() {
    rl::test_params params;
    params.iteration_count = 10000000;
    rl::simulate<continuation_test_with_value>(params);
    rl::simulate<continuation_test_without_value>(params);
}
