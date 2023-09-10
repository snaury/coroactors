#pragma once
#include <coroactors/detail/atomic_semaphore.h>
#include <atomic>
#include <cassert>
#include <chrono>
#include <memory>
#include <optional>
#include <thread>

namespace coroactors::detail {

    struct blocking_queue_waiter : protected sync_semaphore {
        blocking_queue_waiter* next{ nullptr };

        static blocking_queue_waiter* current() noexcept {
            static thread_local blocking_queue_waiter per_thread{};
            return &per_thread;
        }

        void notify() noexcept {
            sync_semaphore::release();
        }

        void wait() noexcept {
            sync_semaphore::acquire();
        }
    };

    /**
     * A multiple producers multiple consumers blocking queue
     */
    template<class T>
    class blocking_queue {
        struct node {
            std::atomic<void*> next{ nullptr };
            T item;

            template<class... TArgs>
            explicit node(TArgs&&... args)
                : item(std::forward<TArgs>(args)...)
            {}
        };

        struct unlock_taken_guard {
            blocking_queue* self;
            node* next;
            blocking_queue_waiter* pending;
            blocking_queue_waiter* pendingLast;

            ~unlock_taken_guard() noexcept {
                self->unlock_taken(next, pending, pendingLast);
            }
        };

    public:
        blocking_queue()
            : blocking_queue(new node)
        {
        }

        ~blocking_queue() {
            void* headValue = head_.exchange(nullptr, std::memory_order_acquire);
            if (!headValue || is_waiter(headValue)) {
                [[unlikely]]
                assert(false && "Destroying a queue locked by another thread");
                std::terminate();
            }
            node* head = decode_node(headValue);
            do {
                std::unique_ptr<node> current(head);
                headValue = head->next.exchange(nullptr, std::memory_order_acquire);
                if (is_waiter(headValue)) {
                    [[unlikely]]
                    assert(false && "Destroying a queue with enqueued waiters");
                    std::terminate();
                }
                head = decode_node(headValue);
            } while (head);
        }

        /**
         * Pushes a new item to this queue, potentially waking up a sleeping waiter
         */
        template<class... TArgs>
        void push(TArgs&&... args) {
            // Constructs a new item, it is the only point that may throw on push
            node* next = new node(std::forward<TArgs>(args)...);
            // Note: acquire/release synchronizes with another push
            node* prev = tail_.exchange(next, std::memory_order_acq_rel);
            // Note: release synchronizes with pop, acquire synchronizes with pop installing a waiter
            void* maybeWaiter = prev->next.exchange(next, std::memory_order_acq_rel);
            if (is_waiter(maybeWaiter)) {
                decode_waiter(maybeWaiter)->notify();
            } else {
                assert(maybeWaiter == nullptr);
            }
        }

