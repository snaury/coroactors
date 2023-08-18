#pragma once
#include <atomic>
#include <cassert>
#include <memory>
#include <utility>

namespace coroactors::detail {

    /**
     * A multiple producers single consumer mailbox
     */
    template<class T>
    class TMailbox {
        struct TNode {
            std::atomic<TNode*> Next{ nullptr };
            T Item;

            template<class... TArgs>
            explicit TNode(TArgs&&... args)
                : Item(std::forward<TArgs>(args)...)
            {}
        };

    public:
        /**
         * Constructs a new mailbox, initially locked and empty
         */
        TMailbox() = default;

        /**
         * Destroys all items in the mailbox
         * Caller must ensure no concurrent operations are running
         */
        ~TMailbox() {
            TNode* head = std::exchange(Head, nullptr);
            assert(head != nullptr);
            do {
                TNode* next = head->Next.load(std::memory_order_acquire);
                if (next == (TNode*)MarkerUnlocked) {
                    next = nullptr;
                }
                delete head;
                head = next;
            } while (head);
        }

    public:
        /**
         * Appends a new item to the mailbox
         * Returns false when mailbox is already locked (or will be)
         * Returns true when a new first item is pushed and it became locked
         */
        template<class TArg, class... TArgs>
        bool Push(TArg&& arg, TArgs&&... args) {
            // Constructs a new item, it is the only point that may throw on Push
            TNode* node = new TNode(std::forward<TArg>(arg), std::forward<TArgs>(args)...);
            // Note: acquire/release synchronizes with another Push
            TNode* prev = Tail.exchange(node, std::memory_order_acq_rel);
            // Note: release synchronizes with Pop, acquire synchronizes with unlock
            TNode* marker = prev->Next.exchange(node, std::memory_order_acq_rel);
            // The mailbox was unlocked only if previous Next was MarkerUnlocked
            return marker == (TNode*)MarkerUnlocked;
        }

        /**
         * Returns the next item from the locked mailbox when it is not empty
         * Returns a default value and unlocks when mailbox is empty
         */
        T Pop() {
            TNode* head = Head;
            TNode* next = head->Next.load(std::memory_order_acquire);
            if (next == nullptr) {
                // Mailbox is currently empty, try to unlock
                if (head->Next.compare_exchange_strong(next, (TNode*)MarkerUnlocked, std::memory_order_acq_rel)) {
                    // Successfully unlocked
                    return T();
                }
                // Lost the race: now next != nullptr
                assert(next != nullptr);
            }
            assert(next != (TNode*)MarkerUnlocked);
            std::unique_ptr<TNode> current(head);
            Head = next;
            return std::move(next->Item);
        }

        /**
         * Returns pointer to the next element or nullptr if the mailbox is empty
         */
        const T* Peek() const {
            TNode* head = Head;
            TNode* next = head->Next.load(std::memory_order_acquire);
            if (next == nullptr) {
                // Mailbox is currently empty, keep it locked
                return nullptr;
            }
            return &next->Item;
        }

        /**
         * Returns true if the mailbox is empty and push is likely to produce a new first element
         * Note: it may return false even when Peek returns nullptr, e.g. when a
         * concurrent push is currently 
         * Requires mailbox to be locked
         */
        bool Empty() const {
            TNode* head = Head;
            TNode* tail = Tail.load(std::memory_order_relaxed);
            return head == tail;
        }

        /**
         * Tries to unlock an empty mailbox
         * Returns true on success or false when it's not empty
         */
        bool TryUnlock() {
            TNode* head = Head;
            TNode* next = head->Next.load(std::memory_order_relaxed);
            if (next == nullptr) {
                // We either succeed at unlocking or mailbox becomes non-empty
                return head->Next.compare_exchange_strong(next, (TNode*)MarkerUnlocked, std::memory_order_release);
            }
            assert(next != (TNode*)MarkerUnlocked);
            // Mailbox is not empty
            return false;
        }

    private:
        static constexpr uintptr_t MarkerUnlocked = 1;

    private:
        TNode* Head = new TNode;
        char HeadPadding[64 - sizeof(Head)];
        std::atomic<TNode*> Tail{ Head };
        char TailPadding[64 - sizeof(Tail)];
    };

} // namespace coroactors::detail
