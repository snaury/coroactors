#include <coroactors/actor.h>
#include <coroactors/detach_awaitable.h>
#include <coroactors/detail/blocking_queue.h>
#include <coroactors/detail/intrusive_mailbox.h>
#include <deque>
#include <iostream>
#include <string>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <variant>
#include <random>
#include <sstream>

#if HAVE_ABSEIL
#include <absl/synchronization/mutex.h>
#endif

#if HAVE_ASIO
#include <coroactors/asio_actor_scheduler.h>
#include <asio/thread_pool.hpp>
#endif

using namespace coroactors;

using TClock = std::chrono::steady_clock;
using TTime = std::chrono::time_point<TClock>;

class TPingable {
public:
    actor<int> ping() {
        co_await context();
        int result = ++counter;
        co_return result;
    }

private:
    actor_context context{ actor_scheduler::current() };
    int counter = 0;
};

class TPinger {
public:
    struct TRunResult {
        std::chrono::microseconds max_latency;
    };

    TPinger(TPingable& pingable)
        : pingable(pingable)
    {}

    actor<TRunResult> runWithoutLatencies(int count) {
        co_await context();

        int last = 0;
        for (int i = 0; i < count; ++i) {
            int value = co_await pingable.ping();
            if (value <= last) {
                std::terminate();
            }
            last = value;
        }

        co_return TRunResult{};
    }

    actor<TRunResult> runWithLatencies(int count, TTime start) {
        co_await context();

        TTime end = TClock::now();
        auto maxLatency = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

        int last = 0;
        for (int i = 0; i < count; ++i) {
            TTime call_start = end;
            int value = co_await pingable.ping();
            if (value <= last) {
                std::terminate();
            }
            last = value;
            TTime call_end = TClock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(call_end - call_start);
            maxLatency = std::max(maxLatency, elapsed);
            end = call_end;
        }

        co_return TRunResult{
            maxLatency,
        };
    }

    actor<TRunResult> run(int count, TTime start, bool withLatencies) {
        if (withLatencies) {
            return runWithLatencies(count, start);
        } else {
            return runWithoutLatencies(count);
        }
    }

private:
    actor_context context{ actor_scheduler::current() };
    TPingable& pingable;
};

template<class T>
class TBlockingQueueWrapper : public detail::blocking_queue<T> {
public:
    void shutdown(size_t threads) {
        for (size_t i = 0; i < threads; ++i) {
            this->push(T());
        }
    }

    std::string stats() {
        return {};
    }
};

template<class T>
class TBlockingQueueWithStdMutex {
public:
    template<class... TArgs>
    void push(TArgs&&... args) {
        std::unique_lock l(Lock);
        Items.emplace_back(std::forward<TArgs>(args)...);
        NotEmpty.notify_one();
    }

    T pop() {
        std::unique_lock l(Lock);
        if (Items.empty() && !Shutdown) {
            ++Waiters;
            do {
                NotEmpty.wait(l);
            } while (Items.empty() && !Shutdown);
            --Waiters;
        }
        if (!Items.empty()) {
            T item(std::move(Items.front()));
            Items.pop_front();
            return item;
        }
        assert(Shutdown);
        return T();
    }

    T try_pop() {
        std::unique_lock l(Lock);
        if (!Items.empty()) {
            T item(std::move(Items.front()));
            Items.pop_front();
            return item;
        }
        return T();
    }

    void shutdown(size_t) {
        std::unique_lock l(Lock);
        Shutdown = true;
        NotEmpty.notify_all();
    }

    std::string stats() {
        return {};
    }

private:
    std::mutex Lock;
    std::condition_variable NotEmpty;
    std::deque<T> Items;
    size_t Waiters = 0;
    bool Shutdown = false;
};

#if HAVE_ABSEIL
template<class T>
class TBlockingQueueWithAbslMutex {
public:
    template<class... TArgs>
    void push(TArgs&&... args) {
        absl::MutexLock l(&Lock);
        Items.emplace_back(std::forward<TArgs>(args)...);
    }

