#pragma once
#include <coroactors/detail/awaiters.h>
#include <coroactors/detail/intrusive_ptr.h>
#include <coroactors/detail/result.h>
#include <coroactors/stop_token.h>
#include <atomic>
#include <cassert>
#include <coroutine>
#include <memory>
#include <optional>
#include <utility>

namespace coroactors::detail {

    /**
     * Encapsulates a single result in a task group
     */
    template<class T>
    class task_group_result : public result<T> {
    public:
        task_group_result() noexcept = default;

    public:
        size_t index{ size_t(-1) };
    };

    template<class T>
    class task_group_sink;

    /**
     * A node in a linked list of result nodes
     */
    template<class T>
    class task_group_result_node : public task_group_result<T> {
        friend class task_group_sink<T>;

    public:
        task_group_result_node() noexcept = default;

        task_group_result_node(const task_group_result_node&) = delete;
        task_group_result_node& operator=(const task_group_result_node&) = delete;

        ~task_group_result_node() noexcept {
            // Destroy all linked elements while avoiding recursion
            auto* head = std::exchange(next, nullptr);
            while (head) {
                std::unique_ptr<task_group_result_node<T>> current(head);
                head = std::exchange(head->next, nullptr);
            }
        }

    private:
        task_group_result_node<T>* next{ nullptr };
    };

    /**
     * A sink where ready results are pushed and awaited
     *
     * This class is shared with a intrusive_ptr
     */
    template<class T>
    class task_group_sink {
    public:
        task_group_sink() noexcept = default;

        task_group_sink(const task_group_sink&) = delete;
        task_group_sink& operator=(const task_group_sink&) = delete;

        ~task_group_sink() noexcept {
            detach();
        }

        void add_ref() noexcept {
            refcount.fetch_add(1, std::memory_order_relaxed);
        }

        size_t release_ref() noexcept {
            return refcount.fetch_sub(1, std::memory_order_acq_rel) - 1;
        }

        /**
         * Detach is usually called from the task group (which holds a strong
         * reference to the sink), and signals no new results will ever be
         * awaited and may be discarded. However we also call detach from
         * destructor to make sure pending results are deallocated.
        */
        void detach() noexcept {
            void* headValue = last_ready.exchange(reinterpret_cast<void*>(MarkerDetached), std::memory_order_acq_rel);
            if (headValue) {
                if (headValue == reinterpret_cast<void*>(MarkerDetached)) {
                    // Task group already detached, normal for destructor
                } else if (headValue == reinterpret_cast<void*>(MarkerAwaiting)) {
                    // Task group destroyed while awaiting, but the awaiting
                    // continuation is supposed to have a strong reference to
                    // task sink, so it means the parent coroutine was likely
                    // destroyed. This is unsafe in multi-threaded environment,
                    // because it could lead to a race between destroy and
                    // resume being called concurrently, but here it means we're
                    // lucky and may just ignore it.
                    continuation = {};
                } else if (headValue == reinterpret_cast<void*>(MarkerCancelled)) {
                    // A leftover cancellation marker, probably never happens
                } else {
                    // Destroy current linked list of results
                    std::unique_ptr<task_group_result_node<T>> head(
                        reinterpret_cast<task_group_result_node<T>*>(headValue));
                }
            }
            // Eagerly destroy ready queue to free unnecessary memory
            ready_queue.reset();
        }

