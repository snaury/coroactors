#pragma once
#include <atomic>
#include <cassert>
#include <memory>
#include <optional>
#include <utility>

namespace coroactors::detail {

    /**
     * A multiple producers single consumer mailbox
     */
    template<class T>
    class mailbox {
        struct node {
            std::atomic<void*> next{ nullptr };
            T item;

            template<class... TArgs>
            explicit node(TArgs&&... args)
                : item(std::forward<TArgs>(args)...)
            {}
        };

    public:
        struct initially_locked_t {};
        struct initially_unlocked_t {};

        static constexpr initially_locked_t initially_locked{};
        static constexpr initially_unlocked_t initially_unlocked{};

    public:
        /**
         * Constructs a new mailbox, initially locked and empty
         */
        explicit mailbox(initially_locked_t = {}) {
            // this is the default
        }

        /**
         * Constructs a new mailbox, initially unlocked and empty
         */
        explicit mailbox(initially_unlocked_t) {
            head_->next.store(reinterpret_cast<void*>(MarkerUnlocked), std::memory_order_relaxed);
        }

        /**
         * Destroys all items in the mailbox
         * Caller must ensure no concurrent operations are running
         */
        ~mailbox() noexcept {
            std::unique_ptr<node> head(std::move(head_));
            assert(head != nullptr);
            do {
                node* next;
                void* marker = head->next.load(std::memory_order_acquire);
                if (marker == reinterpret_cast<void*>(MarkerUnlocked)) {
                    next = nullptr;
                } else {
                    next = reinterpret_cast<node*>(marker);
                }
                head.reset(next);
            } while (head);
        }

    public:
        /**
         * Pushes a new item to this mailbox
         *
         * Thread-safe and lock-free (wait-free except for allocation when a
         * new node and item are constructed), may be performed by any thread.
         * Returns false when mailbox is already locked (or a concurrent push
         * will return true sooner or later, unblocking the mailbox), which
         * should be the most common case under contention. Returns true when a
         * new first item is pushed (the next pop is guaranteed to remove it)
         * and this operation locked the mailbox, caller is supposed to
         * schedule it for processing.
         */
        template<class... TArgs>
        bool push(TArgs&&... args) {
            // Constructs a new item, it is the only point that may throw on push
            node* next = new node(std::forward<TArgs>(args)...);
            // Note: acquire/release synchronizes with another push
            node* prev = tail_.exchange(next, std::memory_order_acq_rel);
            // Note: release synchronizes with Pop, acquire synchronizes with unlock
            void* marker = prev->next.exchange(next, std::memory_order_acq_rel);
            // The mailbox was unlocked only if previous next was MarkerUnlocked
            return marker == reinterpret_cast<void*>(MarkerUnlocked);
        }

        /**
         * Removes the next item from a locked mailbox
         *
         * Returns it when the next item is available.
         * Returns a default value and unlocks otherwise.
         */
        T pop_default() {
            node* head = head_.get();
            void* marker = head->next.load(std::memory_order_acquire);
            if (marker == nullptr) {
                // Next item is unavailable, try to unlock
                if (head->next.compare_exchange_strong(marker, reinterpret_cast<void*>(MarkerUnlocked), std::memory_order_acq_rel)) {
                    // Successfully unlocked
                    return T();
                }
                // Lost the race: now next != nullptr
                assert(marker != nullptr);
            }
            assert(marker != reinterpret_cast<void*>(MarkerUnlocked));
            node* next = reinterpret_cast<node*>(marker);
            head_.reset(next);
            return std::move(next->item);
        }

        /**
         * Removes the next item from a locked mailbox
         *
         * Returns it when the next item is available.
         * Returns std::nullopt and unlocks otherwise.
         */
        std::optional<T> pop_optional() {
            node* head = head_.get();
            void* marker = head->next.load(std::memory_order_acquire);
            if (marker == nullptr) {
                // Next item is unavailable, try to unlock
                if (head->next.compare_exchange_strong(marker, reinterpret_cast<void*>(MarkerUnlocked), std::memory_order_acq_rel)) {
                    // Successfully unlocked
                    return std::nullopt;
                }
                // Lost the race: now next != nullptr
                assert(marker != nullptr);
            }
            assert(marker != reinterpret_cast<void*>(MarkerUnlocked));
            node* next = reinterpret_cast<node*>(marker);
            head_.reset(next);
            return std::move(next->item);
        }

        /**
         * Returns a pointer to the next item in a locked mailbox,
         * or nullptr if no next item is available.
         */
        T* peek() {
            node* head = head_.get();
            void* marker = head->next.load(std::memory_order_acquire);
            if (marker == nullptr) {
                // Next item is unavailable, keep it locked
                return nullptr;
            }
            assert(marker != reinterpret_cast<void*>(MarkerUnlocked));
            node* next = reinterpret_cast<node*>(marker);
            return &next->item;
        }

        /**
         * Returns true if the locked mailbox is empty and push is likely to
         * produce a new first item. It may return false (not empty) even when
         * peek returns nullptr (no item available), e.g. when a concurrent
         * push operation is currently running and blocking mailbox head.
         */
        bool empty() const {
            node* head = head_.get();
            node* tail = tail_.load(std::memory_order_relaxed);
            return head == tail;
        }

        /**
         * Tries to unlock a locked mailbox
         *
         * Returns true on success (a future or currently running concurrent
         * push will return true and lock this mailbox) or false when there's
         * an item currently available at the head.
         */
        bool try_unlock() {
            node* head = head_.get();
            void* marker = head->next.load(std::memory_order_acquire);
            if (marker == nullptr) {
                // We either succeed at unlocking or next item becomes available
                return head->next.compare_exchange_strong(marker, reinterpret_cast<void*>(MarkerUnlocked), std::memory_order_release);
            }
            assert(marker != reinterpret_cast<node*>(MarkerUnlocked));
            // Next item is available
            return false;
        }

    private:
        static constexpr uintptr_t MarkerUnlocked = 1;

    private:
        alignas(128) std::unique_ptr<node> head_{ new node };
        alignas(128) std::atomic<node*> tail_{ head_.get() };
    };

} // namespace coroactors::detail
