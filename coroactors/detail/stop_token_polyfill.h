#pragma once
#include <coroactors/detail/atomic_semaphore.h>
#include <atomic>
#include <thread>

namespace coroactors::detail {

    /**
     * A type erased callback
     */
    struct stop_state_callback {
        using invoke_t = void (*)(stop_state_callback*) noexcept;
        const invoke_t invoke;

        // A linked list of registered callbacks
        stop_state_callback* prev{ nullptr };
        stop_state_callback* next{ nullptr };

        // Blocks a running callback destruction in a different thread
        sync_semaphore done{ 0 };

        // Pointer to flag signalling removal from the same thread
        bool* removed{ nullptr };

        explicit stop_state_callback(invoke_t invoke)
            : invoke(invoke)
        {}
    };

    /**
     * Shared state between objects
     */
    class stop_state {
        friend class stop_source;

        stop_state() noexcept = default;

    public:
        void ref() noexcept {
            refcount.fetch_add(1, std::memory_order_relaxed);
        }

        void unref() noexcept {
            if (1 == refcount.fetch_sub(1, std::memory_order_acq_rel)) {
                delete this;
            }
        }

        void add_source() noexcept {
            state.fetch_add(SourceCountIncrement, std::memory_order_relaxed);
        }

        void remove_source() noexcept {
            state.fetch_sub(SourceCountIncrement, std::memory_order_release);
        }

        /**
         * Adds callback to this state
         *
         * Returns true when added and may be called later
         * Returns false when not added, may or may not run inline
         */
        bool add_callback(stop_state_callback* callback) noexcept {
            // Note: acquire synchronizes with everything before request_stop
            auto current = state.load(std::memory_order_acquire);
            for (int i = 0; ; ++i) {
                if (current & FlagStopped) {
                    // Already stopped, run callback synchronously
                    // We are in the constructor, so no need to signal semaphore
                    callback->invoke(callback);
                    return false;
                }
                if (current < SourceCountIncrement) {
                    // There are no sources and stop can never be requested
                    // We will simply never run the callback
                    return false;
                }
                if (!(current & FlagLocked)) {
                    // Unlocked, try to lock
                    if (state.compare_exchange_weak(current, current | FlagLocked, std::memory_order_acquire)) {
                        break;
                    }
                } else if (!(current & FlagWaiting)) {
                    // Locked, spin first without setting the waiting flag
                    if (i < 64) {
                        current = state.load(std::memory_order_acquire);
                        continue;
                    }
                    // Locked, set the waiting flag
                    if (!state.compare_exchange_weak(current, current | FlagWaiting, std::memory_order_acquire)) {
                        continue;
                    }
                }
                state.wait(current, std::memory_order_acquire);
                current = state.load(std::memory_order_acquire);
            }

            // We add callback at the front, so the order better matches when
            // callback is added to a stopped state, e.g. it runs synchronously
            // as if callback was added at the front and immediately executed.
            // Plus both gcc and clang do it that way.
            if (head) {
                callback->next = head;
                head->prev = callback;
            }
            head = callback;

            unlock();
            return true;
        }

        /**
         * Removes callback from this state
         *
         * May only be called once after add_callback returns true
         */
        void remove_callback(stop_state_callback* callback) {
            lock();
            bool removed = remove_callback_locked(callback);
            unlock();

            if (!removed) {
                // Callback was not in the list, and it must be because it is
                // currently running, or has already completed.
                if (std::this_thread::get_id() == running_thread_id) {
                    // We are in the same thread, signal our removal
                    if (callback->removed) {
                        *callback->removed = true;
                    }
                } else {
                    // We are in a different thread, wait on a semaphore
                    callback->done.acquire();
                }
            }
        }

        /**
         * See `stop_token::stop_requested`
         */
        bool stop_requested() const noexcept {
            return state.load(std::memory_order_acquire) & FlagStopped;
        }