        /**
         * Pushes a new result to the sink, returns an optional continuation to
         * run next (e.g. an awaiter continuation installed before)
         */
        std::coroutine_handle<> push(std::unique_ptr<task_group_result_node<T>>&& result) noexcept {
            // Note: we don't need acquire here to synchronize with another
            // push. This is because we don't touch anything stored inside that
            // pointer ourselves, and publishing a new head is part of a
            // release sequence.
            void* headValue = last_ready.load(std::memory_order_relaxed);
            for (;;) {
                if (headValue == reinterpret_cast<void*>(MarkerAwaiting)) {
                    // Try to lock current awaiter
                    // Note: acquire here synchronizes with release in await_suspend
                    // It also functions similar to a mutex lock, allowing us exclusive access to continuation
                    if (!last_ready.compare_exchange_weak(headValue, nullptr, std::memory_order_acquire)) {
                        continue;
                    }
                    // Awaiting is single threaded, so the queue is effectively
                    // locked until continuation is resumed. We may access
                    // ready queue and continuation now.
                    assert(ready_queue == nullptr && "Task group is awaiting with non-empty ready queue");
                    result->next = ready_queue.release();
                    ready_queue = std::move(result);
                    return std::exchange(continuation, {});
                }
                if (headValue == reinterpret_cast<void*>(MarkerDetached)) {
                    // Task group is detached, discard all results
                    break;
                }
                // Note: MarkerCancelled is just a flag, the real head is nullptr
                task_group_result_node<T>* head =
                    headValue != reinterpret_cast<void*>(MarkerCancelled)
                        ? reinterpret_cast<task_group_result_node<T>*>(headValue)
                        : nullptr;
                result->next = head;
                void* nextValue = result.get();
                // Note: release here synchronizes with acquire in await_resume
                if (last_ready.compare_exchange_strong(headValue, nextValue, std::memory_order_release)) {
                    // Release successfully added result
                    result.release();
                    break;
                }
                result->next = nullptr;
            }
            return std::noop_coroutine();
        }

        /**
         * Returns true when at least one result is already available
         *
         * Note it is allowed to call ready() while another coroutine is
         * awaiting on a task group. For await_ready() it is not.
         */
        bool ready() const noexcept {
            if (ready_queue) {
                return true;
            }

            void* headValue = last_ready.load(std::memory_order_relaxed);
            if (headValue) {
                // Not ready: another coroutine is awaiting
                if (headValue == reinterpret_cast<void*>(MarkerAwaiting)) [[unlikely]] {
                    return false;
                }
                // These indicate a data race
                assert(headValue != reinterpret_cast<void*>(MarkerDetached));
                assert(headValue != reinterpret_cast<void*>(MarkerCancelled));
                return true;
            }

            // Currently empty
            return false;
        }

        /**
         * Returns true when at least one result is already available
         */
        bool await_ready() const {
            if (continuation) [[unlikely]] {
                // Note: this only protects against synchronized coroutines
                // calling awaiting methods at the same time. It does not
                // prevent data races.
                throw std::logic_error("task group cannot be awaited by multiple coroutines");
            }

            if (ready_queue) {
                return true;
            }

            void* headValue = last_ready.load(std::memory_order_relaxed);
            if (headValue) {
                // These indicate a data race
                assert(headValue != reinterpret_cast<void*>(MarkerDetached));
                assert(headValue != reinterpret_cast<void*>(MarkerAwaiting));
                assert(headValue != reinterpret_cast<void*>(MarkerCancelled));
                return true;
            }

            // Currently empty
            return false;
        }

        /**
         * Tries to register c as the next result continuation, and returns
         * true on success. If there's a race and new result or a concurrent
         * cancellation is discovered it will return false so caller may
         * resume immediately.
         */
        bool await_suspend(std::coroutine_handle<> c) noexcept {
            assert(!ready_queue && "Caller suspending with non-empty ready queue");
            assert(!continuation && "Caller suspending with another continuation");
            continuation = c;
            void* headValue = last_ready.load(std::memory_order_relaxed);
            for (;;) {
                if (!headValue) {
                    // Note: release here synchronizes with acquire in push
                    // It also functions similar to mutex unlock, release exclusive access to another thread
                    if (!last_ready.compare_exchange_weak(headValue, reinterpret_cast<void*>(MarkerAwaiting), std::memory_order_release)) {
                        continue;
                    }
                    // Continuation may already be waking up in another thread
                    return true;
                }
                if (headValue == reinterpret_cast<void*>(MarkerCancelled)) {
                    // An awaiter has concurrently cancelled itself
                    // Note: acquire here synchronizes with release in await_cancel
                    if (!last_ready.compare_exchange_weak(headValue, nullptr, std::memory_order_acquire)) {
                        continue;
                    }
                    // We removed a cancellation flag, resume now
                    break;
                }
                // Lost the race: ready queue is not empty
                assert(headValue != reinterpret_cast<void*>(MarkerAwaiting));
                assert(headValue != reinterpret_cast<void*>(MarkerDetached));
                break;
            }
            continuation = {};
            return false;
        }

