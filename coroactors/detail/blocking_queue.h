#pragma once
#include <coroactors/detail/atomic_semaphore.h>
#include <atomic>
#include <cassert>
#include <chrono>
#include <memory>
#include <optional>
#include <thread>

namespace coroactors::detail {

    struct TBlockingQueueWaiter {
        using TAtomic = detail::semaphore_atomic_t;
        using TValue = TAtomic::value_type;

        TAtomic Semaphore{ 0 };
        TBlockingQueueWaiter* Next = nullptr;

        void Wait() noexcept {
            // Fast path: assume Wake was called already
            TValue current = 2;
            if (Semaphore.compare_exchange_strong(current, 0, std::memory_order_acquire)) {
                return;
            }

            // Fast path: spin assuming Wake will be called very soon
            for (int i = 0; i < 64; ++i) {
                while (current >= 2) {
                    if (Semaphore.compare_exchange_weak(current, current - 2, std::memory_order_acquire)) {
                        return;
                    }
                }
                current = Semaphore.load(std::memory_order_relaxed);
            }

            // Spin for 64µs, yielding thread after 4µs
            auto start = std::chrono::high_resolution_clock::now();
            for (;;) {
                while (current >= 2) {
                    if (Semaphore.compare_exchange_weak(current, current - 2, std::memory_order_acquire)) {
                        return;
                    }
                }
                std::chrono::nanoseconds elapsed = std::chrono::high_resolution_clock::now() - start;
                if (elapsed >= std::chrono::microseconds(64)) {
                    break;
                }
                if (elapsed >= std::chrono::microseconds(4)) {
                    std::this_thread::yield();
                }
                current = Semaphore.load(std::memory_order_relaxed);
            }

            // Set the wait flag, asking Wake to call notify_one
            if (!(current & 1)) {
                [[likely]]
                current = Semaphore.fetch_or(1, std::memory_order_relaxed) | 1;
            } else {
                assert(false && "Wait semaphore has an unexpected wait flag already set");
            }

            // Slow path
            for (;;) {
                while (current >= 2) {
                    if (Semaphore.compare_exchange_weak(current, current - 3, std::memory_order_acquire)) {
                        return;
                    }
                }
                Semaphore.wait(current, std::memory_order_relaxed);
                current = Semaphore.load(std::memory_order_relaxed);
            }
        }

        void Wake() noexcept {
            TValue current = Semaphore.fetch_add(2, std::memory_order_release);
            if (current & 1) {
                Semaphore.notify_one();
            }
        }
    };

    inline thread_local TBlockingQueueWaiter PerThreadBlockingQueueWaiter;

    /**
     * A multiple producers multiple consumers blocking queue
     */
    template<class T>
    class TBlockingQueue {
        struct TNode {
            std::atomic<void*> Next{ nullptr };
            T Item;

            template<class... TArgs>
            explicit TNode(TArgs&&... args)
                : Item(std::forward<TArgs>(args)...)
            {}
        };

        using TBlockingQueueWaiter = detail::TBlockingQueueWaiter;

        struct TUnlockTaken {
            TBlockingQueue* Self;
            TNode* Next;
            TBlockingQueueWaiter* Pending;
            TBlockingQueueWaiter* PendingLast;

            ~TUnlockTaken() noexcept {
                Self->UnlockTaken(Next, Pending, PendingLast);
            }
        };

    public:
        TBlockingQueue()
            : TBlockingQueue(new TNode)
        {
        }

        ~TBlockingQueue() {
            void* headValue = Head.exchange(nullptr, std::memory_order_acquire);
            if (!headValue || IsWaiter(headValue)) {
                [[unlikely]]
                assert(false && "Destroying a queue locked by another thread");
                std::terminate();
            }
            TNode* head = AsNode(headValue);
            do {
                std::unique_ptr<TNode> current(head);
                headValue = head->Next.exchange(nullptr, std::memory_order_acquire);
                if (IsWaiter(headValue)) {
                    [[unlikely]]
                    assert(false && "Destroying a queue with enqueued waiters");
                    std::terminate();
                }
                head = AsNode(headValue);
            } while (head);
        }