        /**
         * See `stop_token::stop_possible`
         */
        bool stop_possible() const noexcept {
            auto current = state.load(std::memory_order_acquire);
            return (current & FlagStopped) || current >= SourceCountIncrement;
        }

        /**
         * See `stop_source::request_stop`
         */
        bool request_stop() noexcept {
            if (!lock_stop()) {
                return false;
            }

            if (!head) {
                // Don't need to run any callbacks
                unlock();
                return true;
            }

            running_thread_id = std::this_thread::get_id();

            while (auto* callback = head) {
                head = callback->next;
                if (head) {
                    head->prev = nullptr;
                }
                callback->next = nullptr;

                // Note: more callbacks cannot be added if the list is empty
                bool empty = !head;

                bool removed = false;
                callback->removed = &removed;
                unlock();

                callback->invoke(callback);

                // If removed == true callback was removed in the same thread,
                // which happens in the destructor and it's unsafe to use.
                // Otherwise some other thread will wait until done is notified.
                if (!removed) {
                    callback->removed = nullptr;
                    // Note: there's a possible race in release() where it
                    // increments counter and another thread immediately
                    // wakes up, decrements counter and destroys the callback
                    // before release() called notify_one(). That race is benign
                    // however, because notify_one() notifies the address, and
                    // doesn't actually touch any data.
                    callback->done.release();
                }

                if (empty) {
                    // Don't lock if already empty
                    return true;
                }

                lock();
            }

            unlock();
            return true;
        }

    private:
        /**
         * Removes the callback and returns true on success, false if not in the list
         */
        bool remove_callback_locked(stop_state_callback* callback) {
            if (head == callback) {
                head = callback->next;
                if (head) {
                    head->prev = nullptr;
                }
                callback->next = nullptr;
                return true;
            }
            if (callback->prev) {
                callback->prev->next = callback->next;
                if (callback->next) {
                    callback->next->prev = callback->prev;
                    callback->next = nullptr;
                }
                callback->prev = nullptr;
                return true;
            }
            return false;
        }

        /**
         * Locks state for exclusive access
         */
        void lock() noexcept {
            auto current = state.load(std::memory_order_relaxed);
            for (int i = 0; ; ++i) {
                if (!(current & FlagLocked)) {
                    // Unlocked, try to lock
                    if (state.compare_exchange_weak(current, current | FlagLocked, std::memory_order_acquire)) {
                        return;
                    }
                } else if (!(current & FlagWaiting)) {
                    // Locked, spin first without setting the waiting flag
                    if (i < 64) {
                        current = state.load(std::memory_order_acquire);
                        continue;
                    }
                    // Locked, set the waiting flag
                    if (!state.compare_exchange_weak(current, current | FlagWaiting, std::memory_order_relaxed)) {
                        continue;
                    }
                }
                state.wait(current, std::memory_order_relaxed);
                current = state.load(std::memory_order_relaxed);
            }
        }

        /**
         * Locks state for exclusive access and sets the stopped bit
         *
         * Returns false and does not lock when stopped bit is already set.
         */
        bool lock_stop() noexcept {
            auto current = state.load(std::memory_order_acquire);
            for (int i = 0; ; ++i) {
                if (current & FlagStopped) {
                    return false;
                }
                if (!(current & FlagLocked)) {
                    // Unlocked, try to lock and stop
                    if (state.compare_exchange_weak(current, current | FlagLocked | FlagStopped, std::memory_order_acq_rel)) {
                        if (current & FlagWaiting) {
                            // Wake other threads waiting for FlagStopped
                            state.notify_all();
                        }
                        return true;
                    }
                } else if (!(current & FlagWaiting)) {
                    // Locked, spin first without setting the waiting flag
                    if (i < 64) {
                        current = state.load(std::memory_order_acquire);
                        continue;
                    }
                    // Locked, set the waiting flag
                    if (!state.compare_exchange_weak(current, current | FlagWaiting, std::memory_order_acquire)) {
                        continue;
                    }
                }
                state.wait(current, std::memory_order_acquire);
                current = state.load(std::memory_order_acquire);
            }
        }