        /**
         * Tries to cancel a currently pending await and returns the removed
         * continuation on success.
         *
         * When concurrent is true will try to set a concurrent cancellation
         * flag on an empty result queue (no awaiter yet), so a suspend attempt
         * is properly cancelled after a race. When concurrent is false (the
         * default) any already existing cancellation flag is also cleaned up
         * instead.
         */
        std::coroutine_handle<> await_cancel(bool concurrent = false) noexcept {
            void* headValue = last_ready.load(std::memory_order_relaxed);
            for (;;) {
                if (headValue == reinterpret_cast<void*>(MarkerAwaiting)) {
                    // Remove current awaiter and return it on success
                    if (!last_ready.compare_exchange_weak(headValue, nullptr, std::memory_order_acquire)) {
                        continue;
                    }
                    return std::exchange(continuation, {});
                }
                if (headValue == reinterpret_cast<void*>(MarkerCancelled) && !concurrent) {
                    // Remove an existing cancellation flag and return
                    if (!last_ready.compare_exchange_weak(headValue, nullptr, std::memory_order_acquire)) {
                        continue;
                    }
                    break;
                }
                if (!headValue && concurrent) {
                    // Mark current head as concurrently cancelled and return
                    if (!last_ready.compare_exchange_weak(headValue, reinterpret_cast<void*>(MarkerCancelled), std::memory_order_release)) {
                        continue;
                    }
                    break;
                }
                assert(headValue != reinterpret_cast<void*>(MarkerDetached));
                break;
            }
            return {};
        }

        /**
         * Removes a ready result from the queue, which we know exists.
         */
        std::unique_ptr<task_group_result_node<T>> await_resume() noexcept {
            std::unique_ptr<task_group_result_node<T>> result;
            if (!ready_queue) {
                // Note: acquire here synchronizes with release in push
                void* headValue = last_ready.exchange(nullptr, std::memory_order_acquire);
                // These indicate a data race
                assert(headValue != reinterpret_cast<void*>(MarkerAwaiting));
                assert(headValue != reinterpret_cast<void*>(MarkerDetached));
                assert(headValue != reinterpret_cast<void*>(MarkerCancelled));
                task_group_result_node<T>* head = reinterpret_cast<task_group_result_node<T>*>(headValue);
                assert(head && "Task group is resuming with an empty queue");
                while (head) {
                    // We invert the linked list here
                    auto* next = std::exchange(head->next, nullptr);
                    head->next = ready_queue.release();
                    ready_queue.reset(head);
                    head = next;
                }
            }
            assert(ready_queue && "Resumed without any results ready");
            auto* next = std::exchange(ready_queue->next, nullptr);
            result = std::move(ready_queue);
            ready_queue.reset(next);
            --count_;
            return result;
        }

        /**
         * Returns the number of tasks that have not been awaited yet
         */
        size_t count() const noexcept {
            return count_;
        }

        /**
         * Returns index for the next added task
         */
        size_t next_index() noexcept {
            ++count_;
            return next_index_++;
        }

    private:
        // Signals there is a continuation waiting for the first result
        static constexpr uintptr_t MarkerAwaiting = 1;
        // Signals task group is detached and new results will not be consumed
        static constexpr uintptr_t MarkerDetached = 2;
        // Signals the next await_suspend that the request is already cancelled
        static constexpr uintptr_t MarkerCancelled = 3;

        // A reference count for intrusive_ptr
        std::atomic<size_t> refcount{ 0 };
        // A linked list of ready results (last result first) or a marker
        std::atomic<void*> last_ready{ nullptr };
        // A linked list of ready results removed from the atomic head
        std::unique_ptr<task_group_result_node<T>> ready_queue;
        // Continuation waiting for the next result
        std::coroutine_handle<> continuation;

        // The number of tasks that have not been awaited yet
        size_t count_{ 0 };
        // The number of tasks added to the group, also the next index
        size_t next_index_{ 0 };
    };

