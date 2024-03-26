#pragma once
#include <coroactors/detail/config.h>
#include <coroactors/detail/task_group.h>

namespace coroactors {

    /**
     * Task group allows waiting for multiple awaitables with the same result
     * type T and awaiting for results in the order they complete. Awaitables
     * are started when added and may run concurrently with the owner of the
     * task group. Methods in the task group, unless otherwise specified, are
     * not thread-safe, cannot be called from multiple threads simultaneously,
     * and only one coroutine may await on the task group at any one time.
     *
     * When the task group is destroyed all unfinished tasks are detached,
     * cancelled, and their results (even exceptions) will be ignored.
     */
    template<class T>
    class task_group {
        using sink_type = detail::task_group_sink<T>;
        using sink_ptr = intrusive_ptr<sink_type>;

    public:
        using result_type = task_group_result<T>;
        using value_type = T;

        task_group() = default;

        task_group(const task_group&) = delete;
        task_group& operator=(const task_group&) = delete;

        task_group(task_group&& rhs)
            : sink_(std::move(rhs.sink_))
            , source_(std::move(rhs.source_))
        {
            rhs.sink_.reset();
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
            auto coro = detail::make_task_group_coroutine<T>(std::forward<Awaitable>(awaitable));
            return coro.start(sink_, source_.get_token(), inherited_locals_);
        }

        /**
         * Returns the number that have been started but not awaited yet
         */
        size_t count() const {
            assert(sink_);
            return sink_->count();
        }

        /**
         * Returns true when task group has at least one unawaited task
         */
        explicit operator bool() const {
            assert(sink_);
            return sink_->count() > 0;
        }

        /**
         * Returns true when there is at least one result which can be awaited without blocking
         */
        bool ready() const {
            assert(sink_);
            return sink_->ready();
        }

        /**
         * Returns when ready() starts to return true when awaited, or when
         * then caller (not the task group) is cancelled.
         *
         * Returns the result of calling ready(), i.e. it would return false
         * when this call returns because is was cancelled.
         */
        detail::task_group_when_ready_awaiter<T> when_ready() const {
            assert(sink_);
            return detail::task_group_when_ready_awaiter<T>{ sink_.get() };
        }

        /**
         * Implementation of task_group<T>::next()
         */
        class [[nodiscard]] next_awaiter_t {
        public:
            explicit next_awaiter_t(sink_type* sink) noexcept
                : sink(sink)
            {}

            ~next_awaiter_t() noexcept {
                if (suspended) {
                    // Support for bottom-up destruction (awaiter destroyed
                    // before it was resumed). It is up to user to ensure there
                    // are no concurrent resume attempts.
                    sink->await_cancel();
                }
            }

            bool await_ready() {
                if (sink->count() == 0) {
                    throw task_group_error("task group has no tasks to await");
                }
                return sink->await_ready();
            }

            COROACTORS_AWAIT_SUSPEND
            bool await_suspend(std::coroutine_handle<> c) noexcept {
                suspended = true;
                return sink->await_suspend(c);
            }

            T await_resume() {
                suspended = false;
                auto result = sink->await_resume();
                return std::move(*result).take_value();
            }

        private:
            sink_type* sink;
            bool suspended = false;
        };

        /**
         * Returns the next available result value when awaited
         *
         * Note: this call cannot be cancelled after it starts awaiting, and
         * will only return when at least one task finishes and its result can
         * be consumed. Use `when_ready()` for awaiting with cancellation.
         */
        next_awaiter_t next() {
            assert(sink_);
            return next_awaiter_t{ sink_.get() };
        }

        /**
         * Implementation of task_group<T>::next_result()
         */
        class [[nodiscard]] next_result_awaiter_t {
        public:
            explicit next_result_awaiter_t(sink_type* sink) noexcept
                : sink(sink)
            {}

            ~next_result_awaiter_t() noexcept {
                if (suspended) {
                    // Support for bottom-up destruction (awaiter destroyed
                    // before it was resumed). It is up to user to ensure there
                    // are no concurrent resume attempts.
                    sink->await_cancel();
                }
            }

            bool await_ready() {
                if (sink->count() == 0) {
                    throw task_group_error("task group has no tasks to await");
                }
                return sink->await_ready();
            }

            COROACTORS_AWAIT_SUSPEND
            bool await_suspend(std::coroutine_handle<> c) noexcept {
                suspended = true;
                return sink->await_suspend(c);
            }

            result_type await_resume() {
                suspended = false;
                auto result = sink->await_resume();
                return std::move(*result);
            }

        private:
            sink_type* sink;
            bool suspended = false;
        };

        /**
         * Returns the next available result wrapper when awaited
         *
         * Note: this call cannot be cancelled after it starts awaiting, and
         * will only return when at least one task finishes and its result can
         * be consumed. Use `when_ready()` for awaiting with cancellation.
         */
        next_result_awaiter_t next_result() {
            return next_result_awaiter_t{ sink_.get() };
        }

        /**
         * Returns a stop token associated with this task group
         *
         * This method is thread-safe and can be called by any thread.
         */
        stop_token get_stop_token() const noexcept {
            return source_.get_token();
        }

        /**
         * Requests all added tasks to stop
         *
         * This method is thread-safe and can be called by any thread.
         */
        void request_stop() noexcept {
            source_.request_stop();
        }

        /**
         * Used by with_task_group to pass locals to pass coroutine locals to
         * this group's task. Caller must guarantee that this record will
         * remain valid for the lifetime of this task group and all tasks
         * running in this task group.
         */
        void set_inherited_locals(const detail::async_task_local* record) noexcept {
            inherited_locals_ = record;
        }

    private:
        sink_ptr sink_{ new sink_type };
        scoped_stop_source source_;
        const detail::async_task_local* inherited_locals_{ nullptr };
    };

} // namespace coroactors
