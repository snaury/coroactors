#include <coroactors/actor.h>
#include <coroactors/detach_awaitable.h>
#include <coroactors/detail/blocking_queue.h>
#include <deque>
#include <iostream>
#include <string>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <variant>

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
        if (Items.empty()) {
            ++Waiters;
            do {
                NotEmpty.wait(l);
            } while (Items.empty());
            --Waiters;
        }
        T item(std::move(Items.front()));
        Items.pop_front();
        return item;
    }

    std::optional<T> try_pop() {
        std::unique_lock l(Lock);
        if (!Items.empty()) {
            std::optional<T> item(std::move(Items.front()));
            Items.pop_front();
            return item;
        }
        return std::nullopt;
    }

private:
    std::mutex Lock;
    std::condition_variable NotEmpty;
    std::deque<T> Items;
    size_t Waiters = 0;
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
        absl::MutexLock l(&Lock, absl::Condition(this, &TBlockingQueueWithAbslMutex::HasItems));
        T item(std::move(Items.front()));
        Items.pop_front();
        return item;
    }

    std::optional<T> try_pop() {
        absl::MutexLock l(&Lock);
        if (!Items.empty()) {
            std::optional<T> item(std::move(Items.front()));
            Items.pop_front();
            return item;
        }
        return std::nullopt;
    }

private:
    bool HasItems() const {
        return !Items.empty();
    }

private:
    absl::Mutex Lock;
    std::deque<T> Items;
};
#endif

static std::atomic<size_t> mailbox_wakeups{ 0 };

template<class T>
class TBlockingQueueWithStdMailbox {
public:
    TBlockingQueueWithStdMailbox() {
        MailboxLocked = !Mailbox.try_unlock();
    }

    template<class... TArgs>
    void push(TArgs&&... args) {
        // Most of the time this will be lockfree
        if (Mailbox.push(std::forward<TArgs>(args)...)) {
            mailbox_wakeups.fetch_add(1, std::memory_order_relaxed);
            std::unique_lock l(Lock);
            MailboxLocked = true;
            if (Waiters > 0) {
                // Wake a single waiter, others should daisy chain
                CanPop.notify_one();
            }
        }
    }

    T pop() {
        std::unique_lock l(Lock);
        for (;;) {
            while (!MailboxLocked) {
                ++Waiters;
                CanPop.wait(l);
                --Waiters;
            }
            if (auto result = Mailbox.pop_optional()) {
                if (Waiters > 0) {
                    // Mailbox still locked, wake one more waiter
                    CanPop.notify_one();
                }
                return std::move(*result);
            }
            // Mailbox was unlocked, now wait
            MailboxLocked = false;
        }
    }

    std::optional<T> try_pop() {
        std::unique_lock l(Lock);
        if (MailboxLocked) {
            if (auto result = Mailbox.pop_optional()) {
                if (Waiters > 0) {
                    CanPop.notify_one();
                }
                return std::move(*result);
            }
            MailboxLocked = false;
        }
        return std::nullopt;
    }

public:
    std::mutex Lock;
    std::condition_variable CanPop;
    detail::mailbox<T> Mailbox;
    size_t Waiters = 0;
    bool MailboxLocked;
};

#if HAVE_ABSEIL
template<class T>
class TBlockingQueueWithAbslMailbox {
public:
    TBlockingQueueWithAbslMailbox() {
        MailboxLocked = !Mailbox.try_unlock();
    }

    template<class... TArgs>
    void push(TArgs&&... args) {
        // Most of the time this will be lockfree
        if (Mailbox.push(std::forward<TArgs>(args)...)) {
            mailbox_wakeups.fetch_add(1, std::memory_order_relaxed);
            absl::MutexLock l(&Lock);
            MailboxLocked = true;
        }
    }

    T pop() {
        for (;;) {
            absl::MutexLock l(&Lock, absl::Condition(this, &TBlockingQueueWithAbslMailbox::CanPop));
            if (auto result = Mailbox.pop_optional()) {
                return std::move(*result);
            }
            // Mailbox was unlocked, now wait
            MailboxLocked = false;
        }
    }

    std::optional<T> try_pop() {
        absl::MutexLock l(&Lock);
        if (MailboxLocked) {
            if (auto result = Mailbox.pop_optional()) {
                return std::move(*result);
            }
            // Mailbox was unlocked, new ops will wait
            MailboxLocked = false;
        }
        return std::nullopt;
    }

private:
    bool CanPop() const {
        return MailboxLocked;
    }

public:
    absl::Mutex Lock;
    detail::mailbox<T> Mailbox;
    bool MailboxLocked;
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
};

template<template <class> typename BlockingQueue>
class BlockingQueueScheduler : public actor_scheduler {
public:
    using execute_callback_type = actor_scheduler::execute_callback_type;

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
        // Send a stop signal for every thread
        for (size_t i = 0; i < Threads.size(); ++i) {
            Queue.push(execute_callback_type());
        }
        for (auto& thread : Threads) {
            thread.join();
        }
        if (auto c = Queue.try_pop()) {
            assert(false && "Unexpected scheduler shutdown with non-empty queue");
        }
    }

    void post(execute_callback_type c) override {
        assert(c && "Cannot schedule an empty callback");
        Queue.push(std::move(c));
    }

    bool preempt() override {
        if (thread_deadline) {
            return TClock::now() >= *thread_deadline;
        }
        // Don't allow monopolization of non-worker threads
        return true;
    }

private:
    void RunWorker() {
        actor_scheduler::set_current_ptr(this);
        TTime deadline{};
        thread_deadline = &deadline;
        while (auto cont = Queue.pop()) {
            deadline = TClock::now() + PreemptUs;
            cont();
        }
        thread_deadline = nullptr;
    }

private:
    std::chrono::microseconds PreemptUs;
    BlockingQueue<execute_callback_type> Queue;
    std::vector<std::thread> Threads;

    static inline thread_local const TTime* thread_deadline{ nullptr };
};

#if HAVE_ASIO
class AsioScheduler
    : private asio::thread_pool
    , public asio_actor_scheduler
{
public:
    AsioScheduler(size_t threads, std::chrono::microseconds preemptUs)
        : asio::thread_pool(threads)
        , asio_actor_scheduler(this->get_executor(), preemptUs)
    {}
};
#endif

std::shared_ptr<actor_scheduler> create_scheduler(ESchedulerType type,
        size_t threads, std::chrono::microseconds preemptUs)
{
    switch (type) {
    case ESchedulerType::LockFree:
        return std::make_shared<BlockingQueueScheduler<detail::blocking_queue>>(threads, preemptUs);
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
    default:
        throw std::runtime_error("unsupported scheduler type");
    }
}

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
    bool debugWakeups = false;

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
        if (arg == "--without-latencies") {
            withLatencies = false;
            continue;
        }
        if (arg == "--debug-wakeups") {
            debugWakeups = true;
            continue;
        }
        std::cerr << "ERROR: unexpected argument: " << argv[i] << std::endl;
        return 1;
    }

    auto scheduler = create_scheduler(schedulerType, numThreads, preemptUs);
    actor_scheduler::set_current_ptr(scheduler.get());

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

    if (debugWakeups) {
        if (auto wakeups = mailbox_wakeups.load(std::memory_order_relaxed)) {
            std::cout << "Mailbox wakeups: " << wakeups << std::endl;
        }
    }

    return 0;
}