        /**
         * Unlocks state from exclusive access
         *
         * Wakes up a single thread currently waiting for a lock
         */
        void unlock() noexcept {
            // Note: it is a prerequisite that FlagLocked is actually set
            // and fetch_sub is usually faster than fetch_and.
            auto current = state.fetch_sub(FlagLocked, std::memory_order_release);
            // The FlagWaiting flag is set on contention and never removed.
            if (current & FlagWaiting) {
                // Wake a single thread waiting to lock
                state.notify_one();
            }
        }

    private:
        static constexpr semaphore_atomic_t::value_type FlagStopped = 1;
        static constexpr semaphore_atomic_t::value_type FlagWaiting = 2;
        static constexpr semaphore_atomic_t::value_type FlagLocked = 4;
        static constexpr semaphore_atomic_t::value_type SourceCountIncrement = 8;

    private:
        // We use an intrusive refcount so objects are a single pointer in size
        std::atomic<size_t> refcount;
        // Note: when state is an int (e.g. linux futex) there would be 29 bits
        // left for the sources ref count, which gives us ~500 million possible
        // sources. This should be ok, sources are not copied as often as
        // tokens.
        semaphore_atomic_t state;
        // A linked list of callbacks
        stop_state_callback* head{ nullptr };
        // A thread that initially called request_stop with callbacks
        std::thread::id running_thread_id;
    };

    /**
     * Polyfill for `std::nostopstate_t`
     */
    struct nostopstate_t {
        explicit nostopstate_t() = default;
    };

    /**
     * Polyfill for `std::nostopstate`
     */
    inline constexpr nostopstate_t nostopstate{};

    /**
     * Polyfill for `std::stop_token`
     */
    class stop_token {
        friend class stop_source;

        template<class Callback>
        friend class stop_callback;

        explicit stop_token(stop_state* state)
            : state(state)
        {
            if (state) {
                state->ref();
            }
        }

    public:
        stop_token() noexcept
            : state(nullptr)
        {}

        stop_token(const stop_token& rhs) noexcept
            : state(rhs.state)
        {
            if (state) {
                state->ref();
            }
        }

        stop_token(stop_token&& rhs) noexcept
            : state(rhs.state)
        {
            rhs.state = nullptr;
        }

        ~stop_token() {
            if (state) {
                state->unref();
                state = nullptr;
            }
        }

        stop_token& operator=(const stop_token& rhs) noexcept {
            if (this != &rhs) {
                stop_state* prev = state;
                state = rhs.state;
                if (state) {
                    state->ref();
                }
                if (prev) {
                    prev->unref();
                }
            }
            return *this;
        }

        stop_token& operator=(stop_token&& rhs) noexcept {
            if (this != &rhs) {
                stop_state* prev = state;
                state = rhs.state;
                rhs.state = nullptr;
                if (prev) {
                    prev->unref();
                }
            }
            return *this;
        }

        void swap(stop_token& rhs) noexcept {
            stop_state* tmp = state;
            state = rhs.state;
            rhs.state = tmp;
        }

        bool stop_requested() const noexcept {
            return state && state->stop_requested();
        }

        bool stop_possible() const noexcept {
            return state && state->stop_possible();
        }

        friend bool operator==(const stop_token& a, const stop_token& b) noexcept {
            return a.state == b.state;
        }

        friend void swap(stop_token& a, stop_token& b) noexcept {
            stop_state* tmp = a.state;
            a.state = b.state;
            b.state = tmp;
        }

    private:
        stop_state* state;
    };

    /**
     * Polyfill for `std::stop_source`
     */
    class stop_source {
    public:
        stop_source()
            : state(new stop_state)
        {
            state->ref();
            state->add_source();
        }