    T pop() {
        absl::MutexLock l(&Lock, absl::Condition(this, &TBlockingQueueWithAbslMutex::NotEmpty));
        if (!Items.empty()) {
            T item(std::move(Items.front()));
            Items.pop_front();
            return item;
        }
        assert(Shutdown);
        return T();
    }

    T try_pop() {
        absl::MutexLock l(&Lock);
        if (!Items.empty()) {
            T item(std::move(Items.front()));
            Items.pop_front();
            return item;
        }
        return T();
    }

    void shutdown(size_t) {
        absl::MutexLock l(&Lock);
        Shutdown = true;
    }

    std::string stats() {
        return {};
    }

private:
    bool NotEmpty() const {
        return !Items.empty() || Shutdown;
    }

private:
    absl::Mutex Lock;
    std::deque<T> Items;
    bool Shutdown = false;
};
#endif

template<class T>
class TBlockingQueueWithStdMailbox;

template<class T>
class TBlockingQueueWithStdMailbox<T*> {
    using TMailbox = detail::intrusive_mailbox<T>;

public:
    TBlockingQueueWithStdMailbox()
        : Mailbox(TMailbox::initially_unlocked)
    {}

    template<class... TArgs>
    void push(TArgs&&... args) {
        // Most of the time this will be lockfree
        if (Mailbox.push(std::forward<TArgs>(args)...)) {
            push_locks.fetch_add(1, std::memory_order_relaxed);
            std::unique_lock l(Lock);
            MailboxLocked = true;
            if (Waiters > 0 && !Notified) {
                // Wake a single waiter, others should daisy chain
                push_wakeups.fetch_add(1, std::memory_order_relaxed);
                CanPop.notify_one();
                Notified = true;
            }
        }
    }

    T* pop() {
        std::unique_lock l(Lock);
        for (;;) {
            while (!MailboxLocked && !Shutdown) {
                ++Waiters;
                CanPop.wait(l);
                --Waiters;
                // Since we just returned from wait, assume notify_one was meant
                // for us and consume it. This would make sure another thread is
                // notified when there's more work or after we go back to wait.
                Notified = false;
            }

            if (MailboxLocked) {
                // Pessimistic: unlock mailbox when still no work
                if (auto* result = Mailbox.pop()) {
                    if (Waiters > 0 && !Notified) {
                        // Mailbox still locked, wake one more waiter
                        pop_wakeups.fetch_add(1, std::memory_order_relaxed);
                        CanPop.notify_one();
                        Notified = true;
                    }
                    return result;
                }

                // Mailbox was unlocked, now wait
                MailboxLocked = false;
            }

            if (Shutdown) {
                return nullptr;
            }
        }
    }

    T* try_pop() {
        std::unique_lock l(Lock);
        if (MailboxLocked) {
            if (auto* result = Mailbox.try_pop()) {
                if (Waiters > 0 && !Notified) {
                    CanPop.notify_one();
                    Notified = true;
                }
                return result;
            }
            // Mailbox is still locked
        }
        return nullptr;
    }

    void shutdown(size_t) {
        std::unique_lock l(Lock);
        Shutdown = true;
        CanPop.notify_all();
    }

    std::string stats() {
        std::stringstream s;
        s << "push_locks=" << push_locks.exchange(0, std::memory_order_relaxed);
        s << " push_wakeups=" << push_wakeups.exchange(0, std::memory_order_relaxed);
        s << " pop_wakeups=" << pop_wakeups.exchange(0, std::memory_order_relaxed);
        return std::move(s).str();
    }

public:
    std::mutex Lock;
    std::condition_variable CanPop;
    TMailbox Mailbox;
    std::atomic<uint32_t> push_locks{ 0 };
    std::atomic<uint32_t> push_wakeups{ 0 };
    std::atomic<uint32_t> pop_wakeups{ 0 };
    size_t Waiters = 0;
    bool Notified = false;
    bool MailboxLocked = false;
    bool Shutdown = false;
};

#if HAVE_ABSEIL
template<class T>
class TBlockingQueueWithAbslMailbox;

template<class T>
class TBlockingQueueWithAbslMailbox<T*> {
    using TMailbox = detail::intrusive_mailbox<T>;

public:
    TBlockingQueueWithAbslMailbox()
        : Mailbox(TMailbox::initially_unlocked)
    {}

