#pragma once
#include <coroactors/detail/task_group.h>

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
            , source_(std::move(rhs.source_))
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
                source_.request_stop();
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
            coro.start(sink_, source_.get_token(), index);
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

        /**
         * Implementation of task_group<T>::next()
         */
        class [[nodiscard]] next_awaiter_t {
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

        /**
         * Implementation of task_group<T>::next_result()
         */
        class [[nodiscard]] next_result_awaiter_t {
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

        /**
         * Requests all added tasks to stop
         */
        void request_stop() noexcept {
            source_.request_stop();
        }

    private:
        detail::intrusive_ptr<detail::task_group_sink<T>> sink_{ new detail::task_group_sink<T> };
        stop_source source_;
        size_t count_ = 0;
        size_t left_ = 0;
    };

} // namespace coroactors