        /**
         * Pops the next item from the queue, blocking until it is available
         */
        T pop() {
            // Linked list of waiters behind us
            blocking_queue_waiter* pending = nullptr;
            blocking_queue_waiter* pendingLast = nullptr;

            int spinCount = 0;
            std::chrono::high_resolution_clock::time_point spinStart;

            auto shouldSpin = [&]() -> bool {
                if (spinCount < 64) {
                    return true;
                }

                auto now = std::chrono::high_resolution_clock::now();
                if (spinCount == 64) {
                    spinStart = now;
                }
                auto elapsed = now - spinStart;

                if (elapsed < std::chrono::microseconds(4)) {
                    return true;
                }

                if (elapsed < std::chrono::microseconds(64)) {
                    std::this_thread::yield();
                    return true;
                }

                return false;
            };

            // Prefer loads to avoid stealing cache line unnecessarily
            void* headValue = head_.load(std::memory_order_relaxed);

            for (;;) {
                // We want spin for some time and wait for head to unlock
                if ((!headValue || is_waiter(headValue)) && shouldSpin()) {
                    void* updated = head_.load(std::memory_order_acquire);
                    if (headValue != updated) {
                        headValue = updated;
                        spinCount = 0;
                    } else {
                        ++spinCount;
                    }
                    continue;
                }

                // We don't consider outer loop as spinning since it tries to
                // install this thread as a waiter on lock failure.
                spinCount = 0;

                // We either lock the queue or remove some existing waiters, and
                // maybe help an unlocking thread in the process.
                if (headValue) {
                    headValue = head_.exchange(nullptr, std::memory_order_acquire);
                }

                // We successfully lock when previous head is not a waiter
                if (headValue && !is_waiter(headValue)) {
                    node* head = decode_node(headValue);
                    // This acquire synchronizes with push publishing an item
                    void* nextValue = head->next.load(std::memory_order_acquire);
                    for (;;) {
                        // We remove published item when next is not a waiter
                        if (nextValue && !is_waiter(nextValue)) {
                            node* next = decode_node(nextValue);
                            // We will free current head before returning
                            std::unique_ptr<node> current(head);
                            // We will unlock to next node before returning
                            unlock_taken_guard unlock{ this, next, pending, pendingLast };
                            // Move result out of the node, even if it throws the
                            // two objects above will cleanup in destructors.
                            return std::move(next->item);
                        }

                        // There is no next item yet, so we install ourselves as
                        // the first waiter in the list of waiters. We may have
                        // accumulated some list of pending waiters already, in
                        // which case the order is us, then pending, then the next
                        // waiters list, simply because we cannot modify next
                        // waiters in any way due to races with push here: the
                        // waiters list may have been removed already and in the
                        // process of waking up.
                        blocking_queue_waiter* local = blocking_queue_waiter::current();
                        if (pending) {
                            local->next = pending;
                            pendingLast->next = decode_waiter(nextValue);
                        } else {
                            local->next = decode_waiter(nextValue);
                        }

                        // Note: acquire synchronizes with push, release synchronizes with push consuming the waiter
                        if (head->next.compare_exchange_strong(nextValue, encode_waiter(local), std::memory_order_acq_rel)) {
                            // We have successfully installed ourselves as the next
                            // waiter, now we just need to unlock. In the process
                            // of unlocking we may find more waiters to lock, in
                            // which case this would be more waiters behind us.
                            pending = unlock_waiting(head);
                            pendingLast = pending ? find_last(pending) : nullptr;

                            // Wait until someone wakes us up
                            local->wait();

                            // A different thread may have added more waiters behind
                            // us, all of them have lower priority than pending list
                            // acquired while unlocking.
                            if (local->next) {
                                blocking_queue_waiter* last = find_last(local->next);
                                if (pending) {
                                    pendingLast->next = local->next;
                                    pendingLast = last;
                                } else {
                                    pending = local->next;
                                    pendingLast = last;
                                }
                                local->next = nullptr;
                            }

                            // Restart by trying to lock the head
                            headValue = head_.load(std::memory_order_relaxed);
                            break;
                        }

                        // Lost the race, and since head is currently locked by us
                        // it can only happen after push added a new item.
                        assert(nextValue && !is_waiter(nextValue));

                        // Remove links to consumed waiter list tail
                        if (pending) {
                            pendingLast->next = nullptr;
                        }
                        local->next = nullptr;
                    }

                    // Restart by trying to lock the head
                    continue;
                }

                // Queue is currently locked, but we may have removed some waiters
                // These are fresh and will be added at the front of pending
                if (headValue) {
                    blocking_queue_waiter* fresh = decode_waiter(headValue);
                    blocking_queue_waiter* last = find_last(fresh);
                    if (pending) {
                        last->next = pending;
                    } else {
                        pendingLast = last;
                    }
                    pending = fresh;
                    headValue = nullptr;
                }

                // Install ourselves as a new lock waiter, note however that we
                // will only wake up when there is at least one item in the queue,
                // and then we might start racing to lock and grab it.
                blocking_queue_waiter* local = blocking_queue_waiter::current();
                if (head_.compare_exchange_strong(headValue, encode_waiter(local), std::memory_order_release)) {
                    // Wait until someone wakes us up
                    local->wait();

                    // A different thread may have added more waiters behind us,
                    // all of them have higher priority than our pending list.
                    if (local->next) {
                        blocking_queue_waiter* last = find_last(local->next);
                        if (pending) {
                            last->next = pending;
                        } else {
                            pendingLast = last;
                        }
                        pending = local->next;
                        local->next = nullptr;
                    }

                    headValue = head_.load(std::memory_order_relaxed);
                }
            }
        }

