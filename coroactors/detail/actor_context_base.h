#pragma once
#include <coroactors/detail/awaiters.h>
#include <coroactors/detail/intrusive_mailbox.h>
#include <coroactors/intrusive_ptr.h>

namespace coroactors {

    class actor_context;
    class actor_scheduler;
    class actor_scheduler_runnable;

    template<class T>
    class async;

} // namespace coroactors

namespace coroactors::detail {

    class async_context_manager;

    class actor_context_state final : public intrusive_atomic_base<actor_context_state> {
        friend actor_context;
        friend async_context_manager;

        using mailbox_t = intrusive_mailbox<actor_scheduler_runnable>;

    public:
        explicit actor_context_state(actor_scheduler& s)
            : scheduler(s)
        {}

    private:
        /**
         * Pushes the next runnable to this context's mailbox
         *
         * Returns false when the mailbox is currently locked, the runnable is
         * enqueued and may run in another thread. Otherwise locks the mailbox
         * and returns true. The next (likely the same) runnable should be
         * acquired with `next_runnable()`, which may rarely fail and unlock
         * the mailbox due to a concurrent push.
         *
         * This method is thread-safe and may be called by any thread. It is
         * also wait-free, never allocates and never throws exceptions.
         */
        bool push_runnable(actor_scheduler_runnable* r) noexcept;

        /**
         * Removes the next runnable from this context's mailbox
         *
         * Returns nullptr and unlocks the mailbox when the next runnable is
         * unavailable, care must be taken since this context may immediately
         * start running in another thread due to a successful `push_runnable`.
         * Returns the next runnable and keeps the mailbox locked otherwise.
         *
         * This method may only be called by a thread that implicitly has this
         * context locked by a previous call to `push_runnable`. It is also
         * wait-free, never allocates and never throws exceptions.
         */
        actor_scheduler_runnable* next_runnable() noexcept;

    private:
        actor_scheduler& scheduler;
        mailbox_t mailbox_{ mailbox_t::initially_unlocked };
    };

} // namespace coroactors::detail