    template<class... TArgs>
    void push(TArgs&&... args) {
        // Most of the time this will be lockfree
        if (Mailbox.push(std::forward<TArgs>(args)...)) {
            push_locks.fetch_add(1, std::memory_order_relaxed);
            absl::MutexLock l(&Lock);
            MailboxLocked = true;
        }
    }

    T* pop() {
        for (;;) {
            absl::MutexLock l(&Lock, absl::Condition(this, &TBlockingQueueWithAbslMailbox::CanPop));
            if (MailboxLocked) {
                if (auto* result = Mailbox.pop()) {
                    return result;
                }
                // Mailbox was unlocked, now wait
                pop_unlocks.fetch_add(1, std::memory_order_relaxed);
                MailboxLocked = false;
            }
            if (Shutdown) {
                return nullptr;
            }
        }
    }

    T* try_pop() {
        absl::MutexLock l(&Lock);
        if (MailboxLocked) {
            if (auto* result = Mailbox.try_pop()) {
                return result;
            }
        }
        return nullptr;
    }

    void shutdown(size_t) {
        absl::MutexLock l(&Lock);
        Shutdown = true;
    }

    std::string stats() {
        std::stringstream s;
        s << "push_locks=" << push_locks.exchange(0, std::memory_order_relaxed);
        s << " pop_unlocks=" << pop_unlocks.exchange(0, std::memory_order_relaxed);
        return std::move(s).str();
    }

private:
    bool CanPop() const {
        return MailboxLocked || Shutdown;
    }

public:
    absl::Mutex Lock;
    TMailbox Mailbox;
    std::atomic<uint32_t> push_locks{ 0 };
    std::atomic<uint32_t> pop_unlocks{ 0 };
    bool MailboxLocked = false;
    bool Shutdown = false;
};
#endif

enum class ESchedulerType {
    LockFree,
    StdMutex,
    StdMailbox,
#if HAVE_ABSEIL
    AbslMutex,
    AbslMailbox,
#endif
#if HAVE_ASIO
    Asio,
#endif
#if HAVE_ABSEIL
    WorkStealing,
#endif
};

class actor_scheduler_stats {
public:
    virtual std::string stats() = 0;
};

template<template <class> typename BlockingQueue>
class BlockingQueueScheduler
    : public actor_scheduler
    , public actor_scheduler_stats
{
public:
    BlockingQueueScheduler(size_t threads, std::chrono::microseconds preemptUs)
        : PreemptUs(preemptUs)
    {
        for (size_t i = 0; i < threads; ++i) {
            Threads.emplace_back([this]{
                this->RunWorker();
            });
        }
    }

    ~BlockingQueueScheduler() {
        Queue.shutdown(Threads.size());

        for (auto& thread : Threads) {
            thread.join();
        }

        if (auto c = Queue.try_pop()) {
            assert(false && "Unexpected scheduler shutdown with non-empty queue");
        }
    }

    void post(actor_scheduler_runnable* r) override {
        assert(r && "Cannot schedule an empty runnable");
        Queue.push(r);
    }

    bool preempt() override {
        if (thread_deadline) {
            return TClock::now() >= *thread_deadline;
        }
        // Don't allow monopolization of non-worker threads
        return true;
    }

    std::string stats() override {
        return Queue.stats();
    }

private:
    void RunWorker() {
        actor_scheduler::set_current_ptr(this);
        TTime deadline{};
        thread_deadline = &deadline;
        while (auto* r = Queue.pop()) {
            deadline = TClock::now() + PreemptUs;
            r->run();
        }
        thread_deadline = nullptr;
    }

private:
    std::chrono::microseconds PreemptUs;
    BlockingQueue<actor_scheduler_runnable*> Queue;
    std::vector<std::thread> Threads;

    static inline thread_local const TTime* thread_deadline{ nullptr };
};

