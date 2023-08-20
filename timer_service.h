#pragma once
#include <absl/synchronization/mutex.h>
#include <atomic>
#include <chrono>
#include <coroutine>
#include <map>
#include <thread>

namespace coroactors {

    /**
     * An example timer service
     */
    class timer_service {
    public:
        timer_service()
            : worker([this]{ run_worker(); })
        {}

        ~timer_service() {
            {
                absl::MutexLock l(&lock);
                wakeups.fetch_or(1, std::memory_order_release);
            }

            worker.join();
        }

        void push(std::coroutine_handle<> c, std::chrono::steady_clock::time_point deadline) {
            TNode* node = new TNode(c, deadline);
            // Note: acquire/release synchronizes with another push
            TNode* prev = tail.exchange(node, std::memory_order_acq_rel);
            // Note: release synchronizes with worker taking a value
            void* mark = prev->next.exchange(node, std::memory_order_release);
            if (mark == reinterpret_cast<void*>(MarkerWaiting)) {
                wakeup();
            }
        }

        struct TDeadlineAwaiter {
            timer_service& self;
            std::chrono::steady_clock::time_point deadline;

            bool await_ready() noexcept { return false; }

            __attribute__((__noinline__))
            void await_suspend(std::coroutine_handle<> c) {

            }
        };

        struct TAfterAwaiter {
            timer_service& self;
            std::chrono::steady_clock::duration timeout;

            bool await_ready() noexcept { return false; }

            __attribute__((__noinline__))
            void await_suspend(std::coroutine_handle<> c) {
                auto deadline = std::chrono::steady_clock::now() + timeout;
                self.push(c, deadline);
            }

            void await_resume() noexcept {}
        };

        TAfterAwaiter after(std::chrono::steady_clock::duration timeout) noexcept {
            return TAfterAwaiter{ *this, timeout };
        }

    private:
        void wakeup() {
            absl::MutexLock l(&lock);
            wakeups.fetch_add(2, std::memory_order_release);
        }

        enum class pop_result {
            item,
            added_waiter,
            already_waiting,
        };

        pop_result pop(std::coroutine_handle<>* c, std::chrono::steady_clock::time_point* deadline) {
            void* nextValue = head->next.load(std::memory_order_acquire);
            for (;;) {
                if (!nextValue) {
                    if (!head->next.compare_exchange_weak(nextValue, reinterpret_cast<void*>(MarkerWaiting), std::memory_order_acquire)) {
                        continue;
                    }
                    return pop_result::added_waiter;
                }
                if (nextValue == reinterpret_cast<void*>(MarkerWaiting)) {
                    return pop_result::already_waiting;
                }
                TNode* next = reinterpret_cast<TNode*>(nextValue);
                *c = next->continuation;
                *deadline = next->deadline;
                std::unique_ptr<TNode> current(head);
                head = next;
                return pop_result::item;
            }
        }

        void run_worker() {
            int neededWakeups = 0;
            for (;;) {
                for (;;) {
                    std::coroutine_handle<> c;
                    std::chrono::steady_clock::time_point deadline;
                    switch (pop(&c, &deadline)) {
                        case pop_result::item: {
                            sorted.emplace(deadline, c);
                            continue;
                        }
                        case pop_result::added_waiter: {
                            neededWakeups += 2;
                            break;
                        }
                        case pop_result::already_waiting: {
                            break;
                        }
                    }
                    break;
                }

                std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();

                auto it = sorted.begin();
                if (it != sorted.end() && it->first <= now) {
                    it->second.resume();
                    sorted.erase(it++);
                    // We may have spent a lot of time, recheck queues and update time
                    continue;
                }

                auto currentWakeups = wakeups.load(std::memory_order_acquire);
                if (currentWakeups > 0) {
                    do {
                        assert(currentWakeups <= neededWakeups);
                        if (wakeups.compare_exchange_weak(currentWakeups, currentWakeups & 1, std::memory_order_acquire)) {
                            break;
                        }
                    } while (currentWakeups > 0);

                    if (currentWakeups & 1) {
                        // Destructor asking us to stop
                        return;
                    }

                    neededWakeups -= currentWakeups;
                    continue;
                }

                assert(neededWakeups > 0);

                absl::Duration duration;
                if (it != sorted.end()) {
                    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(it->first - now).count();
                    duration = absl::Nanoseconds(ns);
                } else {
                    duration = absl::InfiniteDuration();
                }

                // We use lock as a timer
                lock.LockWhenWithTimeout(absl::Condition(this, &timer_service::has_nonzero_wakeups), duration);
                lock.Unlock();
            }
        }

        bool has_nonzero_wakeups() const {
            return wakeups.load(std::memory_order_acquire) != 0;
        }

    private:
        struct TNode {
            std::atomic<void*> next{ nullptr };
            std::coroutine_handle<> continuation;
            std::chrono::steady_clock::time_point deadline;

            TNode() noexcept = default;

            TNode(std::coroutine_handle<> c, std::chrono::steady_clock::time_point deadline) noexcept
                : continuation(c)
                , deadline(deadline)
            {}
        };

        static constexpr uintptr_t MarkerWaiting = 1;

    private:
        absl::Mutex lock;
        TNode* head{ new TNode };
        std::multimap<std::chrono::steady_clock::time_point, std::coroutine_handle<>> sorted;
        std::atomic<TNode*> tail{ head };
        std::atomic<int> wakeups{ 0 };

        // Must be the last field
        std::thread worker;
    };

} // namespace coroactors