    template<class T>
    class task_group_when_ready_awaiter {
    public:
        explicit task_group_when_ready_awaiter(task_group_sink<T>* sink) noexcept
            : sink(sink)
        {}

        task_group_when_ready_awaiter(const task_group_when_ready_awaiter&) = delete;
        task_group_when_ready_awaiter& operator=(const task_group_when_ready_awaiter&) = delete;

        task_group_when_ready_awaiter(task_group_when_ready_awaiter&& rhs) noexcept
            : sink(rhs.sink)
        {}

        ~task_group_when_ready_awaiter() noexcept {
            if (cancel) {
                // Make sure to clean up the callback first
                cancel.reset();
            }

            if (suspended) {
                // Support bottom-up destruction (awaiter destroyed before it
                // was resumed). It is up to user to ensure there are no
                // concurrent resume attempts.
                sink->await_cancel();
            }
        }

        bool await_ready(stop_token token = {}) {
            if (sink->count() == 0) {
                throw std::out_of_range("task group has no tasks to await");
            }

            if (sink->await_ready()) {
                return true;
            }

            // Setup cancellation forwarding when needed
            if (token.stop_possible()) {
                if (token.stop_requested()) {
                    // Don't suspend when already cancelled
                    return true;
                }

                // It's ok when cancellation races and callback runs here
                cancel.emplace(std::move(token), sink);
            }

            return false;
        }

        __attribute__((__noinline__))
        bool await_suspend(std::coroutine_handle<> h) noexcept {
            suspended = true;
            // Note: there are two possible pathways for an empty queue here:
            // 1) The cancellation callback runs first, there is no awaiter or
            //    a result yet, it sets the cancellation flag, which we consume
            //    while trying to install a continuation. Post coditions: head
            //    is either empty (cancel consumed, and will not run again) or
            //    awaiting.
            // 2) We install our continuation first, then the cancellation
            //    callback runs, consumes the awaiting flag and resumes us.
            //    Post conditions: head is empty, we are resumed.
            // In all cases the result may be pushed to the queue, in which
            // case cancellation flag will be dropped and ignored (removed
            // when already set, will never be added to a non-empty queue), we
            // are resumed one way or another, post condition is head having a
            // result (ready). This result cannot be consumed by a concurrent
            // coroutine unless there is a data race, so cancellation will do
            // nothing because the head is not empty.
            //
            // In short, our invariants:
            // 1) We resume with an empty head only when cancellation happend.
            // 2) We resume with a non-empty head only when there's a result.
            // 3) The cancellation flag is never set when we resume.
            return sink->await_suspend(h);
        }

        bool await_resume() noexcept {
            suspended = false;

            if (cancel) {
                // Remove the cancellation callback, it either happend already,
                // or we synchronize with it finishing (doing nothing). Note
                // we don't need to call await_cancel here, we are not awaiting.
                cancel.reset();
            }

            // We call await_ready and not ready because current coroutine is
            // still awaiting and it double checks for possible data races.
            return sink->await_ready();
        }

    private:
        struct cancel_t {
            task_group_sink<T>* sink;

            explicit cancel_t(task_group_sink<T>* sink)
                : sink(sink)
            {}

            void operator()() noexcept {
                if (auto h = sink->await_cancel(true)) {
                    h.resume();
                }
            }
        };

    private:
        task_group_sink<T>* sink;
        std::optional<stop_callback<cancel_t>> cancel;
        bool suspended = false;
    };

    template<class T>
    class task_group_result_handler_base {
    public:
        void unhandled_exception() noexcept {
            result_->set_exception(std::current_exception());
        }

    protected:
        std::unique_ptr<task_group_result_node<T>> result_ = std::make_unique<task_group_result_node<T>>();
    };

    template<class T>
    class task_group_result_handler : public task_group_result_handler_base<T> {
    public:
        template<class Value>
        void return_value(Value&& value)
            requires (std::is_convertible_v<Value&&, T>)
        {
            this->result_->set_value(std::forward<Value>(value));
        }
    };

    template<>
    class task_group_result_handler<void> : public task_group_result_handler_base<void> {
    public:
        void return_void() noexcept {
            this->result_->set_value();
        }
    };

    template<class T>
    class task_group_coroutine;