        /**
         * Pushes a new item to the queue, potentially waking up a sleeping waiter
         */
        template<class... TArgs>
        void Push(TArgs&&... args) {
            // Constructs a new item, it is the only point that may throw on Push
            TNode* node = new TNode(std::forward<TArgs>(args)...);
            // Note: acquire/release synchronizes with another Push
            TNode* prev = Tail.exchange(node, std::memory_order_acq_rel);
            // Note: release synchronizes with Pop, acquire synchronizes with Pop installing a waiter
            void* maybeWaiter = prev->Next.exchange(node, std::memory_order_acq_rel);
            if (IsWaiter(maybeWaiter)) {
                AsWaiter(maybeWaiter)->Wake();
            } else {
                assert(maybeWaiter == nullptr);
            }
        }

        /**
         * Pops the next item from the queue, blocking until it is available
         */
        T Pop() {
            // Linked list of waiters behind us
            TBlockingQueueWaiter* pending = nullptr;
            TBlockingQueueWaiter* pendingLast = nullptr;

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

            // Prefer relaxed loads to avoid stealing cache line unnecessarily
            void* headValue = Head.load(std::memory_order_relaxed);

            for (;;) {
                // We want spin for some time and wait for head to unlock
                if ((!headValue || IsWaiter(headValue)) && shouldSpin()) {
                    void* updated = Head.load(std::memory_order_relaxed);
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
                    headValue = Head.exchange(nullptr, std::memory_order_acquire);
                }

                // We successfully lock when previous head is not a waiter
                if (headValue && !IsWaiter(headValue)) {
                    TNode* head = AsNode(headValue);
                    // This acquire synchronizes with Push publishing an item
                    void* nextValue = head->Next.load(std::memory_order_acquire);
                    for (;;) {
                        // We remove published item when next is not a waiter
                        if (nextValue && !IsWaiter(nextValue)) {
                            TNode* next = AsNode(nextValue);
                            // We will free current head before returning
                            std::unique_ptr<TNode> current(head);
                            // We will unlock to next node before returning
                            TUnlockTaken unlock{ this, next, pending, pendingLast };
                            // Move result out of the node, even if it throws the
                            // two objects above will cleanup in destructors.
                            return std::move(next->Item);
                        }

                        // There is no next item yet, so we install ourselves as
                        // the first waiter in the list of waiters. We may have
                        // accumulated some list of pending waiters already, in
                        // which case the order is us, then pending, then the next
                        // waiters list, simply because we cannot modify next
                        // waiters in any way due to races with Push here: the
                        // waiters list may have been removed already and in the
                        // process of waking up.
                        TBlockingQueueWaiter* local = &detail::PerThreadBlockingQueueWaiter;
                        if (pending) {
                            local->Next = pending;
                            pendingLast->Next = AsWaiter(nextValue);
                        } else {
                            local->Next = AsWaiter(nextValue);
                        }

                        // Note: acquire synchronizes with Push, release synchronizes with Push consuming the waiter
                        if (head->Next.compare_exchange_strong(nextValue, EncodeWaiter(local), std::memory_order_acq_rel)) {
                            // We have successfully installed ourselves as the next
                            // waiter, now we just need to unlock. In the process
                            // of unlocking we may find more waiters to lock, in
                            // which case this would be more waiters behind us.
                            pending = UnlockWaiting(head);
                            pendingLast = pending ? Last(pending) : nullptr;

                            // Wait until someone wakes us up
                            local->Wait();

                            // A different thread may have added more waiters behind
                            // us, all of them have lower priority than pending list
                            // acquired while unlocking.
                            if (local->Next) {
                                TBlockingQueueWaiter* last = Last(local->Next);
                                if (pending) {
                                    pendingLast->Next = local->Next;
                                    pendingLast = last;
                                } else {
                                    pending = local->Next;
                                    pendingLast = last;
                                }
                                local->Next = nullptr;
                            }

                            // Restart by trying to lock the head
                            headValue = Head.load(std::memory_order_relaxed);
                            break;
                        }

                        // Lost the race, and since head is currently locked by us
                        // it can only happen after Push added a new item.
                        assert(nextValue && !IsWaiter(nextValue));

                        // Remove links to consumed waiter list tail
                        if (pending) {
                            pendingLast->Next = nullptr;
                        }
                        local->Next = nullptr;
                    }

                    // Restart by trying to lock the head
                    continue;
                }

                // Queue is currently locked, but we may have removed some waiters
                // These are fresh and will be added at the front of pending
                if (headValue) {
                    TBlockingQueueWaiter* fresh = AsWaiter(headValue);
                    TBlockingQueueWaiter* last = Last(fresh);
                    if (pending) {
                        last->Next = pending;
                    } else {
                        pendingLast = last;
                    }
                    pending = fresh;
                    headValue = nullptr;
                }

                // Install ourselves as a new lock waiter, note however that we
                // will only wake up when there is at least one item in the queue,
                // and then we might start racing to lock and grab it.
                TBlockingQueueWaiter* local = &detail::PerThreadBlockingQueueWaiter;
                if (Head.compare_exchange_strong(headValue, EncodeWaiter(local), std::memory_order_release)) {
                    // Wait until someone wakes us up
                    local->Wait();

                    // A different thread may have added more waiters behind us,
                    // all of them have higher priority than our pending list.
                    if (local->Next) {
                        TBlockingQueueWaiter* last = Last(local->Next);
                        if (pending) {
                            last->Next = pending;
                        } else {
                            pendingLast = last;
                        }
                        pending = local->Next;
                        local->Next = nullptr;
                    }

                    headValue = Head.load(std::memory_order_relaxed);
                }
            }
        }

