#pragma once
#include <atomic>
#include <cassert>
#include <tuple>

namespace coroactors::detail {

    /**
     * Base class for intrusive mailbox nodes
     */
    class intrusive_mailbox_node {
        template<class Node>
        friend class intrusive_mailbox;

    public:
        intrusive_mailbox_node() = default;

    private:
        std::atomic<void*> next;
    };

    /**
     * Intrusive multiple producers single consumer mailbox
     */
    template<class Node>
    class intrusive_mailbox {
        using node = intrusive_mailbox_node;

        static constexpr uintptr_t MarkerUnlocked = 1;

    public:
        struct initially_locked_t {};
        struct initially_unlocked_t {};

        static constexpr initially_locked_t initially_locked{};
        static constexpr initially_unlocked_t initially_unlocked{};

    public:
        explicit intrusive_mailbox(initially_locked_t = {}) noexcept {
            stub_.next.store(nullptr, std::memory_order_relaxed);
        }

        explicit intrusive_mailbox(initially_unlocked_t) noexcept {
            stub_.next.store(reinterpret_cast<void*>(MarkerUnlocked), std::memory_order_relaxed);
        }

        ~intrusive_mailbox() noexcept {
            // there is nothing we have to do
        }

        /**
         * Returns true if this locked mailbox is empty and push is likely to
         * produce a new first item. It may return false (not empty) even when
         * peek returns nullptr (no item available), e.g. when a concurrent
         * push operation is currently running, but has not published a new
         * first item yet.
         */
        bool empty() const noexcept {
            // Note: tail_ is allowed to be stale, because even when some other
            // threads concurrently pushed items and it wasn't communicated to
            // us in any way, then we would consider that a normal race, i.e.
            // our theoretical push and pop could have happened first.
            return head_ == &stub_ && tail_.load(std::memory_order_relaxed) == &stub_;
        }

        /**
         * Pushes a new node `item` to this mailbox
         *
         * Thread-safe and wait-free, may be performed by any thread.
         *
         * Returns true when this push also locks a previously unlocked mailbox,
         * however the first node in the locked mailbox might be a different
         * node entirely.
         */
        bool push(Node* item) noexcept {
            node* prev = push_prepare(item);
            return push_publish(prev, item);
        }

        /**
         * Removes and returns the next available node from a locked mailbox
         *
         * Returns nullptr and unlocks when the next node cannot be removed,
         * either because the mailbox is empty, or the first node is currently
         * blocked by a concurrent push, in which case that push will then lock
         * the mailbox.
         */
        Node* pop() noexcept {
            auto [head, next] = pop_impl<true>();
            if (head) {
                head_ = next;
            }
            return static_cast<Node*>(head);
        }

        /**
         * Tries to pop the next available node from a locked mailbox without unlocking
         */
        Node* try_pop() noexcept {
            auto [head, next] = pop_impl<false>();
            if (head) {
                head_ = next;
            }
            return static_cast<Node*>(head);
        }

        /**
         * Returns the next available node in a locked mailbox without removing it
         *
         * Returns nullptr when the next node cannot be removed, either because
         * the mailbox is empty, or the first node is currently blocked by a
         * concurrent push. When a non-nullptr pointer is returned the next
         * pop() is guaranteed to return it without unlocking.
         */
        Node* peek() noexcept {
            auto [head, next] = pop_impl<false>();
            return static_cast<Node*>(head);
        }

        /**
         * Tries to lock an empty mailbox without pushing new nodes
         *
         * Returns true on success, false when already locked (or will be soon).
         */
        bool try_lock() noexcept {
            // This is a bit subtle, but stub_'s next is MarkerUnlocked only
            // when the mailbox is not locked, but also when there's no item
            // available yet, in which case head == &stub_ as well. There is
            // a possible race with a concurrent push trying to update the
            // next pointer, but it's ok since we are racing to lock the
            // mailbox. After the mailbox is locked the only way for stub_'s
            // next to become MarkerUnlocked again is when head == stub_, i.e.
            // the mailbox is empty, and it is unlocked with a cas from nullptr
            // to MarkerUnlocked.
            void* expected = reinterpret_cast<void*>(MarkerUnlocked);
            if (stub_.next.compare_exchange_strong(expected, nullptr, std::memory_order_acquire)) {
                assert(head_ == &stub_);
                return true;
            }
            return false;
        }

        /**
         * Tries to unlock a locked and empty mailbox without removing a node
         *
         * Returns true on success, or false when the mailbox is not empty and
         * there's an unblocked node that is guaranteed to be removable.
         */
        bool try_unlock() noexcept {
            auto [head, next] = pop_impl<true>();
            return head == nullptr;
        }