        explicit stop_source(nostopstate_t) noexcept
            : state(nullptr)
        {}

        stop_source(const stop_source& rhs) noexcept
            : state(rhs.state)
        {
            if (state) {
                state->ref();
                state->add_source();
            }
        }

        stop_source(stop_source&& rhs) noexcept
            : state(rhs.state)
        {
            rhs.state = nullptr;
        }

        ~stop_source() {
            if (state) {
                state->remove_source();
                state->unref();
                state = nullptr;
            }
        }

        stop_source& operator=(const stop_source& rhs) noexcept {
            if (this != &rhs) {
                stop_state* prev = state;
                state = rhs.state;
                if (state) {
                    state->ref();
                    state->add_source();
                }
                if (prev) {
                    prev->remove_source();
                    prev->unref();
                }
            }
            return *this;
        }

        stop_source& operator=(stop_source&& rhs) noexcept {
            if (this != &rhs) {
                stop_state* prev = state;
                state = rhs.state;
                rhs.state = nullptr;
                if (prev) {
                    prev->remove_source();
                    prev->unref();
                }
            }
            return *this;
        }

        bool request_stop() noexcept {
            return state && state->request_stop();
        }

        void swap(stop_source& rhs) noexcept {
            stop_state* tmp = state;
            state = rhs.state;
            rhs.state = tmp;
        }

        [[nodiscard]] stop_token get_token() const noexcept {
            return stop_token(state);
        }

        bool stop_requested() const noexcept {
            return state && state->stop_requested();
        }

        bool stop_possible() const noexcept {
            return bool(state);
        }

        friend bool operator==(const stop_source& a, const stop_source& b) noexcept {
            return a.state == b.state;
        }

        friend void swap(stop_source& a, stop_source& b) noexcept {
            stop_state* tmp = a.state;
            a.state = b.state;
            b.state = tmp;
        }

    private:
        stop_state* state;
    };

    /**
     * Polyfill for `std::stop_callback`
     */
    template<class Callback>
    class stop_callback {
    public:
        using callback_type = Callback;

        template<class C>
        explicit stop_callback(const stop_token& token, C&& callback)
            noexcept(std::is_nothrow_constructible_v<Callback, C>)
            : callback_(std::forward<C>(callback))
        {
            if (token.state && token.state->add_callback(&callback_)) {
                state = token.state;
                state->ref();
            }
        }

        template<class C>
        explicit stop_callback(stop_token&& token, C&& callback)
            noexcept(std::is_nothrow_constructible_v<Callback, C>)
            : callback_(std::forward<C>(callback))
        {
            if (token.state) {
                if (token.state->add_callback(&callback_)) {
                    state = token.state;
                    token.state = nullptr;
                } else {
                    token.state->unref();
                    token.state = nullptr;
                }
            }
        }

        ~stop_callback() {
            if (state) {
                state->remove_callback(&callback_);
                state->unref();
                state = nullptr;
            }
        }

        stop_callback(const stop_callback&) = delete;
        stop_callback(stop_callback&&) = delete;

        stop_callback& operator=(const stop_callback&) = delete;
        stop_callback& operator=(stop_callback&&) = delete;

    private:
        class callback_t final : public stop_state_callback {
        public:
            template<class C>
            explicit callback_t(C&& callback)
                : stop_state_callback(
                    [](stop_state_callback* self) noexcept {
                        std::forward<Callback>(static_cast<callback_t*>(self)->callback_)();
                    })
                , callback_(std::forward<C>(callback))
            {}

        private:
            Callback callback_;
        };

    private:
        callback_t callback_;
        stop_state* state{ nullptr };
    };

    /**
     * Polyfill for `std::stop_callback`
     */
    template<class Callback>
    stop_callback(stop_token, Callback) -> stop_callback<Callback>;

} // namespace coroactors::detail