#if HAVE_ASIO
class AsioScheduler
    : private asio::thread_pool
    , public asio_actor_scheduler
    , public actor_scheduler_stats
{
public:
    AsioScheduler(size_t threads, std::chrono::microseconds preemptUs)
        : asio::thread_pool(threads)
        , asio_actor_scheduler(this->get_executor(), preemptUs)
    {}

    std::string stats() {
        return {};
    }
};
#endif

#if HAVE_ABSEIL
class WorkStealingScheduler
    : public actor_scheduler
    , public actor_scheduler_stats
{
private:
    using mailbox_t = detail::intrusive_mailbox<actor_scheduler_runnable>;

    static constexpr size_t max_local_tasks = 256;
    static constexpr uint32_t task_index_mask = 255;

    struct thread_state {
        WorkStealingScheduler* scheduler;
        size_t thread_count;

        // The only reason these are atomic is to make it possible to atomically
        // read them while racing with other threads. All operations on these
        // atomics are relaxed.
        std::atomic<actor_scheduler_runnable*> local_queue[max_local_tasks];
        // Packed head and tail indexes to simplify consistent load and cas
        std::atomic<uint64_t> local_queue_head_tail;
        // The number of processed local tasks
        size_t local_processed = 0;
        // Random state for stealing
        std::mt19937 random_;

        TTime preempt_deadline{};

        explicit thread_state(WorkStealingScheduler* self, size_t thread_count)
            : scheduler(self)
            , thread_count(thread_count)
        {}
    };

public:
    WorkStealingScheduler(size_t thread_count, std::chrono::microseconds preempt_us)
        : preempt_us_(preempt_us)
        , global_queue_(mailbox_t::initially_unlocked)
    {
        for (size_t i = 0; i < thread_count; ++i) {
            thread_states_.emplace_back(this, thread_count);
        }
        for (size_t i = 0; i < thread_count; ++i) {
            threads_.emplace_back([this, state = &thread_states_[i]] {
                this->run_worker(state);
            });
        }
    }

    ~WorkStealingScheduler() {
        {
            absl::MutexLock l(&lock_);
            shutdown_ = true;
        }

        for (auto& thread : threads_) {
            thread.join();
        }
    }

    void post(actor_scheduler_runnable* r) override {
        auto* state = local_state;
        if (state && state->scheduler == this) [[likely]] {
            if (push_to_local(state, r)) {
                // Other threads may want to steal our tasks
                wake_by_local_tasks();
                return;
            }
        }
        push_to_global(r, /* wake */ true);
    }

    void defer(actor_scheduler_runnable* r) override {
        auto* state = local_state;
        if (state && state->scheduler == this) [[likely]] {
            if (push_to_local(state, r)) {
                // No new threads of execution, no need to wake up
                return;
            }
            push_to_global(r, /* wake */ false);
        } else {
            push_to_global(r, /* wake */ true);
        }
    }

    bool preempt() override {
        auto* state = local_state;
        if (state && state->scheduler == this) [[likely]] {
            return TClock::now() >= state->preempt_deadline;
        }
        // Preempt all unexpected threads
        return true;
    }

private:
    void init_seed(thread_state* state) {
        auto now = std::chrono::high_resolution_clock::now();
        auto a = now.time_since_epoch().count();
        auto b = std::this_thread::get_id();
        auto c = std::hash<decltype(b)>()(b);
        state->random_.seed(a + c);
    }

    void run_worker(thread_state* state) noexcept {
        init_seed(state);

        local_state = state;
        while (auto* r = next_task(state)) {
            state->preempt_deadline = TClock::now() + preempt_us_;
            r->run();
        }
        local_state = nullptr;
    }

    actor_scheduler_runnable* next_task(thread_state* state) {
        // Check global queue periodically
        if (state->local_processed >= 61 /*state->thread_count*/) {
            state->local_processed = 0;
            if (auto* r = pop_global_task(state)) {
                return r;
            }
        }

        if (auto* r = pop_from_local(state)) {
            state->local_processed++;
            return r;
        }

        // Try stealing from other threads
        if (auto* r = try_stealing(state)) {
            state->local_processed++;
            return r;
        }

        // We couldn't find anything, block waiting for various wakeup conditions
        actor_scheduler_runnable* r;
        blocked_threads_.fetch_add(1, std::memory_order_acquire);
        for (int attempt = 0; ; ++attempt) {
            inc(stats_mutex_locks);
            // Note: we lock unconditionally in the first iteration, to make
            // sure we don't miss local tasks that have been added without
            // signalling before blocked threads counter was incremented.
            absl::MutexLock l(&lock_, attempt == 0 ? absl::Condition::kTrue : absl::Condition(this, &WorkStealingScheduler::must_wakeup));

            if (global_queue_locked_.load(std::memory_order_relaxed)) {
                if ((r = global_queue_.pop())) {
                    if (global_queue_.try_unlock()) {
                        global_queue_locked_.store(false, std::memory_order_relaxed);
                    }
                    state->local_processed = 0;
                    break;
                }
                global_queue_locked_.store(false, std::memory_order_relaxed);
            }

            if (attempt == 0 || may_have_local_tasks_) {
                if ((r = try_stealing(state))) {
                    state->local_processed++;
                    break;
                }
                may_have_local_tasks_ = false;
            }

            if (shutdown_) {
                r = nullptr;
                break;
            }
        }
        blocked_threads_.fetch_sub(1, std::memory_order_release);
        return r;
    }

    bool must_wakeup() const {
        return global_queue_locked_.load(std::memory_order_relaxed) ||
            may_have_local_tasks_ ||
            shutdown_;
    }

    actor_scheduler_runnable* pop_global_task(thread_state* state) {
        // Don't steal global tasks from blocked threads
        if (blocked_threads_.fetch_add(0, std::memory_order_relaxed) > 0) {
            return nullptr;
        }

        // Don't lock the mutex when the global queue is likely empty
        if (!global_queue_locked_.load(std::memory_order_relaxed)) {
            return nullptr;
        }

        inc(stats_mutex_locks);
        absl::MutexLock l(&lock_);

        if (!global_queue_locked_.load(std::memory_order_relaxed)) {
            return nullptr;
        }

        auto* r = global_queue_.pop();
        if (!r || global_queue_.try_unlock()) {
            global_queue_locked_.store(false, std::memory_order_relaxed);
        }

        if (r && local_queue_size(state) > 0) {
            // We have removed a task from global queue, but we have some local
            // tasks. Avoid a situation where we have threads equal to the
            // number of tasks, but not all threads are working. Example:
            // - thread 1 has 1 local task, but it checks global queue and takes 1 more task
            // - thread 2 has empty local queue, takes the last global task and unlocks
            // - thread 3 has nothing to execute and blocks without wake up conditions
            // Make sure other threads know there is potentially more work in
            // local queues, so they try to steal tasks when they block.
            may_have_local_tasks_ = true;
        }

        return r;
    }

    actor_scheduler_runnable* try_stealing(thread_state* state) {
        inc(stats_steal_attempts);
        uint32_t pos = state->random_();
        for (size_t i = 0; i < state->thread_count; ++i, pos += 1) {
            thread_state* from = &thread_states_[pos % state->thread_count];
            if (state != from) {
                if (auto* r = steal_from_local(state, from)) {
                    return r;
                }
            }
        }
        return nullptr;
    }

private:
    static uint64_t pack_head_tail(uint32_t head, uint32_t tail) {
        return (uint64_t(head) << 32) | tail;
    }

    static std::tuple<uint32_t, uint32_t> unpack_head_tail(uint64_t value) {
        return { uint32_t(value >> 32), uint32_t(value) };
    }

    actor_scheduler_runnable* pop_from_local(thread_state* state) {
        auto head_tail = state->local_queue_head_tail.load(std::memory_order_relaxed);
        for (;;) {
            auto [head, tail] = unpack_head_tail(head_tail);
            if (head == tail) {
                return nullptr;
            }
            actor_scheduler_runnable* r = state->local_queue[head & task_index_mask].load(std::memory_order_relaxed);
            // Note: we don't need any synchronization here, because only the
            // current thread ever writes to the local queue and reordering
            // should be ok.
            if (state->local_queue_head_tail.compare_exchange_strong(head_tail,
                    pack_head_tail(head + 1, tail), std::memory_order_relaxed))
            {
                return r;
            }
            // Lost the race with a stealer, head_tail is reloaded
            inc(stats_pop_local_cas_fail);
        }
    }

    bool push_to_local(thread_state* state, actor_scheduler_runnable* r) {
        // Note: tail is only modified locally, no synchronization needed
        // And while head may be updated by a stealer, we only use it for
        // determining if there's enough capacity, nothing else.
        auto head_tail = state->local_queue_head_tail.load(std::memory_order_relaxed);
        auto [head, tail] = unpack_head_tail(head_tail);
        if (uint32_t(tail - head) >= max_local_tasks) {
            return false;
        }
        state->local_queue[tail & task_index_mask].store(r, std::memory_order_relaxed);
        // We want to increment tail without changing head as a single increment
        // This computes a wrapping difference that will make tail = tail + 1
        // even when other threads are modifying head concurrently.
        uint64_t increment = pack_head_tail(0, tail + 1) - pack_head_tail(0, tail);
        // Note: release synchronizes with other threads stealing tasks
        state->local_queue_head_tail.fetch_add(increment, std::memory_order_release);
        return true;
    }

    uint32_t local_queue_size(thread_state* state) {
        auto head_tail = state->local_queue_head_tail.load(std::memory_order_relaxed);
        auto [head, tail] = unpack_head_tail(head_tail);
        return tail - head;
    }

    actor_scheduler_runnable* steal_from_local(thread_state* state, thread_state* from) {
        auto our_head_tail = state->local_queue_head_tail.load(std::memory_order_relaxed);
        auto [our_head, our_tail] = unpack_head_tail(our_head_tail);
        assert(our_head == our_tail);
        // Note: acquire synchronizes with push_to_local
        auto their_head_tail = from->local_queue_head_tail.load(std::memory_order_acquire);
        for (;;) {
            auto [their_head, their_tail] = unpack_head_tail(their_head_tail);
            uint32_t n = their_tail - their_head;
            if (n == 0) {
                return nullptr;
            }
            n -= n >> 1;
            // Copy pointer values, the first task is not copied
            auto* r = from->local_queue[their_head & task_index_mask].load(std::memory_order_relaxed);
            for (uint32_t i = 1; i < n; ++i) {
                state->local_queue[(our_tail + i - 1) & task_index_mask].store(
                    from->local_queue[(their_head + i) & task_index_mask].load(std::memory_order_relaxed),
                    std::memory_order_relaxed);
            }
            // Note: acquire needed in case of failures
            // Note: release needed so loads above are not reordered with cas
            if (from->local_queue_head_tail.compare_exchange_strong(their_head_tail,
                    pack_head_tail(their_head + n, their_tail), std::memory_order_acq_rel))
            {
                // We successfully stole some tasks
                if (n > 1) {
                    // Note: we can use a store, because local queue is empty
                    // and no concurrent stealer could have changed head/tail
                    // Note: release synchronizes with other threads stealing our tasks
                    state->local_queue_head_tail.store(
                        pack_head_tail(our_head, our_tail + n - 1),
                        std::memory_order_release);
                }
                return r;
            }
            // Lost the race: will retry with updated their_head_tail
            inc(stats_steal_cas_fail);
        }
    }

    void push_to_global(actor_scheduler_runnable* r, bool wake) {
        (void)wake;
        if (global_queue_.push(r)) {
            // We have locked the queue
            inc(stats_mutex_locks);
            absl::MutexLock l(&lock_);
            global_queue_locked_.store(true, std::memory_order_relaxed);
            if (blocked_threads_.fetch_add(0, std::memory_order_relaxed) > 0) {
                // This will wake up at least one thread
                inc(stats_wakeups);
            }
        }
    }

    void wake_by_local_tasks() {
        if (blocked_threads_.fetch_add(0, std::memory_order_relaxed) > 0) {
            inc(stats_mutex_locks);
            absl::MutexLock l(&lock_);
            may_have_local_tasks_ = true;
            inc(stats_wakeups);
        }
    }

public:
    std::string stats() override {
        std::stringstream s;
        s << "wakeups=" << take(stats_wakeups);
        s << " mutex_locks=" << take(stats_mutex_locks);
        s << " steal_attempts=" << take(stats_steal_attempts);
        s << " pop_local_cas_fails=" << take(stats_pop_local_cas_fail);
        s << " steal_cas_fail=" << take(stats_steal_cas_fail);
        s << " blocked=" << blocked_threads_.load(std::memory_order_relaxed);
        return std::move(s).str();
    }

private:
    template<class T>
    static void inc(std::atomic<T>& v) {
        v.fetch_add(1, std::memory_order_relaxed);
    }

    template<class T>
    static T take(std::atomic<T>& v) {
        return v.exchange(T{}, std::memory_order_relaxed);
    }

private:
    std::chrono::microseconds preempt_us_;
    std::deque<thread_state> thread_states_;
    std::deque<std::thread> threads_;

    std::atomic<uint32_t> blocked_threads_{ 0 };
    bool may_have_local_tasks_ = false;
    bool shutdown_ = false;

    absl::Mutex lock_;
    std::atomic<bool> global_queue_locked_{ false };
    mailbox_t global_queue_;

    std::atomic<uint32_t> stats_wakeups{ 0 };
    std::atomic<uint32_t> stats_mutex_locks{ 0 };
    std::atomic<uint32_t> stats_steal_attempts{ 0 };
    std::atomic<uint32_t> stats_pop_local_cas_fail{ 0 };
    std::atomic<uint32_t> stats_steal_cas_fail{ 0 };

private:
    static inline thread_local thread_state* local_state{ nullptr };
};
#endif