        /**
         * Tries to pop the next item from the queue without blocking
         *
         * May spuriously return std::nullopt when there are items in the queue,
         * but the queue is currently blocked by another thread.
         */
        std::optional<T> try_pop() {
            // Linked list of waiters behind us
            blocking_queue_waiter* pending = nullptr;
            blocking_queue_waiter* pendingLast = nullptr;

            void* headValue = head_.load(std::memory_order_acquire);
            while (headValue && !is_waiter(headValue)) {
                if (!head_.compare_exchange_weak(headValue, nullptr, std::memory_order_acquire)) {
                    continue;
                }

                // Successfully locked
                node* head = decode_node(headValue);

                void* nextValue = head->next.load(std::memory_order_acquire);
                do {
                    // Note: on the second iteration it is guaranteed to be true
                    if (nextValue && !is_waiter(nextValue)) {
                        node* next = decode_node(nextValue);
                        // We will free current head before returning
                        std::unique_ptr<node> current(head);
                        // We will unlock to next node before returning
                        unlock_taken_guard unlock{ this, next, pending, pendingLast };
                        // Move result out of the node, even if it throws the
                        // two objects above will cleanup in destructors.
                        return std::move(next->item);
                    }
                } while (!try_unlock_no_item(head, nextValue, pending, pendingLast));

                // We have unlocked and there is no item
                break;
            }

            // Locked by another thread
            return std::nullopt;
        }

    private:
        /**
         * Unlocks after taking an item, setting head_ to the new head. Existing
         * waiters are either installed at the next pointer, or the first one in
         * the list is woken up. Even though there are multiple loops they are
         * technically wait-free, because retries only happen when more threads
         * install their waiters, and the number of threads is finite.
         */
        void unlock_taken(node* head, blocking_queue_waiter* waiter, blocking_queue_waiter* last) noexcept {
            for (;;) {
                while (!waiter) {
                    void* headValue = nullptr;
                    if (head_.compare_exchange_strong(headValue, head, std::memory_order_release)) {
                        // Fast path: we don't need to wake up anyone
                        return;
                    }

                    // Someone installed some waiters during our lock, remove them
                    headValue = head_.exchange(nullptr, std::memory_order_acquire);
                    if (headValue && is_waiter(headValue)) {
                        waiter = decode_waiter(headValue);
                        last = find_last(waiter);
                        break;
                    } else {
                        // Lost the race: another thread grabbed waiters
                        assert(!headValue);
                    }
                }

                // Note: acquire synchronizes with push or installing of another waiter
                void* nextValue = head->next.load(std::memory_order_acquire);
                for (;;) {
                    // Fully unlock and wake up a waiter when we have the next item
                    // The queue is still locked, so the next item will not change
                    if (nextValue && !is_waiter(nextValue)) {
                        for (;;) {
                            void* headValue = nullptr;
                            if (head_.compare_exchange_strong(headValue, head, std::memory_order_release)) {
                                break;
                            }
                            // More waiters have been installed, remove them and prepend to the list
                            headValue = head_.exchange(nullptr, std::memory_order_acquire);
                            if (headValue && is_waiter(headValue)) {
                                blocking_queue_waiter* fresh = decode_waiter(headValue);
                                find_last(fresh)->next = waiter;
                                waiter = fresh;
                            } else {
                                // Lost the race: another thread grabbed waiters
                                assert(!headValue);
                            }
                        }
                        waiter->notify();
                        return;
                    }

                    // Try installing our waiter list at the next node, queueing
                    // existing waiter list behind.
                    if (nextValue) {
                        last->next = decode_waiter(nextValue);
                    }

                    // Note: acquire synchronizes with push, release synchronizes with push consuming the waiter
                    if (head->next.compare_exchange_strong(nextValue, encode_waiter(waiter), std::memory_order_acq_rel)) {
                        // Installed a new waiter head, try unlocking without waiters
                        waiter = nullptr;
                        break;
                    }

                    // Lost the race, and since head is currently locked by us
                    // it can only happen after push added a new item.
                    assert(nextValue && !is_waiter(nextValue));

                    // Remove link to consumed waiter list tail
                    last->next = nullptr;
                }
            }
        }