    public:
        /**
         * First half of push: do not use directly, for testing only.
         *
         * Starts pushing a new node to the mailbox, but does not publish it
         * yet. Returns the previous node in the queue.
         */
        node* push_prepare(Node* item) noexcept {
            node* next = item;
            next->next.store(nullptr, std::memory_order_relaxed);
            // Note: acquire/release synchronizes with another push
            return tail_.exchange(next, std::memory_order_acq_rel);
        }

        /**
         * Second half of push: do not use directly, for testing only.
         *
         * Publishes a pushed node in the mailbox. Returns true when it locks.
         */
        bool push_publish(node* prev, Node* item) noexcept {
            node* next = item;
            // Note: release synchronizes with pop, acquire synchronizes with unlock
            void* marker = prev->next.exchange(next, std::memory_order_acq_rel);
            assert(marker == nullptr || marker == reinterpret_cast<void*>(MarkerUnlocked));
            // This mailbox was unlocked only when prev's next was MarkerUnlocked
            return marker == reinterpret_cast<void*>(MarkerUnlocked);
        }

    private:
        /**
         * Implementation of various forms of head node removal or unlocking
         *
         * Tries to make it safe to remove the current mailbox head node, and
         * returns a [head, next] pair, where head may be removed and replaced
         * with the next pointer. Returns head == nullptr when the mailbox is
         * either empty, or the head node is blocked by a concurrent push and
         * cannot be removed.
         *
         * When the Unlock template parameter is true the mailbox is atomically
         * unlocked when the mailbox is either empty or the current head cannot
         * be removed.
         */
        template<bool Unlock>
        std::tuple<node*, node*> pop_impl() noexcept {
            node* head = head_;
            // Note: acquire synchronizes with push publishing via next
            void* marker = head->next.load(std::memory_order_acquire);
            // Remove the stub node when the next item is published
            if (head == &stub_) {
                // When mailbox is empty we try to unlock
                if (marker == nullptr) {
                    if constexpr (!Unlock) {
                        // Unlock disabled, return empty
                        return { nullptr, nullptr };
                    } else {
                        if (head->next.compare_exchange_strong(
                                marker, reinterpret_cast<void*>(MarkerUnlocked), std::memory_order_acq_rel))
                        {
                            // Successfully unlocked the mailbox
                            return { nullptr, nullptr };
                        }
                        // Lost the race: next is updated with an inserted item
                        assert(marker != nullptr);
                    }
                }
                assert(marker != reinterpret_cast<void*>(MarkerUnlocked));
                // Remove the stub and move head to the first published item
                stub_.next.store(nullptr, std::memory_order_relaxed);
                head_ = head = reinterpret_cast<node*>(marker);
                marker = head->next.load(std::memory_order_acquire);
            }
            if (marker == nullptr) {
                // We have a published head node, but it cannot be removed
                // until we can be sure it's next pointer will not be modified
                // by a different thread.
                node* last = tail_.load(std::memory_order_relaxed);
                if (head == last) {
                    // Try inserting stub when head is the only item in the mailbox
                    assert(stub_.next.load(std::memory_order_relaxed) == nullptr);
                    if (tail_.compare_exchange_strong(last, &stub_, std::memory_order_release)) {
                        // We have inserted stub as the new tail immediately
                        // after head. This means no other thread was trying
                        // to insert anything after the current head.
                        head->next.store(&stub_, std::memory_order_relaxed);
                        return { head, &stub_ };
                    }
                    // Lost the race: tail was updated, and at least one more
                    // item is (maybe in the process of being) inserted.
                }
                // There is one more item, but its next pointer may be owned
                // by a different thread. Try to cas it to MarkerUnlocked,
                // which will either fail because push already finished, or
                // succeed and push will lock the mailbox.
                if constexpr (!Unlock) {
                    // Unlock disabled, return blocked head
                    return { nullptr, head };
                } else {
                    if (head->next.compare_exchange_strong(
                            marker, reinterpret_cast<void*>(MarkerUnlocked), std::memory_order_acq_rel))
                    {
                        // Successfully unlocked the mailbox
                        return { nullptr, nullptr };
                    }
                    // Lost the race: next item's push has finished, safe to remove
                }
            }
            assert(marker && marker != reinterpret_cast<void*>(MarkerUnlocked));
            return { head, reinterpret_cast<node*>(marker) };
        }

    private:
        alignas(128) node stub_;
        alignas(128) node* head_{ &stub_ };
        alignas(128) std::atomic<node*> tail_{ &stub_ };
    };

} // namespace coroactors::detail