        /**
         * Tries to pop the next item from the queue without blocking
         *
         * May spuriously return std::nullopt when there are items in the queue,
         * but the queue is currently blocked by another thread.
         */
        std::optional<T> TryPop() {
            // Linked list of waiters behind us
            TBlockingQueueWaiter* pending = nullptr;
            TBlockingQueueWaiter* pendingLast = nullptr;

            void* headValue = Head.load(std::memory_order_acquire);
            while (headValue && !IsWaiter(headValue)) {
                if (!Head.compare_exchange_weak(headValue, nullptr, std::memory_order_acquire)) {
                    continue;
                }

                // Successfully locked
                TNode* head = AsNode(headValue);

                void* nextValue = head->Next.load(std::memory_order_acquire);
                do {
                    // Note: on the second iteration it is guaranteed to be true
                    if (nextValue && !IsWaiter(nextValue)) {
                        TNode* next = AsNode(nextValue);
                        // We will free current head before returning
                        std::unique_ptr<TNode> current(head);
                        // We will unlock to next node before returning
                        TUnlockTaken unlock{ this, next, pending, pendingLast };
                        // Move result out of the node, even if it throws the
                        // two objects above will cleanup in destructors.
                        return std::move(next->Item);
                    }
                } while (!TryUnlockNoItem(head, nextValue, pending, pendingLast));

                // We have unlocked and there is no item
                break;
            }

            // Locked by another thread
            return std::nullopt;
        }

    private:
        /**
         * Unlocks after taking an item, setting Head to the new head. Existing
         * waiters are either installed at the next pointer, or the first one in
         * the list is woken up. Even though there are multiple loops they are
         * technically wait-free, because retries only happen when more threads
         * install their waiters, and the number of threads is finite.
         */
        void UnlockTaken(TNode* head, TBlockingQueueWaiter* waiter, TBlockingQueueWaiter* last) noexcept {
            for (;;) {
                while (!waiter) {
                    void* headValue = nullptr;
                    if (Head.compare_exchange_strong(headValue, head, std::memory_order_release)) {
                        // Fast path: we don't need to wake up anyone
                        return;
                    }

                    // Someone installed some waiters during our lock, remove them
                    headValue = Head.exchange(nullptr, std::memory_order_acquire);
                    if (headValue && IsWaiter(headValue)) {
                        waiter = AsWaiter(headValue);
                        last = Last(waiter);
                        break;
                    } else {
                        // Lost the race: another thread grabbed waiters
                        assert(!headValue);
                    }
                }

                // Note: acquire synchronizes with Push or installing of another waiter
                void* nextValue = head->Next.load(std::memory_order_acquire);
                for (;;) {
                    // Fully unlock and wake up a waiter when we have the next item
                    // The queue is still locked, so the next item will not change
                    if (nextValue && !IsWaiter(nextValue)) {
                        for (;;) {
                            void* headValue = nullptr;
                            if (Head.compare_exchange_strong(headValue, head, std::memory_order_release)) {
                                break;
                            }
                            // More waiters have been installed, remove them and prepend to the list
                            headValue = Head.exchange(nullptr, std::memory_order_acquire);
                            if (headValue && IsWaiter(headValue)) {
                                TBlockingQueueWaiter* fresh = AsWaiter(headValue);
                                Last(fresh)->Next = waiter;
                                waiter = fresh;
                            } else {
                                // Lost the race: another thread grabbed waiters
                                assert(!headValue);
                            }
                        }
                        waiter->Wake();
                        return;
                    }

                    // Try installing our waiter list at the next node, queueing
                    // existing waiter list behind.
                    if (nextValue) {
                        last->Next = AsWaiter(nextValue);
                    }

                    // Note: acquire synchronizes with Push, release synchronizes with Push consuming the waiter
                    if (head->Next.compare_exchange_strong(nextValue, EncodeWaiter(waiter), std::memory_order_acq_rel)) {
                        // Installed a new waiter head, try unlocking without waiters
                        waiter = nullptr;
                        break;
                    }

                    // Lost the race, and since head is currently locked by us
                    // it can only happen after Push added a new item.
                    assert(nextValue && !IsWaiter(nextValue));

                    // Remove link to consumed waiter list tail
                    last->Next = nullptr;
                }
            }
        }