std::shared_ptr<actor_scheduler> create_scheduler(ESchedulerType type,
        size_t threads, std::chrono::microseconds preemptUs)
{
    switch (type) {
    case ESchedulerType::LockFree:
        return std::make_shared<BlockingQueueScheduler<TBlockingQueueWrapper>>(threads, preemptUs);
    case ESchedulerType::StdMutex:
        return std::make_shared<BlockingQueueScheduler<TBlockingQueueWithStdMutex>>(threads, preemptUs);
    case ESchedulerType::StdMailbox:
        return std::make_shared<BlockingQueueScheduler<TBlockingQueueWithStdMailbox>>(threads, preemptUs);
#if HAVE_ABSEIL
    case ESchedulerType::AbslMutex:
        return std::make_shared<BlockingQueueScheduler<TBlockingQueueWithAbslMutex>>(threads, preemptUs);
    case ESchedulerType::AbslMailbox:
        return std::make_shared<BlockingQueueScheduler<TBlockingQueueWithAbslMailbox>>(threads, preemptUs);
#endif
#if HAVE_ASIO
    case ESchedulerType::Asio:
        return std::make_shared<AsioScheduler>(threads, preemptUs);
#endif
    case ESchedulerType::WorkStealing:
        return std::make_shared<WorkStealingScheduler>(threads, preemptUs);
    default:
        throw std::runtime_error("unsupported scheduler type");
    }
}