        /**
         * Tries unlocking while there is no next item available
         */
        bool try_unlock_no_item(node* head, void*& nextValue, blocking_queue_waiter*& waiter, blocking_queue_waiter*& last) {
            for (;;) {
                while (!waiter) {
                    void* headValue = nullptr;
                    if (head_.compare_exchange_strong(headValue, head, std::memory_order_release)) {
                        // Fast path: we don't need to wake up anyone
                        return true;
                    }

                    // Someone installed some waiters during our unlock, remove them
                    headValue = head_.exchange(nullptr, std::memory_order_acquire);
                    if (headValue && is_waiter(headValue)) {
                        waiter = decode_waiter(headValue);
                        last = find_last(waiter);
                        break;
                    } else {
                        // Lost the race: another thread grabbed waiters
                        assert(!headValue);
                    }
                }

                // Try installing our waiter list at the next node, queueing
                // existing waiter list behind.
                if (nextValue) {
                    assert(is_waiter(nextValue));
                    last->next = decode_waiter(nextValue);
                }

                // Note: acquire synchronizes with push, release synchronizes with push consuming the waiter
                if (!head->next.compare_exchange_strong(nextValue, encode_waiter(waiter), std::memory_order_acq_rel)) {
                    // Lost the race, and since head is currently locked by us
                    // it can only happen after push added a new item.
                    assert(nextValue && !is_waiter(nextValue));

                    // Remove link to consumed waiter list tail
                    last->next = nullptr;
                    return false;
                }

                // Installed a new waiter head, try unlocking without waiters
                waiter = nullptr;
            }
        }

        /**
         * Unlocks after installing a waiter, setting head_ to the new head.
         * Returns existing waiter list queued for locking the queue.
         */
        blocking_queue_waiter* unlock_waiting(node* head) noexcept {
            void* headValue = head_.exchange(head, std::memory_order_acq_rel);
            if (headValue) {
                assert(is_waiter(headValue));
                return decode_waiter(headValue);
            } else {
                return nullptr;
            }
        }

    private:
        static bool is_waiter(void* p) noexcept {
            return reinterpret_cast<uintptr_t>(p) & WaiterMark;
        }

        static node* decode_node(void* p) noexcept {
            return reinterpret_cast<node*>(p);
        }

        static blocking_queue_waiter* decode_waiter(void* p) noexcept {
            return reinterpret_cast<blocking_queue_waiter*>(reinterpret_cast<uintptr_t>(p) & WaiterMask);
        }

        static void* encode_waiter(blocking_queue_waiter* waiter) noexcept {
            return reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(waiter) | WaiterMark);
        }

        static blocking_queue_waiter* find_last(blocking_queue_waiter* waiter) noexcept {
            while (waiter->next) {
                waiter = waiter->next;
            }
            return waiter;
        }

    private:
        /**
         * A helper constructor setting both pointers to the same head
         */
        constexpr blocking_queue(node* head) noexcept
            : head_{ head }
            , tail_{ head }
        {}

    private:
        static constexpr uintptr_t WaiterMark = 1;
        static constexpr uintptr_t WaiterMask = ~WaiterMark;

    private:
        alignas(128) std::atomic<void*> head_;
        alignas(128) std::atomic<node*> tail_;
    };

} // namespace coroactors::detail