    template<class T>
    class task_group_promise;

    template<class T>
    using task_group_handle = std::coroutine_handle<task_group_promise<T>>;

    template<class T>
    class task_group_promise : public task_group_result_handler<T> {
    public:
        ~task_group_promise() {
            if (running) {
                // Coroutine was destroyed before it could finish. This could
                // happen when e.g. it was suspended and caller decided to
                // destroy it instead of resuming, unwinding frames back to
                // us. Make sure we wake up awaiter with an empty result.
                if (auto next = sink_->push(std::move(this->result_))) {
                    next.resume();
                }
            }
        }

        task_group_coroutine<T> get_return_object() noexcept {
            return task_group_coroutine<T>(task_group_handle<T>::from_promise(*this));
        }

        auto initial_suspend() noexcept { return std::suspend_always(); }

        struct final_suspend_t {
            bool await_ready() noexcept { return false; }

            __attribute__((__noinline__))
            std::coroutine_handle<> await_suspend(task_group_handle<T> h) noexcept {
                auto& self = h.promise();
                self.running = false;
                auto sink = std::move(self.sink_);
                auto next = sink->push(std::move(self.result_));
                h.destroy();
                return next;
            }

            void await_resume() noexcept {}
        };

        auto final_suspend() noexcept { return final_suspend_t{}; }

        size_t start(const intrusive_ptr<task_group_sink<T>>& sink, stop_token&& token) {
            size_t index = sink->next_index();
            sink_ = sink;
            token_ = std::move(token);
            running = true;
            this->result_->index = index;
            task_group_handle<T>::from_promise(*this).resume();
            return index;
        }

        template<awaitable_with_stop_token_propagation Awaitable>
        class pass_stop_token_awaiter {
            using Awaiter = awaiter_transform_type_t<Awaitable>;

        public:
            pass_stop_token_awaiter(Awaitable&& awaitable, task_group_promise& self)
                : awaiter(get_awaiter(std::forward<Awaitable>(awaitable)))
                , self(self)
            {}

            pass_stop_token_awaiter(const pass_stop_token_awaiter&) = delete;
            pass_stop_token_awaiter& operator=(const pass_stop_token_awaiter&) = delete;

            bool await_ready()
                noexcept(has_noexcept_await_ready_stop_token<Awaiter>)
            {
                // Note: our coroutine awaits exactly once, so token is moved
                return awaiter.await_ready(std::move(self.token_));
            }

            template<class Promise>
            __attribute__((__noinline__))
            decltype(auto) await_suspend(std::coroutine_handle<Promise> c)
                noexcept(has_noexcept_await_suspend<Awaiter, Promise>)
                requires has_await_suspend<Awaiter, Promise>
            {
                return awaiter.await_suspend(c);
            }

            decltype(auto) await_resume()
                noexcept(has_noexcept_await_resume<Awaiter>)
            {
                return awaiter.await_resume();
            }

        private:
            Awaiter awaiter;
            task_group_promise& self;
        };

        template<awaitable_with_stop_token_propagation Awaitable>
        auto await_transform(Awaitable&& awaitable) noexcept {
            return pass_stop_token_awaiter<Awaitable>(std::forward<Awaitable>(awaitable), *this);
        }

        template<awaitable Awaitable>
        Awaitable&& await_transform(Awaitable&& awaitable) noexcept {
            return std::forward<Awaitable>(awaitable);
        }

    private:
        intrusive_ptr<task_group_sink<T>> sink_;
        stop_token token_;
        bool running = false;
    };

    template<class T>
    class [[nodiscard]] task_group_coroutine {
        friend class task_group_promise<T>;

        task_group_coroutine(task_group_handle<T> handle)
            : handle(handle)
        {}

    public:
        using promise_type = task_group_promise<T>;

        size_t start(const intrusive_ptr<task_group_sink<T>>& sink, stop_token&& token) {
            return handle.promise().start(sink, std::move(token));
        }

    private:
        task_group_handle<T> handle;
    };

    template<class T, class Awaitable>
    task_group_coroutine<T> make_task_group_coroutine(Awaitable awaitable) {
        co_return co_await std::move(awaitable);
    }

} // coroactors::detail