class alignas(128) SchedulerThroughputTask final
    : public actor_scheduler_runnable
{
public:
    SchedulerThroughputTask(actor_scheduler& scheduler)
        : scheduler(scheduler)
    {}

    void run() noexcept {
        count_.fetch_add(1, std::memory_order_relaxed);
        scheduler.defer(this);
    }

    uint64_t getCount() noexcept {
        return count_.exchange(0, std::memory_order_relaxed);
    }

private:
    actor_scheduler& scheduler;
    std::atomic<uint64_t> count_{ 0 };
};

template<class T>
    requires (!std::is_void_v<T>)
std::vector<T> run_sync(std::vector<actor<T>> actors) {
    detail::sync_wait_group wg(actors.size());
    std::vector<T> results(actors.size());

    for (size_t i = 0; i < actors.size(); ++i) {
        detach_awaitable(std::move(actors[i]), [&wg, &results, i](T&& result){
            results[i] = std::move(result);
            wg.done();
        });
    }
    actors.clear();

    wg.wait();

    return results;
}

void run_sync(std::vector<actor<void>> actors) {
    detail::sync_wait_group wg(actors.size());

    for (auto& a : actors) {
        detach_awaitable(std::move(a), [&]{
            wg.done();
        });
    }
    actors.clear();

    wg.wait();
}

