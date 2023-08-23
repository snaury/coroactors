#pragma once
#include <coroactors/detail/result.h>
#include <atomic>
#include <cassert>
#include <coroutine>
#include <memory>
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
     * This class is shared with a shared_ptr
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

        /**
         * Detach is usually called from the task group (which holds a strong
         * reference to the sink), and signals no new results will ever be
         * awaited and may be discarded. However we also call detach from
         * destructor to make sure pending results are deallocated.
        */
        void detach() noexcept {
            // Note: detach is usually called from the task group, and it holds
            // a strong reference to the sink. However we also call detach from
            // destructor and usually it's already detached.
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
                } else {
                    // Destroy current linked list of results
                    std::unique_ptr<task_group_result_node<T>> head(reinterpret_cast<task_group_result_node<T>*>(headValue));
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
                task_group_result_node<T>* head = reinterpret_cast<task_group_result_node<T>*>(headValue);
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
         * We return true when at least one result is already available
         */
        bool await_ready() noexcept {
            return ready_queue || last_ready.load(std::memory_order_relaxed);
        }

        /**
         * Tries to register c as the next result continuation, and returns
         * noop_coroutine on success. If there's a race and new result is
         * discovered will return c to resume immediately.
         */
        std::coroutine_handle<> await_suspend(std::coroutine_handle<> c) noexcept {
            assert(!ready_queue && "Caller suspending with non-empty ready queue");
            continuation = c;
            void* headValue = last_ready.load(std::memory_order_relaxed);
            for (;;) {
                if (!headValue) {
                    // Note: release here synchronizes with acquire in push
                    // It also functions similar to mutex unlock, release exclusive access to another thread
                    if (!last_ready.compare_exchange_weak(headValue, reinterpret_cast<void*>(MarkerAwaiting), std::memory_order_release)) {
                        continue;
                    }
                    // Continuation may already be waking up on another thread
                    return std::noop_coroutine();
                }
                // Lost the race: ready queue is not empty
                assert(headValue != reinterpret_cast<void*>(MarkerAwaiting));
                assert(headValue != reinterpret_cast<void*>(MarkerDetached));
                break;
            }
            continuation = {};
            return c;
        }

        /**
         * Removes a ready result from the queue, which we know exists.
         */
        std::unique_ptr<task_group_result_node<T>> await_resume() noexcept {
            std::unique_ptr<task_group_result_node<T>> result;
            if (!ready_queue) {
                // Note: acquire here synchronizes with release in push
                void* headValue = last_ready.exchange(nullptr, std::memory_order_acquire);
                assert(headValue != reinterpret_cast<void*>(MarkerAwaiting));
                assert(headValue != reinterpret_cast<void*>(MarkerDetached));
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
            return result;
        }

    private:
        // Signals there is a continuation waiting for the first result
        static constexpr uintptr_t MarkerAwaiting = 1;
        // Signals task group is detached and new results will not be consumed
        static constexpr uintptr_t MarkerDetached = 2;

        // A linked list of ready results (last result first) or a marker
        std::atomic<void*> last_ready{ nullptr };
        // A linked list of ready results removed from the atomic head
        std::unique_ptr<task_group_result_node<T>> ready_queue;
        // Continuation waiting for the next result
        std::coroutine_handle<> continuation;
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

        [[nodiscard]] task_group_coroutine<T> get_return_object() noexcept {
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

        void start(const std::shared_ptr<task_group_sink<T>>& sink, size_t index) {
            this->result_->index = index;
            sink_ = sink;
            running = true;
            task_group_handle<T>::from_promise(*this).resume();
        }

    private:
        std::shared_ptr<task_group_sink<T>> sink_;
        bool running = false;
    };

    template<class T>
    class task_group_coroutine {
        friend class task_group_promise<T>;

        task_group_coroutine(task_group_handle<T> handle)
            : handle(handle)
        {}

    public:
        using promise_type = task_group_promise<T>;

        void start(const std::shared_ptr<task_group_sink<T>>& sink, size_t index) {
            handle.promise().start(sink, index);
        }

    private:
        task_group_handle<T> handle;
    };

    template<class T, class Awaitable>
    task_group_coroutine<T> make_task_group_coroutine(Awaitable awaitable) {
        co_return co_await std::move(awaitable);
    }

} // coroactors::detail

namespace coroactors {

    /**
     * Task group allows waiting for multiple awaitables with the same result
     * type T and processing results in the order they complete. Awaitables
     * are started when added and may run concurrently with the owner of the
     * task group. The task group object itself is strictly single-threaded
     * however and must not be shared between coroutines. The task group is
     * destroyed all unfinished tasks are detached and their results will be
     * ignored.
     */
    template<class T>
    class task_group {
    public:
        using result_type = detail::task_group_result<T>;
        using value_type = T;

        task_group() = default;

        task_group(const task_group&) = delete;
        task_group& operator=(const task_group&) = delete;

        task_group(task_group&& rhs)
            : sink_(std::move(rhs.sink_))
            , count_(rhs.count_)
            , left_(rhs.left_)
        {
            rhs.sink_.reset();
            rhs.count_ = 0;
            rhs.left_ = 0;
        }

        ~task_group() {
            if (sink_) {
                sink_->detach();
            }
        }

        /**
         * Adds a new awaitable to the task group and returns its index
         */
        template<class Awaitable>
        size_t add(Awaitable&& awaitable) {
            assert(sink_);
            size_t index = count_++;
            auto coro = detail::make_task_group_coroutine<T>(std::forward<Awaitable>(awaitable));
            coro.start(sink_, index);
            ++left_;
            return index;
        }

        /**
         * Returns the number of started and unawaited tasks
         */
        size_t left() const {
            return left_;
        }

        /**
         * Returns true if task group has at least one unawaited task
         */
        explicit operator bool() const {
            return left_ > 0;
        }

        /**
         * Returns true if there is at least one result that can be awaited without blocking
         */
        bool ready() const {
            assert(sink_);
            return sink_->await_ready();
        }

        class next_awaiter_t {
        public:
            explicit next_awaiter_t(task_group& group) noexcept
                : group(group)
            {}

            bool await_ready() noexcept {
                assert(group.left_ > 0 && "Task group has no tasks to await");
                --group.left_;
                return group.sink_->await_ready();
            }

            __attribute__((__noinline__))
            std::coroutine_handle<> await_suspend(std::coroutine_handle<> c) noexcept {
                return group.sink_->await_suspend(c);
            }

            T await_resume() {
                auto result = group.sink_->await_resume();
                return std::move(*result).take();
            }

        private:
            task_group& group;
        };

        /**
         * Returns the next available result value when awaited
         */
        next_awaiter_t next() noexcept {
            return next_awaiter_t{ *this };
        }

        class next_result_awaiter_t {
        public:
            explicit next_result_awaiter_t(task_group& group) noexcept
                : group(group)
            {}

            bool await_ready() noexcept {
                assert(group.left_ > 0 && "Task group has no tasks to await");
                --group.left_;
                return group.sink_->await_ready();
            }

            __attribute__((__noinline__))
            std::coroutine_handle<> await_suspend(std::coroutine_handle<> c) noexcept {
                return group.sink_->await_suspend(c);
            }

            result_type await_resume() {
                auto result = group.sink_->await_resume();
                return std::move(*result);
            }

        private:
            task_group& group;
        };

        /**
         * Returns the next available result wrapper when awaited
         */
        next_result_awaiter_t next_result() noexcept {
            return next_result_awaiter_t{ *this };
        }

    private:
        std::shared_ptr<detail::task_group_sink<T>> sink_ = std::make_shared<detail::task_group_sink<T>>();
        size_t count_ = 0;
        size_t left_ = 0;
    };

} // namespace coroactors