        /**
         * Tries unlocking while there is no next item available
         */
        bool TryUnlockNoItem(TNode* head, void*& nextValue, TBlockingQueueWaiter*& waiter, TBlockingQueueWaiter*& last) {
            for (;;) {
                while (!waiter) {
                    void* headValue = nullptr;
                    if (Head.compare_exchange_strong(headValue, head, std::memory_order_release)) {
                        // Fast path: we don't need to wake up anyone
                        return true;
                    }

                    // Someone installed some waiters during our unlock, remove them
                    headValue = Head.exchange(nullptr, std::memory_order_acquire);
                    if (headValue && IsWaiter(headValue)) {
                        waiter = AsWaiter(headValue);
                        last = Last(waiter);
                        break;
                    } else {
                        // Lost the race: another thread grabbed waiters
                        assert(!headValue);
                    }
                }

                // Try installing our waiter list at the next node, queueing
                // existing waiter list behind.
                if (nextValue) {
                    assert(IsWaiter(nextValue));
                    last->Next = AsWaiter(nextValue);
                }

                // Note: acquire synchronizes with Push, release synchronizes with Push consuming the waiter
                if (!head->Next.compare_exchange_strong(nextValue, EncodeWaiter(waiter), std::memory_order_acq_rel)) {
                    // Lost the race, and since head is currently locked by us
                    // it can only happen after Push added a new item.
                    assert(nextValue && !IsWaiter(nextValue));

                    // Remove link to consumed waiter list tail
                    last->Next = nullptr;
                    return false;
                }

                // Installed a new waiter head, try unlocking without waiters
                waiter = nullptr;
            }
        }

        /**
         * Unlocks after installing a waiter, setting Head to the new head.
         * Returns existing waiter list queued for locking the queue.
         */
        TBlockingQueueWaiter* UnlockWaiting(TNode* head) noexcept {
            void* headValue = Head.exchange(head, std::memory_order_acq_rel);
            if (headValue) {
                assert(IsWaiter(headValue));
                return AsWaiter(headValue);
            } else {
                return nullptr;
            }
        }

    private:
        static bool IsWaiter(void* p) noexcept {
            return reinterpret_cast<uintptr_t>(p) & WaiterMark;
        }

        static TNode* AsNode(void* p) noexcept {
            return reinterpret_cast<TNode*>(p);
        }

        static TBlockingQueueWaiter* AsWaiter(void* p) noexcept {
            return reinterpret_cast<TBlockingQueueWaiter*>(reinterpret_cast<uintptr_t>(p) & WaiterMask);
        }

        static void* EncodeWaiter(TBlockingQueueWaiter* waiter) noexcept {
            return reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(waiter) | WaiterMark);
        }

        static TBlockingQueueWaiter* Last(TBlockingQueueWaiter* waiter) noexcept {
            while (waiter->Next) {
                waiter = waiter->Next;
            }
            return waiter;
        }

    private:
        /**
         * A helper constructor setting both pointers to the same head
         */
        constexpr TBlockingQueue(TNode* head) noexcept
            : Head{ head }
            , Tail{ head }
        {}

    private:
        static constexpr uintptr_t WaiterMark = 1;
        static constexpr uintptr_t WaiterMask = ~WaiterMark;

    private:
        std::atomic<void*> Head;
        char HeadPadding[64 - sizeof(Head)];
        std::atomic<TNode*> Tail;
        char TailPadding[64 - sizeof(Tail)];
    };

} // namespace coroactors::detail