template<class T>
    requires (!std::is_void_v<T>)
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
    std::chrono::microseconds preemptUs(10);
    ESchedulerType schedulerType = ESchedulerType::LockFree;
    bool withLatencies = true;
    bool schedulerStats = false;
    bool schedulerThroughput = false;

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
        if ((arg == "--preempt-us") && i + 1 < argc) {
            preemptUs = std::chrono::microseconds(std::stoi(argv[++i]));
            continue;
        }
        if (arg == "--use-lockfree") {
            schedulerType = ESchedulerType::LockFree;
            continue;
        }
        if (arg == "--use-std-mutex") {
            schedulerType = ESchedulerType::StdMutex;
            continue;
        }
        if (arg == "--use-std-mailbox") {
            schedulerType = ESchedulerType::StdMailbox;
            continue;
        }
#if HAVE_ABSEIL
        if (arg == "--use-absl-mutex") {
            schedulerType = ESchedulerType::AbslMutex;
            continue;
        }
        if (arg == "--use-absl-mailbox") {
            schedulerType = ESchedulerType::AbslMailbox;
            continue;
        }
#endif
#if HAVE_ASIO
        if (arg == "--use-asio") {
            schedulerType = ESchedulerType::Asio;
            continue;
        }
#endif
#if HAVE_ABSEIL
        if (arg == "--use-work-stealing") {
            schedulerType = ESchedulerType::WorkStealing;
            continue;
        }
#endif
        if (arg == "--without-latencies") {
            withLatencies = false;
            continue;
        }
        if (arg == "--scheduler-stats") {
            schedulerStats = true;
            continue;
        }
        if (arg == "--scheduler-throughput") {
            schedulerThroughput = true;
            continue;
        }
        std::cerr << "ERROR: unexpected argument: " << argv[i] << std::endl;
        return 1;
    }

    auto scheduler = create_scheduler(schedulerType, numThreads, preemptUs);
    actor_scheduler::set_current_ptr(scheduler.get());

    if (schedulerThroughput) {
        std::deque<SchedulerThroughputTask> tasks;
        for (int i = 0; i < numPingers; ++i) {
            tasks.emplace_back(*scheduler);
        }
        for (auto& task : tasks) {
            scheduler->post(&task);
        }
        std::cout << "Started " << numPingers << " tasks..." << std::endl;
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            uint64_t sum = 0, min = 0, max = 0;
            for (int i = 0; i < numPingers; ++i) {
                auto count = tasks[i].getCount();
                if (i == 0) {
                    sum = count;
                    min = count;
                    max = count;
                } else {
                    sum += count;
                    min = std::min(min, count);
                    max = std::max(max, count);
                }
            }
            std::cout << "... " << sum << "/s (min=" << min << "/s, max=" << max << "/s)";
            if (schedulerStats) {
                if (auto* s = dynamic_cast<actor_scheduler_stats*>(scheduler.get())) {
                    std::cout << " " << s->stats();
                }
            }
            std::cout << std::endl;
        }
    }

    std::deque<TPingable> pingables;
    std::deque<TPinger> pingers;

    for (int i = 0; i < numPingables; ++i) {
        pingables.emplace_back();
    }
    for (int i = 0; i < numPingers; ++i) {
        pingers.emplace_back(pingables[i % pingables.size()]);
    }

    std::cout << "Warming up..." << std::endl;
    run_sync(pingers[0].run(count / numPingers / 100, TClock::now(), withLatencies));

    std::cout << "Starting..." << std::endl;
    std::vector<actor<TPinger::TRunResult>> runs;
    auto start = TClock::now();
    for (auto& pinger : pingers) {
        runs.push_back(pinger.run(count / numPingers, start, withLatencies));
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

    if (schedulerStats) {
        if (auto* s = dynamic_cast<actor_scheduler_stats*>(scheduler.get())) {
            std::cout << "Scheduler stats: " << s->stats() << std::endl;
        }
    }

    return 0;
}
